/*
 * laced - Lace Database Daemon
 * Async query execution implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "async.h"
#include "db/db.h"
#include "json.h"
#include "log.h"
#include "ssh_tunnel.h"
#include "../../liblace/include/error.h"
#include <util/mem.h>
#include <util/str.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Get current time in milliseconds since epoch */
static uint64_t get_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* ==========================================================================
 * Async Query Structure
 * ========================================================================== */

/* Forward declaration for socket client */

struct AsyncQuery {
  AsyncQueryType type;
  _Atomic AsyncQueryStatus status; /* Workers write it, cancel reads it */

  /* Unique identifier */
  int64_t query_id;
  uint64_t started_at_ms;

  /* Input parameters */
  LacedSession *session;
  int conn_id;
  char *sql;

  /* Request tracking */
  cJSON *request_id;

  /* Result (set by worker thread) */
  cJSON *result;
  char *reply_line;  /* The finished reply (serialized on the worker) */
  char *error;
  int error_code;
  int tx_state;  /* After a statement: 0 none, 1 open, 2 failed; -1 unknown */

  /* Threading */
  pthread_t thread;
  _Atomic bool cancel_requested;
  _Atomic bool began; /* Owns its connection (passed begin_query) */

  /* Queue linkage */
  AsyncQuery *next;
  AsyncQueue *queue;

  /* Client tracking (for socket mode) */
  uint64_t client_serial;  /* Owning socket client (0 = stdio or internal) */
  char owner[LACED_OWNER_MAX];          /* Its owner key ("" = internal, e.g. the reaper) */

  /* Generic job (connect, disconnect, cancel): runs instead of SQL */
  AsyncJobFn job_fn;
  void *job_arg;
  void (*job_arg_free)(void *arg);
};

/* ==========================================================================
 * Async Queue Structure
 * ========================================================================== */

struct AsyncQueue {
  pthread_mutex_t mutex;
  AsyncQuery *head;
  AsyncQuery *tail;

  /* Notification pipe - write to signal, read in select() */
  int notify_pipe[2];

  /* Active queries (for cancellation lookup) */
  AsyncQuery *active_head;

  /* Signalled whenever a worker finishes; shutdown waits on it */
  pthread_cond_t idle_cond;

  /* Query ID counter */
  int64_t next_query_id;

  /* The client whose request is being dispatched (main thread only): jobs
   * started meanwhile belong to it */
};

/* ==========================================================================
 * Queue Implementation
 * ========================================================================== */

AsyncQueue *async_queue_create(int *notify_fd) {
  AsyncQueue *queue = safe_calloc(1, sizeof(AsyncQueue));
  if (!queue) {
    return NULL;
  }

  if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
    free(queue);
    return NULL;
  }

  if (pthread_cond_init(&queue->idle_cond, NULL) != 0) {
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
    return NULL;
  }

  if (pipe(queue->notify_pipe) < 0) {
    pthread_cond_destroy(&queue->idle_cond);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
    return NULL;
  }

  /* Set notify pipe to non-blocking to prevent deadlock in drain_notify */
  int flags = fcntl(queue->notify_pipe[0], F_GETFL, 0);
  if (flags != -1) {
    fcntl(queue->notify_pipe[0], F_SETFL, flags | O_NONBLOCK);
  }
  flags = fcntl(queue->notify_pipe[1], F_GETFL, 0);
  if (flags != -1) {
    fcntl(queue->notify_pipe[1], F_SETFL, flags | O_NONBLOCK);
  }

  if (notify_fd) {
    *notify_fd = queue->notify_pipe[0];
  }

  queue->next_query_id = 1;

  return queue;
}

void async_queue_destroy(AsyncQueue *queue) {
  if (!queue) {
    return;
  }

  /* Free any pending queries */
  pthread_mutex_lock(&queue->mutex);
  AsyncQuery *q = queue->head;
  while (q) {
    AsyncQuery *next = q->next;
    async_query_free(q);
    q = next;
  }
  pthread_mutex_unlock(&queue->mutex);

  close(queue->notify_pipe[0]);
  close(queue->notify_pipe[1]);
  pthread_cond_destroy(&queue->idle_cond);
  pthread_mutex_destroy(&queue->mutex);
  free(queue);
}

