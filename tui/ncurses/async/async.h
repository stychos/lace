/*
 * Lace
 * Async operation infrastructure for cancellable database operations
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef ASYNC_H
#define ASYNC_H

#include "../db_compat.h"
#include "../platform/thread.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Operation types */
typedef enum {
  ASYNC_OP_CONNECT,
  ASYNC_OP_LIST_TABLES,
  ASYNC_OP_GET_SCHEMA,
  ASYNC_OP_QUERY_PAGE,
  ASYNC_OP_QUERY_PAGE_WHERE,
  ASYNC_OP_COUNT_ROWS,
  ASYNC_OP_COUNT_ROWS_WHERE,
  ASYNC_OP_QUERY,
  ASYNC_OP_EXEC
} AsyncOpType;

/* Operation states */
typedef enum {
  ASYNC_STATE_IDLE,
  ASYNC_STATE_RUNNING,
  ASYNC_STATE_COMPLETED,
  ASYNC_STATE_CANCELLED,
  ASYNC_STATE_ERROR
} AsyncState;

/* Async operation structure */
typedef struct {
  AsyncOpType op_type;
  AsyncState state;

  /* Input parameters (set by caller before starting) */
  lace_client_t *client; /* For ASYNC_OP_CONNECT */
  DbConnection *conn;
  char *connstr;
  char *table_name;
  char *sql;
  char *where_clause;
  char *order_by;
  size_t offset;
  size_t limit;
  bool desc;
  bool use_approximate;

  /* Output results (set by worker thread) */
  void *result;        /* ResultSet*, TableSchema*, DbConnection*, char** */
  char *error;         /* Error message if failed */
  int64_t count;       /* For count/exec operations */
  size_t result_count; /* For list operations (e.g., table count) */
  bool is_approximate; /* True if count is an estimate */

  /* Synchronization */
  lace_mutex_t mutex;
  lace_cond_t cond;
  volatile bool cancel_requested;

  /* For cancellation */
  void *cancel_handle; /* Driver-specific cancel handle */
} AsyncOperation;

/* Initialize an async operation structure */
void async_init(AsyncOperation *op);

/* Start an async operation (spawns worker thread) */
bool async_start(AsyncOperation *op);

/* Poll operation state (non-blocking) */
AsyncState async_poll(AsyncOperation *op);

/* Request cancellation of an async operation */
void async_cancel(AsyncOperation *op);

/* Wait for operation to complete (with timeout in ms, 0 = just check) */
bool async_wait(AsyncOperation *op, int timeout_ms);

/* Free async operation resources (does NOT free result) */
void async_free(AsyncOperation *op);

#endif /* ASYNC_H */
