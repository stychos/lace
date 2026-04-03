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
#include <util/mem.h>
#include <util/str.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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
struct LacedClient;

struct AsyncQuery {
  AsyncQueryType type;
  AsyncQueryStatus status;

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
  char *error;
  int error_code;

  /* Threading */
  pthread_t thread;
  _Atomic bool cancel_requested;

  /* Queue linkage */
  AsyncQuery *next;
  AsyncQueue *queue;

  /* Client tracking (for socket mode) */
  struct LacedClient *client;
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

  /* Query ID counter */
  int64_t next_query_id;
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

  if (pipe(queue->notify_pipe) < 0) {
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
  pthread_mutex_destroy(&queue->mutex);
  free(queue);
}

/* Push completed query to queue (called by worker thread) */
static void async_queue_push(AsyncQueue *queue, AsyncQuery *query) {
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

  pthread_mutex_unlock(&queue->mutex);

  /* Signal the main loop */
  char c = 1;
  (void)write(queue->notify_pipe[1], &c, 1);
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

  LOG_DEBUG("Query %lld starting (conn_id=%d, type=%d)",
            (long long)query->query_id, query->conn_id, query->type);

  /* Prepare cancellation - this also checks if connection is busy */
  if (!laced_session_prepare_cancel(query->session, query->conn_id)) {
    query->error = str_dup("Connection busy: another query is in progress");
    query->error_code = -32001;
    query->status = ASYNC_QUERY_ERROR;
    LOG_WARN("Query %lld rejected: connection %d is busy",
             (long long)query->query_id, query->conn_id);
    async_queue_push(query->queue, query);
    return NULL;
  }

  DbConnection *conn = laced_session_get_connection(query->session, query->conn_id);
  if (!conn) {
    query->error = str_dup("Invalid connection ID");
    query->error_code = -32602;
    query->status = ASYNC_QUERY_ERROR;
    LOG_ERROR("Query %lld failed: invalid connection ID %d",
              (long long)query->query_id, query->conn_id);
    laced_session_finish_query(query->session, query->conn_id, false);
    async_queue_push(query->queue, query);
    return NULL;
  }

  char *err = NULL;

  /* Execute SQL - check if it's a SELECT statement */
  const char *p = query->sql;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

  bool is_select = (strncasecmp(p, "SELECT", 6) == 0 ||
                    strncasecmp(p, "PRAGMA", 6) == 0 ||
                    strncasecmp(p, "SHOW", 4) == 0 ||
                    strncasecmp(p, "DESCRIBE", 8) == 0 ||
                    strncasecmp(p, "EXPLAIN", 7) == 0);

  if (is_select) {
    ResultSet *rs = db_query(conn, query->sql, &err);
    bool success = (rs != NULL && !query->cancel_requested);
    laced_session_finish_query(query->session, query->conn_id, success);

    if (query->cancel_requested) {
      query->status = ASYNC_QUERY_CANCELLED;
      query->error = str_dup("Query cancelled");
      query->error_code = -32000;
      if (rs) db_result_free(rs);
      free(err);
    } else if (!rs) {
      query->status = ASYNC_QUERY_ERROR;
      query->error = err ? err : str_dup("Query failed");
      query->error_code = -32603;
    } else {
      cJSON *result = cJSON_CreateObject();
      cJSON_AddStringToObject(result, "type", "select");
      cJSON *data = laced_json_from_result(rs);
      if (data) {
        cJSON_AddItemToObject(result, "data", data);
      }
      query->result = result;
      query->status = ASYNC_QUERY_COMPLETED;
      db_result_free(rs);
      free(err);
    }
  } else {
    int64_t affected = db_exec(conn, query->sql, &err);
    bool success = (affected >= 0 && !query->cancel_requested);
    laced_session_finish_query(query->session, query->conn_id, success);

    if (query->cancel_requested) {
      query->status = ASYNC_QUERY_CANCELLED;
      query->error = str_dup("Query cancelled");
      query->error_code = -32000;
      free(err);
    } else if (affected < 0) {
      query->status = ASYNC_QUERY_ERROR;
      query->error = err ? err : str_dup("Execution failed");
      query->error_code = -32603;
    } else {
      cJSON *result = cJSON_CreateObject();
      cJSON_AddStringToObject(result, "type", "exec");
      cJSON_AddNumberToObject(result, "affected", (double)affected);
      query->result = result;
      query->status = ASYNC_QUERY_COMPLETED;
      free(err);
    }
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
                                      int conn_id, cJSON *request_id) {
  AsyncQuery *query = safe_calloc(1, sizeof(AsyncQuery));
  if (!query) {
    return NULL;
  }

  query->queue = queue;
  query->session = session;
  query->conn_id = conn_id;
  query->status = ASYNC_QUERY_PENDING;

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
  /* Add to active list */
  pthread_mutex_lock(&query->queue->mutex);
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
    query->error_code = -32603;
    return false;
  }

  pthread_detach(query->thread);
  return true;
}

AsyncQuery *async_exec_start(AsyncQueue *queue, LacedSession *session,
                             int conn_id, const char *sql,
                             cJSON *request_id) {
  AsyncQuery *query = async_query_create(queue, session, conn_id, request_id);
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

AsyncQueryStatus async_query_status(const AsyncQuery *query) {
  return query ? query->status : ASYNC_QUERY_ERROR;
}

cJSON *async_query_get_request_id(const AsyncQuery *query) {
  return query ? query->request_id : NULL;
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

int async_query_get_error_code(const AsyncQuery *query) {
  return query ? query->error_code : -32603;
}

void async_query_free(AsyncQuery *query) {
  if (!query) {
    return;
  }

  free(query->sql);
  free(query->error);
  if (query->result) {
    cJSON_Delete(query->result);
  }
  if (query->request_id) {
    cJSON_Delete(query->request_id);
  }
  free(query);
}

bool async_cancel_by_conn_id(AsyncQueue *queue, LacedSession *session, int conn_id) {
  if (!queue || !session) {
    return false;
  }

  bool found = false;

  pthread_mutex_lock(&queue->mutex);

  /* Find query by connection ID in active list */
  AsyncQuery *q = queue->active_head;
  while (q) {
    if (q->conn_id == conn_id && q->status == ASYNC_QUERY_RUNNING) {
      q->cancel_requested = true;
      found = true;
      break;
    }
    q = q->next;
  }

  pthread_mutex_unlock(&queue->mutex);

  /* Call session cancel (this sends the actual cancel to the database) */
  if (found) {
    char *err = NULL;
    laced_session_cancel_query(session, conn_id, &err);
    free(err);
  }

  return found;
}

bool async_cancel_by_query_id(AsyncQueue *queue, LacedSession *session, int64_t query_id) {
  if (!queue || !session || query_id <= 0) {
    return false;
  }

  bool found = false;
  int conn_id = 0;

  pthread_mutex_lock(&queue->mutex);

  /* Find query by ID in active list */
  AsyncQuery *q = queue->active_head;
  while (q) {
    if (q->query_id == query_id && q->status == ASYNC_QUERY_RUNNING) {
      q->cancel_requested = true;
      conn_id = q->conn_id;
      found = true;
      break;
    }
    q = q->next;
  }

  pthread_mutex_unlock(&queue->mutex);

  /* Call session cancel */
  if (found && conn_id > 0) {
    char *err = NULL;
    laced_session_cancel_query(session, conn_id, &err);
    free(err);
  }

  return found;
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

void async_query_set_client(AsyncQuery *query, struct LacedClient *client) {
  if (query) {
    query->client = client;
  }
}

struct LacedClient *async_query_get_client(const AsyncQuery *query) {
  return query ? query->client : NULL;
}