bool async_queue_shutdown(AsyncQueue *queue, LacedSession *session,
                          int timeout_ms) {
  if (!queue) {
    return true;
  }

  ssh_tunnel_abort_pending(); /* Connects waiting for a tunnel give up */

  /* Ask every running query to stop, then cancel them in the database.
   * The conn_ids are collected first: the driver cancel may block, and
   * must not run under the queue lock the workers need to finish. */
  int conn_ids[256];
  size_t n = 0;
  pthread_mutex_lock(&queue->mutex);
  for (AsyncQuery *q = queue->active_head; q; q = q->next) {
    q->cancel_requested = true;
    if (!q->job_fn && n < sizeof(conn_ids) / sizeof(conn_ids[0])) {
      conn_ids[n++] = q->conn_id;
    }
  }
  pthread_mutex_unlock(&queue->mutex);
  for (size_t i = 0; i < n; i++) {
    char *err = NULL;
    laced_session_cancel_query(session, conn_ids[i], 0, &err);
    free(err);
  }

  /* Wait for the workers: they use the session's connections and this
   * queue until they push their result */
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&queue->mutex);
  while (queue->active_head) {
    if (pthread_cond_timedwait(&queue->idle_cond, &queue->mutex, &deadline) != 0) {
      break; /* Timed out */
    }
  }
  bool idle = queue->active_head == NULL;
  pthread_mutex_unlock(&queue->mutex);
  return idle;
}

/* Push completed query to queue (called by worker thread) */
/* On the worker: turn a result into its reply line, so the event loop only
 * copies bytes (printing a large result took seconds there, stalling every
 * other client). A reply that no client output queue could hold becomes an
 * error instead of dropping the client. */
static void serialize_reply(AsyncQuery *query) {
  if (query->status != ASYNC_QUERY_COMPLETED || !query->result ||
      !query->request_id)
    return;
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(query->request_id, true));
  cJSON_AddItemToObject(resp, "result", query->result);
  query->result = NULL;
  char *line = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  /* The same bound laced_client_write_line applies, newline included */
  if (!line || strlen(line) + 1 > LACED_MAX_PENDING_OUTPUT) {
    free(line);
    query->status = ASYNC_QUERY_ERROR;
    query->error_code = LACE_ERR_RESULT_TOO_LARGE;
    query->error = str_dup("The result is too large to send; select fewer "
                           "rows or columns");
    return;
  }
  query->reply_line = line;
}

static void async_queue_push(AsyncQueue *queue, AsyncQuery *query) {
  serialize_reply(query);
  pthread_mutex_lock(&queue->mutex);

  /* Remove from active list */
  AsyncQuery **pp = &queue->active_head;
  while (*pp && *pp != query) {
    pp = &(*pp)->next;
  }
  if (*pp) {
    *pp = query->next;
  }

  /* Add to completion queue */
  query->next = NULL;
  if (queue->tail) {
    queue->tail->next = query;
  } else {
    queue->head = query;
  }
  queue->tail = query;

  /* Signal the main loop. Still under the lock: once it is released, an
   * idle queue may be destroyed by shutdown (the pipe is non-blocking) */
  char c = 1;
  (void)!write(queue->notify_pipe[1], &c, 1);

  if (!queue->active_head) {
    pthread_cond_broadcast(&queue->idle_cond);
  }
  pthread_mutex_unlock(&queue->mutex);
}

AsyncQuery *async_queue_pop(AsyncQueue *queue) {
  if (!queue) {
    return NULL;
  }

  pthread_mutex_lock(&queue->mutex);

  AsyncQuery *query = queue->head;
  if (query) {
    queue->head = query->next;
    if (!queue->head) {
      queue->tail = NULL;
    }
    query->next = NULL;
  }

  pthread_mutex_unlock(&queue->mutex);
  return query;
}

