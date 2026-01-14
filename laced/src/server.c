/*
 * laced - Lace Database Daemon
 * JSON-RPC server implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "server.h"
#include "handler.h"
#include "json.h"
#include "session.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Server structure */
struct LacedServer {
  LacedSession *session; /* Connection pool */
  bool initialized;
};

/* ==========================================================================
 * Server Lifecycle
 * ========================================================================== */

LacedServer *laced_server_create(void) {
  LacedServer *server = calloc(1, sizeof(LacedServer));
  if (!server) {
    return NULL;
  }

  /* Create session manager (connection pool) */
  server->session = laced_session_create();
  if (!server->session) {
    free(server);
    return NULL;
  }

  server->initialized = true;
  return server;
}

void laced_server_destroy(LacedServer *server) {
  if (!server) {
    return;
  }

  if (server->session) {
    laced_session_destroy(server->session);
  }

  free(server);
}

/* ==========================================================================
 * Line-based I/O
 * ========================================================================== */

/* Read a line from stdin (newline-delimited JSON-RPC) */
static char *read_line(FILE *input) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    return NULL;
  }

  while (1) {
    int c = fgetc(input);
    if (c == EOF) {
      if (len == 0) {
        free(buf);
        return NULL;
      }
      break;
    }
    if (c == '\n') {
      break;
    }

    /* Grow buffer if needed */
    if (len + 1 >= cap) {
      size_t new_cap = cap * 2;
      if (new_cap < cap) { /* Overflow check */
        free(buf);
        return NULL;
      }
      char *new_buf = realloc(buf, new_cap);
      if (!new_buf) {
        free(buf);
        return NULL;
      }
      buf = new_buf;
      cap = new_cap;
    }

    buf[len++] = (char)c;
  }

  buf[len] = '\0';
  return buf;
}

/* Write a line to stdout */
static bool write_line(FILE *output, const char *line) {
  if (fprintf(output, "%s\n", line) < 0) {
    return false;
  }
  if (fflush(output) != 0) {
    return false;
  }
  return true;
}

/* ==========================================================================
 * JSON-RPC Message Processing
 * ========================================================================== */

/* Send an error response */
static bool send_error(FILE *output, cJSON *id, int code, const char *message) {
  cJSON *resp = cJSON_CreateObject();
  if (!resp) {
    return false;
  }

  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  if (id) {
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
  } else {
    cJSON_AddNullToObject(resp, "id");
  }

  cJSON *error = cJSON_CreateObject();
  if (error) {
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(resp, "error", error);
  }

  char *json = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);

  if (!json) {
    return false;
  }

  bool ok = write_line(output, json);
  free(json);
  return ok;
}

/* Send a success response */
static bool send_result(FILE *output, cJSON *id, cJSON *result) {
  cJSON *resp = cJSON_CreateObject();
  if (!resp) {
    if (result) {
      cJSON_Delete(result);
    }
    return false;
  }

  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  if (id) {
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
  } else {
    cJSON_AddNullToObject(resp, "id");
  }

  if (result) {
    cJSON_AddItemToObject(resp, "result", result);
  } else {
    cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
  }

  char *json = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);

  if (!json) {
    return false;
  }

  bool ok = write_line(output, json);
  free(json);
  return ok;
}

/* Process a single JSON-RPC request */
static bool process_request(LacedServer *server, FILE *output,
                            const char *json_str) {
  cJSON *req = cJSON_Parse(json_str);
  if (!req) {
    return send_error(output, NULL, -32700, "Parse error");
  }

  /* Validate JSON-RPC structure */
  cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
  cJSON *method = cJSON_GetObjectItem(req, "method");
  cJSON *id = cJSON_GetObjectItem(req, "id");
  cJSON *params = cJSON_GetObjectItem(req, "params");

  if (!jsonrpc || !cJSON_IsString(jsonrpc) ||
      strcmp(jsonrpc->valuestring, "2.0") != 0) {
    cJSON_Delete(req);
    return send_error(output, id, -32600, "Invalid Request: must be JSON-RPC 2.0");
  }

  if (!method || !cJSON_IsString(method)) {
    cJSON_Delete(req);
    return send_error(output, id, -32600, "Invalid Request: missing method");
  }

  /* Check if this is a notification (no id) */
  bool is_notification = (id == NULL);

  /* Handle the request */
  LacedHandlerResult result = laced_handler_dispatch(
      server->session, method->valuestring, params);

  bool ok = true;
  if (!is_notification) {
    if (result.error_code != 0) {
      ok = send_error(output, id, result.error_code,
                      result.error_message ? result.error_message : "Internal error");
    } else {
      ok = send_result(output, id, result.result);
      result.result = NULL; /* Ownership transferred */
    }
  }

  /* Cleanup */
  if (result.result) {
    cJSON_Delete(result.result);
  }
  free(result.error_message);
  cJSON_Delete(req);

  return ok;
}

/* ==========================================================================
 * Server Execution
 * ========================================================================== */

int laced_server_run_stdio(LacedServer *server,
                           volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  /* Set stdin/stdout to line-buffered */
  setvbuf(stdin, NULL, _IOLBF, 0);
  setvbuf(stdout, NULL, _IOLBF, 0);

  while (!*shutdown_flag) {
    /* Check for EOF */
    if (feof(stdin)) {
      break;
    }

    /* Read a request line */
    char *line = read_line(stdin);
    if (!line) {
      if (feof(stdin)) {
        break; /* Clean EOF */
      }
      if (errno == EINTR) {
        continue; /* Interrupted by signal, check shutdown flag */
      }
      break; /* Error */
    }

    /* Skip empty lines */
    if (line[0] == '\0') {
      free(line);
      continue;
    }

    /* Process the request */
    if (!process_request(server, stdout, line)) {
      free(line);
      /* Write error, but don't exit - let client disconnect */
      continue;
    }

    free(line);
  }

  return 0;
}
