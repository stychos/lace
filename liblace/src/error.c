/*
 * liblace - Lace Client Library
 * Error handling implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/error.h"

const char *lace_error_message(int code) {
  switch (code) {
  /* Success */
  case LACE_OK:
    return "Success";

  /* JSON-RPC standard errors */
  case LACE_ERR_PARSE_ERROR:
    return "Parse error: Invalid JSON";
  case LACE_ERR_INVALID_REQUEST:
    return "Invalid request: Not a valid JSON-RPC request";
  case LACE_ERR_METHOD_NOT_FOUND:
    return "Method not found";
  case LACE_ERR_INVALID_PARAMS:
    return "Invalid parameters";
  case LACE_ERR_INTERNAL_ERROR:
    return "Internal error";

  /* Connection errors */
  case LACE_ERR_CONNECTION_FAILED:
    return "Failed to connect to database";
  case LACE_ERR_AUTH_REQUIRED:
    return "Authentication required";
  case LACE_ERR_AUTH_FAILED:
    return "Authentication failed";
  case LACE_ERR_CONNECTION_LOST:
    return "Lost connection to database";
  case LACE_ERR_CONNECTION_CLOSED:
    return "Connection is closed";
  case LACE_ERR_INVALID_CONN_ID:
    return "Invalid connection ID";

  /* Query errors */
  case LACE_ERR_QUERY_FAILED:
    return "Query execution failed";
  case LACE_ERR_QUERY_CANCELLED:
    return "Query was cancelled";
  case LACE_ERR_QUERY_TIMEOUT:
    return "Query timed out";
  case LACE_ERR_SYNTAX_ERROR:
    return "SQL syntax error";

  /* Data errors */
  case LACE_ERR_TABLE_NOT_FOUND:
    return "Table not found";
  case LACE_ERR_COLUMN_NOT_FOUND:
    return "Column not found";
  case LACE_ERR_ROW_NOT_FOUND:
    return "Row not found";
  case LACE_ERR_CONSTRAINT_VIOLATION:
    return "Constraint violation";
  case LACE_ERR_TYPE_MISMATCH:
    return "Data type mismatch";

  /* Transaction errors */
  case LACE_ERR_TRANSACTION_FAILED:
    return "Transaction operation failed";
  case LACE_ERR_DEADLOCK:
    return "Deadlock detected";

  /* Client errors */
  case LACE_ERR_DAEMON_NOT_FOUND:
    return "Daemon not found or could not be started";
  case LACE_ERR_DAEMON_CRASHED:
    return "Daemon process crashed";
  case LACE_ERR_PIPE_ERROR:
    return "IPC pipe error";
  case LACE_ERR_TIMEOUT:
    return "Request timed out";

  /* Resource errors */
  case LACE_ERR_OUT_OF_MEMORY:
    return "Out of memory";
  case LACE_ERR_TOO_MANY_CONNS:
    return "Too many connections";
  case LACE_ERR_RESULT_TOO_LARGE:
    return "Result set too large";

  default:
    return "Unknown error";
  }
}