void async_queue_drain_notify(AsyncQueue *queue) {
  if (!queue) {
    return;
  }

  char buf[64];
  while (read(queue->notify_pipe[0], buf, sizeof(buf)) > 0) {
    /* Drain all pending notifications */
  }
}

/* ==========================================================================
 * Worker Thread Functions
 * ========================================================================== */

static void *query_worker(void *arg) {
  AsyncQuery *query = (AsyncQuery *)arg;
  uint64_t start_time = get_time_ms();

  if (query->job_fn) {
    char *jerr = NULL;
    int jcode = LACE_ERR_INTERNAL_ERROR;
    cJSON *jres = query->job_fn(query->session, query->job_arg, &jerr, &jcode);
    if (jres) {
      query->result = jres;
      query->status = ASYNC_QUERY_COMPLETED;
      free(jerr);
    } else {
      query->status = ASYNC_QUERY_ERROR;
      query->error = jerr ? jerr : str_dup("Operation failed");
      query->error_code = jcode;
    }
    async_queue_push(query->queue, query);
    return NULL;
  }

  LOG_DEBUG("Query %lld starting (conn_id=%d, type=%d)",
            (long long)query->query_id, query->conn_id, query->type);

  /* One statement per connection: fail if this one is busy */
  bool busy = false;
  DbConnection *conn =
      laced_session_begin_query(query->session, query->conn_id, &busy);
  query->began = conn != NULL;
  if (!conn) {
    query->error = str_dup(busy ? "Connection busy: another query is in progress"
                                : "Invalid connection ID");
    query->error_code = busy ? LACE_ERR_CONNECTION_BUSY : LACE_ERR_INVALID_CONN_ID;
    query->status = ASYNC_QUERY_ERROR;
    LOG_WARN("Query %lld rejected: connection %d is %s",
             (long long)query->query_id, query->conn_id,
             busy ? "busy" : "unknown");
    async_queue_push(query->queue, query);
    return NULL;
  }

  char *err = NULL;
  if (!laced_session_check_tunnel(query->session, query->conn_id, &err)) {
    query->error = err ? err : str_dup("SSH tunnel is down");
    query->error_code = LACE_ERR_CONNECTION_LOST;
    query->status = ASYNC_QUERY_ERROR;
    laced_session_finish_query(query->session, query->conn_id, false);
    async_queue_push(query->queue, query);
    return NULL;
  }

  /* A lost connection is reopened (or refused) here, before the cancel
   * handle is taken: a handle taken first addressed the dead session, and
   * Esc on the first statement after a reconnect did nothing */
  bool answered = false;
  if (conn->driver && conn->driver->ensure_ready &&
      !conn->driver->ensure_ready(conn, query->sql, &answered, &err)) {
    query->error = err ? err : str_dup("Connection is not usable");
    query->error_code = LACE_ERR_CONNECTION_LOST;
    query->status = ASYNC_QUERY_ERROR;
    query->tx_state = conn->driver->transaction_state
                          ? conn->driver->transaction_state(conn)
                          : 0;
    laced_session_finish_query(query->session, query->conn_id, false);
    async_queue_push(query->queue, query);
    return NULL;
  }
  laced_session_arm_cancel(query->session, query->conn_id, query->query_id);

  /* Run it and reply with what it actually produced: rows (SELECT, but also
   * INSERT/UPDATE/DELETE ... RETURNING, CALL, CHECK TABLE, SHOW) or a row
   * count. Deciding by keyword dropped RETURNING rows and left MySQL result
   * sets unread ("Commands out of sync"). A cancel that arrived before the
   * handle existed is honoured by not starting the statement. */
  ResultSet *rs = NULL;
  if (answered) {
    rs = db_result_alloc_empty();
  } else if (!query->cancel_requested) {
    rs = db_query(conn, query->sql, &err);
  }
  bool success = rs != NULL;
  /* Read while this worker still owns the connection */
  int tx_state = conn->driver && conn->driver->transaction_state
                     ? conn->driver->transaction_state(conn)
                     : 0;
  query->tx_state = tx_state;
  laced_session_finish_query(query->session, query->conn_id, success);

  /* A cancel that arrived as the statement finished did not stop it:
   * report what actually happened (a completed write is committed) */
  if (!rs && query->cancel_requested) {
    query->status = ASYNC_QUERY_CANCELLED;
    query->error = str_dup("Query cancelled");
    query->error_code = LACE_ERR_QUERY_CANCELLED;
    free(err);
  } else if (!rs) {
    query->status = ASYNC_QUERY_ERROR;
    query->error = err ? err : str_dup("Query failed");
    query->error_code = LACE_ERR_QUERY_FAILED;
  } else {
    cJSON *result = cJSON_CreateObject();
    if (rs->num_columns > 0) {
      cJSON_AddStringToObject(result, "type", "select");
      cJSON *data = laced_json_from_result(rs);
      if (data) {
        cJSON_AddItemToObject(result, "data", data);
      }
    } else {
      cJSON_AddStringToObject(result, "type", "exec");
      cJSON_AddNumberToObject(result, "affected", (double)rs->rows_affected);
    }
    /* Lets the client notice a transaction it would lose by reconnecting */
    cJSON_AddBoolToObject(result, "in_transaction", tx_state != 0);
    if (tx_state == 2) {
      cJSON_AddBoolToObject(result, "transaction_failed", true);
    }
    query->result = result;
    query->status = ASYNC_QUERY_COMPLETED;
    db_result_free(rs);
    free(err);
  }

  /* Log completion */
  uint64_t elapsed_ms = get_time_ms() - start_time;
  if (query->status == ASYNC_QUERY_COMPLETED) {
    LOG_DEBUG("Query %lld completed in %llu ms", (long long)query->query_id,
              (unsigned long long)elapsed_ms);
  } else if (query->status == ASYNC_QUERY_CANCELLED) {
    LOG_INFO("Query %lld cancelled after %llu ms", (long long)query->query_id,
             (unsigned long long)elapsed_ms);
  } else {
    LOG_WARN("Query %lld failed after %llu ms: %s", (long long)query->query_id,
             (unsigned long long)elapsed_ms, query->error ? query->error : "unknown error");
  }

  async_queue_push(query->queue, query);
  return NULL;
}

