/*
 * Lace
 * Async operation implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "async.h"
#include "../util/str.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Worker thread function */
static void *async_worker_thread(void *arg) {
  AsyncOperation *op = (AsyncOperation *)arg;

  pthread_mutex_lock(&op->mutex);
  op->state = ASYNC_STATE_RUNNING;
  pthread_mutex_unlock(&op->mutex);

  /* Prepare cancellation handle before starting operation */
  if (op->conn && op->conn->driver && op->conn->driver->prepare_cancel) {
    op->cancel_handle = op->conn->driver->prepare_cancel(op->conn);
  }

  /* Execute the operation */
  char *err = NULL;

  switch (op->op_type) {
  case ASYNC_OP_CONNECT:
    op->result = db_connect(op->connstr, &err);
    break;

  case ASYNC_OP_LIST_TABLES:
    op->result = db_list_tables(op->conn, &op->result_count, &err);
    break;

  case ASYNC_OP_GET_SCHEMA:
    op->result = db_get_table_schema(op->conn, op->table_name, &err);
    break;

  case ASYNC_OP_QUERY_PAGE:
    op->result = db_query_page(op->conn, op->table_name, op->offset, op->limit,
                               op->order_by, op->desc, &err);
    break;

  case ASYNC_OP_QUERY_PAGE_WHERE:
    op->result =
        db_query_page_where(op->conn, op->table_name, op->offset, op->limit,
                            op->where_clause, op->order_by, op->desc, &err);
    break;

  case ASYNC_OP_COUNT_ROWS:
    if (op->use_approximate && op->conn && op->conn->driver &&
        op->conn->driver->estimate_row_count) {
      op->count =
          op->conn->driver->estimate_row_count(op->conn, op->table_name, &err);
      if (op->count >= 0 && op->count < 1000000) {
        /* Approximate count is under 1M - get exact count instead */
        free(err);
        err = NULL;
        op->count = db_count_rows(op->conn, op->table_name, &err);
        op->is_approximate = false;
      } else if (op->count >= 0) {
        /* Use approximate count for large tables */
        op->is_approximate = true;
      } else {
        /* Fall back to exact count if approximate fails */
        free(err);
        err = NULL;
        op->count = db_count_rows(op->conn, op->table_name, &err);
        op->is_approximate = false;
      }
    } else {
      op->count = db_count_rows(op->conn, op->table_name, &err);
      op->is_approximate = false;
    }
    break;

  case ASYNC_OP_COUNT_ROWS_WHERE:
    op->count =
        db_count_rows_where(op->conn, op->table_name, op->where_clause, &err);
    op->is_approximate = false; /* WHERE counts are always exact */
    break;

  case ASYNC_OP_QUERY:
    op->result = db_query(op->conn, op->sql, &err);
    break;

  case ASYNC_OP_EXEC:
    op->count = db_exec(op->conn, op->sql, &err);
    break;
  }

  /* Clean up cancel handle */
  if (op->cancel_handle && op->conn && op->conn->driver &&
      op->conn->driver->free_cancel_handle) {
    op->conn->driver->free_cancel_handle(op->cancel_handle);
    op->cancel_handle = NULL;
  }

  /* Update state and signal completion */
  pthread_mutex_lock(&op->mutex);
  if (op->cancel_requested) {
    op->state = ASYNC_STATE_CANCELLED;
    /* Free any partial result on cancellation */
    if (op->result) {
      switch (op->op_type) {
      case ASYNC_OP_QUERY:
      case ASYNC_OP_QUERY_PAGE:
      case ASYNC_OP_QUERY_PAGE_WHERE:
        db_result_free(op->result);
        break;
      case ASYNC_OP_GET_SCHEMA:
        db_schema_free(op->result);
        break;
      case ASYNC_OP_LIST_TABLES:
        if (op->result) {
          char **tables = (char **)op->result;
          for (size_t i = 0; i < op->result_count; i++) {
            free(tables[i]);
          }
          free(tables);
        }
        break;
      case ASYNC_OP_CONNECT:
        if (op->result) {
          db_disconnect(op->result);
        }
        break;
      default:
        break;
      }
      op->result = NULL;
    }
    free(err);
  } else if (err) {
    op->state = ASYNC_STATE_ERROR;
    op->error = err;
  } else {
    op->state = ASYNC_STATE_COMPLETED;
  }
  pthread_cond_signal(&op->cond);
  pthread_mutex_unlock(&op->mutex);

  return NULL;
}

void async_init(AsyncOperation *op) {
  if (!op)
    return;
  memset(op, 0, sizeof(AsyncOperation));
  op->state = ASYNC_STATE_IDLE;
}

bool async_start(AsyncOperation *op) {
  if (!op)
    return false;

  pthread_mutex_init(&op->mutex, NULL);
  pthread_cond_init(&op->cond, NULL);
  op->state = ASYNC_STATE_IDLE;
  op->cancel_requested = false;
  op->cancel_handle = NULL;

  /* Use smaller stack size (256KB instead of default 8MB) to reduce VIRT */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);

  pthread_t thread;
  if (pthread_create(&thread, &attr, async_worker_thread, op) != 0) {
    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&op->mutex);
    pthread_cond_destroy(&op->cond);
    return false;
  }
  pthread_attr_destroy(&attr);
  pthread_detach(thread); /* Thread cleans up itself */
  return true;
}

AsyncState async_poll(AsyncOperation *op) {
  if (!op)
    return ASYNC_STATE_ERROR;

  pthread_mutex_lock(&op->mutex);
  AsyncState state = op->state;
  pthread_mutex_unlock(&op->mutex);
  return state;
}

void async_cancel(AsyncOperation *op) {
  if (!op)
    return;

  pthread_mutex_lock(&op->mutex);
  op->cancel_requested = true;

  /* Call driver-specific cancel if operation is running */
  if (op->state == ASYNC_STATE_RUNNING && op->cancel_handle && op->conn &&
      op->conn->driver && op->conn->driver->cancel_query) {
    char *err = NULL;
    op->conn->driver->cancel_query(op->conn, op->cancel_handle, &err);
    free(err);
  }
  pthread_mutex_unlock(&op->mutex);
}

bool async_wait(AsyncOperation *op, int timeout_ms) {
  if (!op)
    return false;

  pthread_mutex_lock(&op->mutex);

  if (timeout_ms == 0) {
    /* Just check */
    bool done =
        (op->state != ASYNC_STATE_IDLE && op->state != ASYNC_STATE_RUNNING);
    pthread_mutex_unlock(&op->mutex);
    return done;
  }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }

  while (op->state == ASYNC_STATE_IDLE || op->state == ASYNC_STATE_RUNNING) {
    if (pthread_cond_timedwait(&op->cond, &op->mutex, &ts) == ETIMEDOUT) {
      pthread_mutex_unlock(&op->mutex);
      return false;
    }
  }

  pthread_mutex_unlock(&op->mutex);
  return true;
}

void async_free(AsyncOperation *op) {
  if (!op)
    return;

  pthread_mutex_destroy(&op->mutex);
  pthread_cond_destroy(&op->cond);
  free(op->connstr);
  free(op->table_name);
  free(op->sql);
  free(op->where_clause);
  free(op->order_by);
  free(op->error);
  /* Note: op->result is owned by caller, not freed here */

  /* Reset to safe state */
  op->connstr = NULL;
  op->table_name = NULL;
  op->sql = NULL;
  op->where_clause = NULL;
  op->order_by = NULL;
  op->error = NULL;
}
