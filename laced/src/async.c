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
#include "util/str.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ==========================================================================
 * Async Query Structure
 * ========================================================================== */

struct AsyncQuery {
  AsyncQueryType type;
  AsyncQueryStatus status;

  /* Input parameters */
  LacedSession *session;
  int conn_id;
  char *table;
  char *sql;
  size_t offset;
  size_t limit;

  /* Request tracking */
  cJSON *request_id;

  /* Result (set by worker thread) */
  cJSON *result;
  char *error;
  int error_code;

  /* Threading */
  pthread_t thread;
  volatile bool cancel_requested;

  /* Queue linkage */
  AsyncQuery *next;
  AsyncQueue *queue;
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
};

/* ==========================================================================
 * Queue Implementation
 * ========================================================================== */

AsyncQueue *async_queue_create(int *notify_fd) {
  AsyncQueue *queue = calloc(1, sizeof(AsyncQueue));
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

  if (notify_fd) {
    *notify_fd = queue->notify_pipe[0];
  }

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

  /* Prepare cancellation */
  laced_session_prepare_cancel(query->session, query->conn_id);

  DbConnection *conn = laced_session_get_connection(query->session, query->conn_id);
  if (!conn) {
    query->error = str_dup("Invalid connection ID");
    query->error_code = -32602;
    query->status = ASYNC_QUERY_ERROR;
    laced_session_finish_query(query->session, query->conn_id);
    async_queue_push(query->queue, query);
    return NULL;
  }

  char *err = NULL;

  switch (query->type) {
  case ASYNC_QUERY_TYPE_QUERY: {
    ResultSet *rs = db_query_page(conn, query->table, query->offset,
                                   query->limit, NULL, false, &err);
    laced_session_finish_query(query->session, query->conn_id);

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
      /* Get total count */
      int64_t total = db_count_rows(conn, query->table, NULL);
      if (total >= 0) {
        rs->total_rows = (size_t)total;
      }

      query->result = laced_json_from_result(rs);
      query->status = ASYNC_QUERY_COMPLETED;
      db_result_free(rs);
      free(err);
    }
    break;
  }

  case ASYNC_QUERY_TYPE_EXEC: {
    /* Check if it's a SELECT statement */
    const char *p = query->sql;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    bool is_select = (strncasecmp(p, "SELECT", 6) == 0 ||
                      strncasecmp(p, "PRAGMA", 6) == 0 ||
                      strncasecmp(p, "SHOW", 4) == 0 ||
                      strncasecmp(p, "DESCRIBE", 8) == 0 ||
                      strncasecmp(p, "EXPLAIN", 7) == 0);

    if (is_select) {
      ResultSet *rs = db_query(conn, query->sql, &err);
      laced_session_finish_query(query->session, query->conn_id);

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
      laced_session_finish_query(query->session, query->conn_id);

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
    break;
  }

  case ASYNC_QUERY_TYPE_COUNT: {
    int64_t count = db_count_rows(conn, query->table, &err);
    laced_session_finish_query(query->session, query->conn_id);

    if (query->cancel_requested) {
      query->status = ASYNC_QUERY_CANCELLED;
      query->error = str_dup("Query cancelled");
      query->error_code = -32000;
      free(err);
    } else if (count < 0) {
      query->status = ASYNC_QUERY_ERROR;
      query->error = err ? err : str_dup("Count failed");
      query->error_code = -32603;
    } else {
      cJSON *result = cJSON_CreateObject();
      cJSON_AddNumberToObject(result, "count", (double)count);
      cJSON_AddBoolToObject(result, "approximate", false);
      query->result = result;
      query->status = ASYNC_QUERY_COMPLETED;
      free(err);
    }
    break;
  }
  }

  async_queue_push(query->queue, query);
  return NULL;
}

/* ==========================================================================
 * Async Query API
 * ========================================================================== */

static AsyncQuery *async_query_create(AsyncQueue *queue, LacedSession *session,
                                      int conn_id, cJSON *request_id) {
  AsyncQuery *query = calloc(1, sizeof(AsyncQuery));
  if (!query) {
    return NULL;
  }

  query->queue = queue;
  query->session = session;
  query->conn_id = conn_id;
  query->status = ASYNC_QUERY_PENDING;

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

AsyncQuery *async_query_start(AsyncQueue *queue, LacedSession *session,
                              int conn_id, const char *table,
                              size_t offset, size_t limit,
                              cJSON *request_id) {
  AsyncQuery *query = async_query_create(queue, session, conn_id, request_id);
  if (!query) {
    return NULL;
  }

  query->type = ASYNC_QUERY_TYPE_QUERY;
  query->table = str_dup(table);
  query->offset = offset;
  query->limit = limit;

  if (!async_query_launch(query)) {
    /* Error already set, push to queue for response */
    async_queue_push(queue, query);
  }

  return query;
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

AsyncQuery *async_count_start(AsyncQueue *queue, LacedSession *session,
                              int conn_id, const char *table,
                              cJSON *request_id) {
  AsyncQuery *query = async_query_create(queue, session, conn_id, request_id);
  if (!query) {
    return NULL;
  }

  query->type = ASYNC_QUERY_TYPE_COUNT;
  query->table = str_dup(table);

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

  free(query->table);
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
