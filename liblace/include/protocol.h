/*
 * liblace - Lace Client Library
 * JSON-RPC Protocol Definitions
 *
 * This file defines the protocol between laced (daemon) and clients.
 * Communication is via JSON-RPC 2.0 over stdin/stdout or Unix sockets.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_PROTOCOL_H
#define LIBLACE_PROTOCOL_H

#include "types.h"
#include <stdint.h>

/* ==========================================================================
 * Protocol Version
 * ========================================================================== */

#define LACE_PROTOCOL_VERSION "1.0"
#define LACE_JSONRPC_VERSION "2.0"

/* ==========================================================================
 * RPC Method Names
 * ========================================================================== */

/* Connection management */
#define LACE_METHOD_CONNECT       "connect"
#define LACE_METHOD_DISCONNECT    "disconnect"
#define LACE_METHOD_CONNECTIONS   "connections"
#define LACE_METHOD_RECONNECT     "reconnect"

/* Schema discovery */
#define LACE_METHOD_TABLES        "tables"
#define LACE_METHOD_SCHEMA        "schema"
#define LACE_METHOD_DATABASES     "databases"

/* Data queries */
#define LACE_METHOD_QUERY         "query"
#define LACE_METHOD_COUNT         "count"
#define LACE_METHOD_EXEC          "exec"

/* Data mutations */
#define LACE_METHOD_UPDATE        "update"
#define LACE_METHOD_DELETE        "delete"
#define LACE_METHOD_INSERT        "insert"

/* Streaming */
#define LACE_METHOD_STREAM_CHUNK  "stream.chunk"
#define LACE_METHOD_STREAM_END    "stream.end"
#define LACE_METHOD_STREAM_CANCEL "stream.cancel"

/* Daemon control */
#define LACE_METHOD_PING          "ping"
#define LACE_METHOD_SHUTDOWN      "shutdown"
#define LACE_METHOD_VERSION       "version"

/* ==========================================================================
 * Request/Response Message IDs
 * ========================================================================== */

/* Request ID type - can be string or integer per JSON-RPC spec */
typedef union {
  int64_t num;
  char *str;
  bool is_string;
} LaceRequestId;

/* ==========================================================================
 * Request Parameters
 * ========================================================================== */

/* connect params */
typedef struct {
  char *connstr;       /* Connection string */
  char *password;      /* Password (optional, NULL if not provided) */
} LaceConnectParams;

/* disconnect params */
typedef struct {
  int conn_id;
} LaceDisconnectParams;

/* tables params */
typedef struct {
  int conn_id;
} LaceTablesParams;

/* schema params */
typedef struct {
  int conn_id;
  char *table;
} LaceSchemaParams;

/* query params */
typedef struct {
  int conn_id;
  char *table;
  LaceFilter *filters;
  size_t num_filters;
  LaceSort *sorts;
  size_t num_sorts;
  size_t offset;
  size_t limit;
  bool stream;          /* Request streaming response */
} LaceQueryParams;

/* count params */
typedef struct {
  int conn_id;
  char *table;
  LaceFilter *filters;
  size_t num_filters;
} LaceCountParams;

/* exec params */
typedef struct {
  int conn_id;
  char *sql;
} LaceExecParams;

/* update params */
typedef struct {
  int conn_id;
  char *table;
  LacePkValue *pk;
  size_t num_pk;
  char *column;
  LaceValue value;
} LaceUpdateParams;

/* delete params */
typedef struct {
  int conn_id;
  char *table;
  LacePkValue *pk;
  size_t num_pk;
} LaceDeleteParams;

/* insert params */
typedef struct {
  int conn_id;
  char *table;
  char **columns;
  LaceValue *values;
  size_t num_columns;
} LaceInsertParams;

/* ==========================================================================
 * Response Results
 * ========================================================================== */

/* connect result */
typedef struct {
  int conn_id;
} LaceConnectResult;

/* tables result */
typedef struct {
  char **tables;
  size_t num_tables;
} LaceTablesResult;

/* query result - uses LaceResult from types.h */

/* count result */
typedef struct {
  size_t count;
  bool approximate;     /* True if count is approximate (estimated) */
} LaceCountResult;

/* exec result */
typedef struct {
  enum { LACE_EXEC_SELECT, LACE_EXEC_MODIFY } type;
  union {
    LaceResult *result;      /* For SELECT */
    int64_t affected;        /* For INSERT/UPDATE/DELETE */
  };
  char *source_table;        /* Detected table name (for SELECT, may be NULL) */
} LaceExecResult;

/* insert result */
typedef struct {
  LacePkValue *pk;           /* Primary key of inserted row */
  size_t num_pk;
} LaceInsertResult;

/* version result */
typedef struct {
  char *daemon_version;
  char *protocol_version;
  char *drivers[4];          /* Available drivers */
  size_t num_drivers;
} LaceVersionResult;

/* stream.chunk notification params */
typedef struct {
  char *stream_id;
  LaceRow *rows;
  size_t num_rows;
  size_t chunk_index;
} LaceStreamChunk;

/* ==========================================================================
 * Generic RPC Message Structures
 * ========================================================================== */

/* JSON-RPC Request */
typedef struct {
  char *jsonrpc;             /* Must be "2.0" */
  LaceRequestId id;          /* Request ID (for matching response) */
  char *method;              /* Method name */
  void *params;              /* Method-specific params (cast based on method) */
} LaceRpcRequest;

/* JSON-RPC Response */
typedef struct {
  char *jsonrpc;             /* Must be "2.0" */
  LaceRequestId id;          /* Same ID as request */
  void *result;              /* Result data (NULL if error) */
  struct {
    int code;
    char *message;
    void *data;              /* Optional error data */
  } error;
  bool has_error;
} LaceRpcResponse;

/* JSON-RPC Notification (no id, no response expected) */
typedef struct {
  char *jsonrpc;
  char *method;
  void *params;
} LaceRpcNotification;

/* ==========================================================================
 * Protocol Helper Functions
 * ========================================================================== */

/* Serialize request to JSON string (caller must free) */
char *lace_request_to_json(const LaceRpcRequest *req);

/* Parse JSON string to response (caller must free with lace_response_free) */
int lace_json_to_response(const char *json, LaceRpcResponse *resp);

/* Free response structure */
void lace_response_free(LaceRpcResponse *resp);

/* Generate unique request ID */
int64_t lace_generate_request_id(void);

#endif /* LIBLACE_PROTOCOL_H */
