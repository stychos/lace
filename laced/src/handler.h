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

/* Forward declarations */
typedef struct AsyncQueue AsyncQueue;

/* Handler result structure */
typedef struct {
  cJSON *result;        /* Result JSON (NULL on error) */
  int error_code;       /* Error code (0 on success) */
  char *error_message;  /* Error message (NULL on success, caller must free) */
  bool deferred;        /* If true, response will be sent later (async query) */
} LacedHandlerResult;

/*
 * Dispatch a JSON-RPC method call to the appropriate handler.
 *
 * @param session     Session manager (connection pool)
 * @param async_queue Async queue for background queries (may be NULL for sync-only)
 * @param method      Method name
 * @param params      Method parameters (may be NULL)
 * @param request_id  Request ID for async response tracking (may be NULL)
 * @return            Handler result (check .deferred for async operations)
 */
LacedHandlerResult laced_handler_dispatch(LacedSession *session,
                                          AsyncQueue *async_queue,
                                          const char *method,
                                          cJSON *params,
                                          cJSON *request_id);

#endif /* LACED_HANDLER_H */
