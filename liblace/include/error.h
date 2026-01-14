/*
 * liblace - Lace Client Library
 * Error codes and handling
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_ERROR_H
#define LIBLACE_ERROR_H

/* ==========================================================================
 * Return Codes
 * ========================================================================== */

/* Success */
#define LACE_OK 0

/* ==========================================================================
 * JSON-RPC Standard Error Codes (-32700 to -32600)
 * ========================================================================== */

#define LACE_ERR_PARSE_ERROR      -32700  /* Invalid JSON */
#define LACE_ERR_INVALID_REQUEST  -32600  /* Not a valid JSON-RPC request */
#define LACE_ERR_METHOD_NOT_FOUND -32601  /* Method does not exist */
#define LACE_ERR_INVALID_PARAMS   -32602  /* Invalid method parameters */
#define LACE_ERR_INTERNAL_ERROR   -32603  /* Internal JSON-RPC error */

/* ==========================================================================
 * Application Error Codes (-32000 to -32099)
 * ========================================================================== */

/* Connection errors */
#define LACE_ERR_CONNECTION_FAILED  -32001  /* Failed to connect to database */
#define LACE_ERR_AUTH_REQUIRED      -32002  /* Authentication required */
#define LACE_ERR_AUTH_FAILED        -32003  /* Authentication failed */
#define LACE_ERR_CONNECTION_LOST    -32004  /* Lost connection to database */
#define LACE_ERR_CONNECTION_CLOSED  -32005  /* Connection already closed */
#define LACE_ERR_INVALID_CONN_ID    -32006  /* Invalid connection ID */

/* Query errors */
#define LACE_ERR_QUERY_FAILED       -32010  /* Query execution failed */
#define LACE_ERR_QUERY_CANCELLED    -32011  /* Query was cancelled */
#define LACE_ERR_QUERY_TIMEOUT      -32012  /* Query timed out */
#define LACE_ERR_SYNTAX_ERROR       -32013  /* SQL syntax error */

/* Data errors */
#define LACE_ERR_TABLE_NOT_FOUND    -32020  /* Table does not exist */
#define LACE_ERR_COLUMN_NOT_FOUND   -32021  /* Column does not exist */
#define LACE_ERR_ROW_NOT_FOUND      -32022  /* Row not found (for update/delete) */
#define LACE_ERR_CONSTRAINT_VIOLATION -32023 /* Constraint violation */
#define LACE_ERR_TYPE_MISMATCH      -32024  /* Data type mismatch */

/* Transaction errors */
#define LACE_ERR_TRANSACTION_FAILED -32030  /* Transaction operation failed */
#define LACE_ERR_DEADLOCK           -32031  /* Deadlock detected */

/* Client errors */
#define LACE_ERR_DAEMON_NOT_FOUND   -32040  /* Could not find/start daemon */
#define LACE_ERR_DAEMON_CRASHED     -32041  /* Daemon process crashed */
#define LACE_ERR_PIPE_ERROR         -32042  /* IPC pipe error */
#define LACE_ERR_TIMEOUT            -32043  /* Request timed out */

/* Resource errors */
#define LACE_ERR_OUT_OF_MEMORY      -32050  /* Memory allocation failed */
#define LACE_ERR_TOO_MANY_CONNS     -32051  /* Too many connections */
#define LACE_ERR_RESULT_TOO_LARGE   -32052  /* Result set too large */

/* ==========================================================================
 * Error Handling Functions
 * ========================================================================== */

/* Get human-readable error message for error code */
const char *lace_error_message(int code);

/* Check if error code indicates authentication is required */
static inline int lace_is_auth_error(int code) {
  return code == LACE_ERR_AUTH_REQUIRED || code == LACE_ERR_AUTH_FAILED;
}

/* Check if error code indicates connection problem */
static inline int lace_is_connection_error(int code) {
  return code >= LACE_ERR_CONNECTION_FAILED && code <= LACE_ERR_INVALID_CONN_ID;
}

/* Check if error is recoverable (can retry) */
static inline int lace_is_recoverable(int code) {
  return code == LACE_ERR_CONNECTION_LOST ||
         code == LACE_ERR_QUERY_TIMEOUT ||
         code == LACE_ERR_DEADLOCK;
}

#endif /* LIBLACE_ERROR_H */
