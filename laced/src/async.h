/*
 * laced - Lace Database Daemon
 * Async query execution infrastructure
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_ASYNC_H
#define LACED_ASYNC_H

#include "session.h"
#include <cjson/cJSON.h>
#include <pthread.h>
#include <stdbool.h>

/* Forward declaration */
typedef struct AsyncQuery AsyncQuery;
typedef struct AsyncQueue AsyncQueue;

/* Query status */
typedef enum {
  ASYNC_QUERY_PENDING,
  ASYNC_QUERY_RUNNING,
  ASYNC_QUERY_COMPLETED,
  ASYNC_QUERY_CANCELLED,
  ASYNC_QUERY_ERROR
} AsyncQueryStatus;

/* Query type */
typedef enum {
  ASYNC_QUERY_TYPE_QUERY,  /* Table query with pagination */
  ASYNC_QUERY_TYPE_EXEC,   /* Raw SQL execution */
  ASYNC_QUERY_TYPE_COUNT   /* Row count */
} AsyncQueryType;

/* ==========================================================================
 * Async Queue (thread-safe response queue)
 * ========================================================================== */

/*
 * Create async queue.
 * Returns pipe read fd for select() integration, or -1 on error.
 */
AsyncQueue *async_queue_create(int *notify_fd);

/*
 * Destroy async queue.
 */
void async_queue_destroy(AsyncQueue *queue);

/*
 * Pop a completed query from the queue (non-blocking).
 * Returns NULL if queue is empty.
 * Caller takes ownership of returned AsyncQuery.
 */
AsyncQuery *async_queue_pop(AsyncQueue *queue);

/*
 * Drain the notification pipe after reading from it.
 * Call this after select() indicates the notify_fd is readable.
 */
void async_queue_drain_notify(AsyncQueue *queue);

/* ==========================================================================
 * Async Query
 * ========================================================================== */

/*
 * Start an async table query.
 *
 * @param queue    Queue for completion notification
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param table    Table name
 * @param offset   Pagination offset
 * @param limit    Pagination limit
 * @param request_id  JSON-RPC request ID (will be duplicated)
 * @return         Query handle, or NULL on error
 */
AsyncQuery *async_query_start(AsyncQueue *queue, LacedSession *session,
                              int conn_id, const char *table,
                              size_t offset, size_t limit,
                              cJSON *request_id);

/*
 * Start an async SQL execution.
 *
 * @param queue    Queue for completion notification
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param sql      SQL statement
 * @param request_id  JSON-RPC request ID (will be duplicated)
 * @return         Query handle, or NULL on error
 */
AsyncQuery *async_exec_start(AsyncQueue *queue, LacedSession *session,
                             int conn_id, const char *sql,
                             cJSON *request_id);

/*
 * Start an async row count.
 *
 * @param queue    Queue for completion notification
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param table    Table name
 * @param request_id  JSON-RPC request ID (will be duplicated)
 * @return         Query handle, or NULL on error
 */
AsyncQuery *async_count_start(AsyncQueue *queue, LacedSession *session,
                              int conn_id, const char *table,
                              cJSON *request_id);

/*
 * Get query status.
 */
AsyncQueryStatus async_query_status(const AsyncQuery *query);

/*
 * Get the JSON-RPC request ID for this query.
 */
cJSON *async_query_get_request_id(const AsyncQuery *query);

/*
 * Get the result JSON (only valid after COMPLETED status).
 * Ownership transfers to caller.
 */
cJSON *async_query_take_result(AsyncQuery *query);

/*
 * Get the error message (only valid after ERROR status).
 * Ownership transfers to caller.
 */
char *async_query_take_error(AsyncQuery *query);

/*
 * Get the error code (only valid after ERROR status).
 */
int async_query_get_error_code(const AsyncQuery *query);

/*
 * Free an async query.
 */
void async_query_free(AsyncQuery *query);

/*
 * Cancel a query by connection ID.
 * This sets the cancel flag and calls the session cancel function.
 */
bool async_cancel_by_conn_id(AsyncQueue *queue, LacedSession *session, int conn_id);

#endif /* LACED_ASYNC_H */