/* ==========================================================================
 * Async Query API
 * ========================================================================== */

static AsyncQuery *async_query_create(AsyncQueue *queue, LacedSession *session,
                                      int conn_id, cJSON *request_id,
                                      const AsyncRequester *from) {
  AsyncQuery *query = safe_calloc(1, sizeof(AsyncQuery));
  if (!query) {
    return NULL;
  }
  query->tx_state = -1;

  query->queue = queue;
  query->session = session;
  query->conn_id = conn_id;
  query->status = ASYNC_QUERY_PENDING;
  query->client_serial = from ? from->serial : 0;
  snprintf(query->owner, sizeof(query->owner), "%s",
           from && from->owner ? from->owner : "");

  /* Assign unique query ID and timestamp */
  pthread_mutex_lock(&queue->mutex);
  query->query_id = queue->next_query_id++;
  pthread_mutex_unlock(&queue->mutex);
  query->started_at_ms = get_time_ms();

  if (request_id) {
    query->request_id = cJSON_Duplicate(request_id, true);
  }

  return query;
}

static bool async_query_launch(AsyncQuery *query) {
  /* Bounded: one thread per request, but never more than LACED_MAX_WORKERS
   * in all, nor LACED_MAX_JOBS_PER_CLIENT for one client (a client firing
   * requests cannot exhaust the daemon's threads for the others) */
  pthread_mutex_lock(&query->queue->mutex);
  size_t total = 0, mine = 0;
  for (AsyncQuery *q = query->queue->active_head; q; q = q->next) {
    total++;
    if (query->client_serial && q->client_serial == query->client_serial)
      mine++;
  }
  if (total >= LACED_MAX_WORKERS || mine >= LACED_MAX_JOBS_PER_CLIENT) {
    pthread_mutex_unlock(&query->queue->mutex);
    query->status = ASYNC_QUERY_ERROR;
    query->error = str_dup(total >= LACED_MAX_WORKERS
                               ? "The daemon is busy: too many requests"
                               : "Too many requests in progress from this "
                                 "client");
    query->error_code = LACE_ERR_DAEMON_BUSY;
    return false;
  }
  /* Add to active list */
  query->next = query->queue->active_head;
  query->queue->active_head = query;
  pthread_mutex_unlock(&query->queue->mutex);

  query->status = ASYNC_QUERY_RUNNING;

  if (pthread_create(&query->thread, NULL, query_worker, query) != 0) {
    /* Remove from active list */
    pthread_mutex_lock(&query->queue->mutex);
    AsyncQuery **pp = &query->queue->active_head;
    while (*pp && *pp != query) {
      pp = &(*pp)->next;
    }
    if (*pp) {
      *pp = query->next;
    }
    pthread_mutex_unlock(&query->queue->mutex);

    query->status = ASYNC_QUERY_ERROR;
    query->error = str_dup("Failed to create worker thread");
    query->error_code = LACE_ERR_INTERNAL_ERROR;
    return false;
  }

  pthread_detach(query->thread);
  return true;
}

