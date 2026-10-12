/*
 * laced - Lace Database Daemon
 * RPC method handler interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_HANDLER_H
#define LACED_HANDLER_H

#include "async.h"
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

/* One request, as the handlers see it. Dispatch runs on the daemon's main
 * thread; a handler that starts a job copies what the job needs. */
typedef struct {
  LacedSession *session;               /* Connection pool */
  AsyncQueue *queue;                   /* Worker queue for deferred replies */
  cJSON *params;                       /* Method parameters (may be NULL) */
  cJSON *id;                           /* Request id (NULL: a notification) */
  AsyncRequester from;                 /* Requesting client; the connections
                                          it opens are reaped once no socket
                                          with its owner key remains */
  volatile sig_atomic_t *shutdown_flag; /* Set by "shutdown" */
} LacedRequest;

/* Dispatch method to its handler (check .deferred for async operations) */
LacedHandlerResult laced_handler_dispatch(const LacedRequest *rq,
                                          const char *method);

#endif /* LACED_HANDLER_H */
