/*
 * laced - Lace Database Daemon
 * RPC method handler interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_HANDLER_H
#define LACED_HANDLER_H

#include "session.h"
#include <cjson/cJSON.h>
#include <signal.h>

/* Forward declarations */
typedef struct AsyncQueue AsyncQueue;
typedef struct AsyncQuery AsyncQuery;

/* Handler result structure */
typedef struct {
  cJSON *result;          /* Result JSON (NULL on error) */
  int error_code;         /* Error code (0 on success) */
  char *error_message;    /* Error message (NULL on success, caller must free) */
  bool deferred;          /* If true, response will be sent later (async query) */
  AsyncQuery *deferred_query; /* For deferred responses, pointer to async query */
} LacedHandlerResult;

/*
 * Dispatch a JSON-RPC method call to the appropriate handler.
 *
 * @param session       Session manager (connection pool)
 * @param async_queue   Async queue for background queries (may be NULL for sync-only)
 * @param method        Method name
 * @param params        Method parameters (may be NULL)
 * @param request_id    Request ID for async response tracking (may be NULL)
 * @param shutdown_flag Pointer to shutdown flag (set by "shutdown" method)
 * @return              Handler result (check .deferred for async operations)
 */
LacedHandlerResult laced_handler_dispatch(LacedSession *session,
                                          AsyncQueue *async_queue,
                                          const char *method,
                                          cJSON *params,
                                          cJSON *request_id,
                                          volatile sig_atomic_t *shutdown_flag);

#endif /* LACED_HANDLER_H */