AsyncQuery *async_exec_start(AsyncQueue *queue, LacedSession *session,
                             int conn_id, const char *sql,
                             cJSON *request_id, const AsyncRequester *from) {
  AsyncQuery *query =
      async_query_create(queue, session, conn_id, request_id, from);
  if (!query) {
    return NULL;
  }

  query->type = ASYNC_QUERY_TYPE_EXEC;
  query->sql = str_dup(sql);

  if (!async_query_launch(query)) {
    async_queue_push(queue, query);
  }

  return query;
}

AsyncQuery *async_job_start(AsyncQueue *queue, LacedSession *session,
                            cJSON *request_id, const AsyncRequester *from,
                            AsyncJobFn fn, void *arg,
                            void (*arg_free)(void *arg)) {
  AsyncQuery *query = async_query_create(queue, session, 0, request_id, from);
  if (!query) {
    if (arg_free) {
      arg_free(arg);
    }
    return NULL;
  }
  query->type = ASYNC_QUERY_TYPE_EXEC;
  query->job_fn = fn;
  query->job_arg = arg;
  query->job_arg_free = arg_free;

  if (!async_query_launch(query)) {
    async_queue_push(queue, query);
  }
  return query;
}

AsyncQueryStatus async_query_status(const AsyncQuery *query) {
  return query ? query->status : ASYNC_QUERY_ERROR;
}

cJSON *async_query_get_request_id(const AsyncQuery *query) {
  return query ? query->request_id : NULL;
}

char *async_query_take_reply_line(AsyncQuery *query) {
  if (!query) {
    return NULL;
  }
  char *line = query->reply_line;
  query->reply_line = NULL;
  return line;
}

cJSON *async_query_take_result(AsyncQuery *query) {
  if (!query) {
    return NULL;
  }
  cJSON *result = query->result;
  query->result = NULL;
  return result;
}

char *async_query_take_error(AsyncQuery *query) {
  if (!query) {
    return NULL;
  }
  char *error = query->error;
  query->error = NULL;
  return error;
}

cJSON *async_query_error_data(const AsyncQuery *query) {
  if (!query || query->tx_state < 0)
    return NULL;
  /* A failed statement can leave a transaction open, or (PostgreSQL) open
   * but failed: the client must not lose track of it */
  cJSON *data = cJSON_CreateObject();
  if (!data)
    return NULL;
  cJSON_AddBoolToObject(data, "in_transaction", query->tx_state != 0);
  if (query->tx_state == 2)
    cJSON_AddBoolToObject(data, "transaction_failed", true);
  return data;
}

int async_query_get_error_code(const AsyncQuery *query) {
  return query ? query->error_code : LACE_ERR_INTERNAL_ERROR;
}

void async_query_free(AsyncQuery *query) {
  if (!query) {
    return;
  }

  if (query->job_arg_free) {
    query->job_arg_free(query->job_arg);
  }
  str_secure_free(query->sql); /* SQL may embed passwords or data */
  free(query->error);
  free(query->reply_line);
  if (query->result) {
    cJSON_Delete(query->result);
  }
  if (query->request_id) {
    cJSON_Delete(query->request_id);
  }
  free(query);
}

