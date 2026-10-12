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
#include "constants.h"
#include <cjson/cJSON.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>

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
 * Stop all work before teardown: flag and cancel every running query, then
 * wait up to timeout_ms for all workers to finish. Returns false if some are
 * still running; the session and queue must then not be freed (workers still
 * use them), so the caller should just exit.
 */
bool async_queue_shutdown(AsyncQueue *queue, LacedSession *session,
                          int timeout_ms);

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

/* Who started a job: it counts against that client's job limit, only that
 * owner may cancel its statement, and the reply goes to that client
 * (matched by serial: client pointers are reused). NULL where a job takes
 * one: internal (the reaper), serial 0 and owner "". */
typedef struct {
  uint64_t serial;   /* LacedClient.serial */
  const char *owner; /* LacedClient.owner */
} AsyncRequester;

/*
 * Start an async SQL execution.
 *
 * @param queue    Queue for completion notification
 * @param session  Session handle
 * @param conn_id  Connection ID
 * @param sql      SQL statement
 * @param request_id  JSON-RPC request ID (will be duplicated)
 * @param from     Requesting client
 * @return         Query handle, or NULL on error
 */
AsyncQuery *async_exec_start(AsyncQueue *queue, LacedSession *session,
                             int conn_id, const char *sql,
                             cJSON *request_id, const AsyncRequester *from);

/*
 * Get query status.
 */
/*
 * Generic job run on a worker thread, replied to like a query. Used for
 * operations that may block (connect, disconnect, cancel) so the daemon's
 * main loop keeps serving other clients. Returns the JSON result, or NULL
 * with *err (allocated) and *err_code set.
 */
typedef cJSON *(*AsyncJobFn)(LacedSession *session, void *arg, char **err,
                             int *err_code);

/*
 * Start a job. arg is owned by the job and released with arg_free (may be
 * NULL) when the job is freed.
 */
AsyncQuery *async_job_start(AsyncQueue *queue, LacedSession *session,
                            cJSON *request_id, const AsyncRequester *from,
                            AsyncJobFn fn, void *arg,
                            void (*arg_free)(void *arg));

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

/* A completed query's reply line, serialized on its worker (NULL if there
 * is none: an error, a notification); caller frees */
char *async_query_take_reply_line(AsyncQuery *query);

/*
 * Get the error message (only valid after ERROR status).
 * Ownership transfers to caller.
 */
char *async_query_take_error(AsyncQuery *query);

/*
 * Get the error code (only valid after ERROR status).
 */
int async_query_get_error_code(const AsyncQuery *query);

/* JSON-RPC error "data" for a failed statement: the connection's
 * transaction state ({"in_transaction", "transaction_failed"}), or NULL if
 * unknown. Caller owns it. */
cJSON *async_query_error_data(const AsyncQuery *query);

/*
 * Free an async query.
 */
void async_query_free(AsyncQuery *query);

/*
 * Cancel the statement running on a connection: set its cancel flag and ask
 * the driver to stop it, if the client owner started it (owner NULL: any,
 * for the daemon itself). *running (optional) says whether one was.
 * Returns false with *err set when the driver could not send the cancel.
 */
bool async_cancel_by_conn_id(AsyncQueue *queue, LacedSession *session,
                             int conn_id, const char *owner, bool *running,
                             char **err);

/* ==========================================================================
 * Query Tracking
 * ========================================================================== */

/* Information about a running query */
typedef struct {
  int64_t query_id;           /* Unique query ID */
  int conn_id;                /* Connection ID */
  AsyncQueryType type;        /* Type of query */
  _Atomic AsyncQueryStatus status; /* Written by the worker, read under
                                    * the queue lock by cancel */
  char *description;          /* SQL or table name (caller must free) */
  uint64_t started_at_ms;     /* Start time in milliseconds since epoch */
} AsyncQueryInfo;

/*
 * Get list of active queries for a connection.
 * If conn_id is 0, returns all active queries.
 *
 * @param queue     Async queue
 * @param conn_id   Connection ID (0 for all connections)
 * @param out_info  Output: allocated array of query info (caller must free with async_query_info_free)
 * @param out_count Output: number of queries
 * @return          true on success
 */
bool async_get_active_queries(AsyncQueue *queue, int conn_id,
                              AsyncQueryInfo **out_info, size_t *out_count);

/*
 * Free query info array returned by async_get_active_queries.
 */
void async_query_info_free(AsyncQueryInfo *info, size_t count);

/*
 * Get the unique query ID for an async query.
 */
int64_t async_query_get_id(const AsyncQuery *query);

/* ==========================================================================
 * Client Tracking (for socket mode)
 * ========================================================================== */

/*
 * Get the serial of the client that issued an async query (0 for stdio).
 * Match against LacedClient.serial; client pointers can be reused after free.
 */
uint64_t async_query_get_client_serial(const AsyncQuery *query);

#endif /* LACED_ASYNC_H */
