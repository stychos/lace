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

/* Handler result structure */
typedef struct {
  cJSON *result;        /* Result JSON (NULL on error) */
  int error_code;       /* Error code (0 on success) */
  char *error_message;  /* Error message (NULL on success, caller must free) */
} LacedHandlerResult;

/*
 * Dispatch a JSON-RPC method call to the appropriate handler.
 *
 * @param session  Session manager (connection pool)
 * @param method   Method name
 * @param params   Method parameters (may be NULL)
 * @return         Handler result
 */
LacedHandlerResult laced_handler_dispatch(LacedSession *session,
                                          const char *method,
                                          cJSON *params);

#endif /* LACED_HANDLER_H */