bool async_cancel_by_conn_id(AsyncQueue *queue, LacedSession *session,
                             int conn_id, const char *owner, bool *running,
                             char **err) {
  if (running) {
    *running = false;
  }
  if (!queue || !session) {
    err_set(err, "Invalid session");
    return false;
  }

  /* Only the statement running on conn_id, and (unless owner is NULL, the
   * daemon's own reaper) only if the requesting client started it: a late
   * cancel must not stop a statement another client began meanwhile */
  /* The statement that owns conn_id (passed begin_query); only if none does
   * yet, the newest launched one, whose cancel then stops it before it
   * starts. A query about to be refused as busy must not be the target
   * while the real one runs. (A finished one can still sit in the list
   * until its reply is queued: the newest wins.) */
  bool found = false, foreign = false;
  int64_t target = 0;
  pthread_mutex_lock(&queue->mutex);
  AsyncQuery *match = NULL;
  for (AsyncQuery *q = queue->active_head; q; q = q->next) {
    if (q->job_fn || q->conn_id != conn_id || q->status != ASYNC_QUERY_RUNNING) {
      continue;
    }
    if (!match || (q->began && !match->began) ||
        (q->began == match->began && q->query_id > match->query_id)) {
      match = q;
    }
  }
  if (match) {
    if (owner && strcmp(owner, match->owner) != 0) {
      foreign = true;
    } else {
      match->cancel_requested = true;
      target = match->query_id;
      found = true;
    }
  }
  pthread_mutex_unlock(&queue->mutex);

  if (foreign) {
    err_set(err, "The running statement belongs to another client");
    return false;
  }
  if (!found) {
    return true; /* Nothing running: nothing to cancel */
  }
  if (running) {
    *running = true;
  }
  /* Send the cancel to the database once, and only to that statement */
  return laced_session_cancel_query(session, conn_id, owner ? target : 0, err);
}

/* ==========================================================================
 * Query Tracking
 * ========================================================================== */

int64_t async_query_get_id(const AsyncQuery *query) {
  return query ? query->query_id : 0;
}

bool async_get_active_queries(AsyncQueue *queue, int conn_id,
                              AsyncQueryInfo **out_info, size_t *out_count) {
  if (!queue || !out_info || !out_count) {
    return false;
  }

  *out_info = NULL;
  *out_count = 0;

  pthread_mutex_lock(&queue->mutex);

  /* Count matching queries */
  size_t count = 0;
  for (AsyncQuery *q = queue->active_head; q; q = q->next) {
    if (conn_id == 0 || q->conn_id == conn_id) {
      count++;
    }
  }

  if (count == 0) {
    pthread_mutex_unlock(&queue->mutex);
    return true;
  }

  /* Allocate result array */
  AsyncQueryInfo *info = safe_calloc(count, sizeof(AsyncQueryInfo));
  if (!info) {
    pthread_mutex_unlock(&queue->mutex);
    return false;
  }

  /* Fill in query info */
  size_t i = 0;
  for (AsyncQuery *q = queue->active_head; q && i < count; q = q->next) {
    if (conn_id == 0 || q->conn_id == conn_id) {
      info[i].query_id = q->query_id;
      info[i].conn_id = q->conn_id;
      info[i].type = q->type;
      info[i].status = q->status;
      info[i].started_at_ms = q->started_at_ms;

      /* Copy description (SQL) */
      if (q->sql) {
        info[i].description = str_dup(q->sql);
      }

      i++;
    }
  }

  pthread_mutex_unlock(&queue->mutex);

  *out_info = info;
  *out_count = i;
  return true;
}

void async_query_info_free(AsyncQueryInfo *info, size_t count) {
  if (!info) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(info[i].description);
  }
  free(info);
}

/* ==========================================================================
 * Client Tracking
 * ========================================================================== */

uint64_t async_query_get_client_serial(const AsyncQuery *query) {
  return query ? query->client_serial : 0;
}
