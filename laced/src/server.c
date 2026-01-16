/*
 * laced - Lace Database Daemon
 * JSON-RPC server implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "server.h"
#include "async.h"
#include "handler.h"
#include "json.h"
#include "session.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* Server structure */
struct LacedServer {
  LacedSession *session;   /* Connection pool */
  AsyncQueue *async_queue; /* Async query queue */
  int async_notify_fd;     /* Pipe fd for async notifications */
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

  /* Create async queue for background queries */
  server->async_queue = async_queue_create(&server->async_notify_fd);
  if (!server->async_queue) {
    laced_session_destroy(server->session);
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

  if (server->async_queue) {
    async_queue_destroy(server->async_queue);
  }

  if (server->session) {
    laced_session_destroy(server->session);
  }

  free(server);
}

/* Get async queue (for handler use) */
AsyncQueue *laced_server_get_async_queue(LacedServer *server) {
  return server ? server->async_queue : NULL;
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
      server->session, server->async_queue, method->valuestring, params, id);

  bool ok = true;
  if (!is_notification && !result.deferred) {
    /* Only send response if not deferred (async handlers send response later) */
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

/* Process completed async queries and send responses */
static void process_async_completions(LacedServer *server, FILE *output) {
  async_queue_drain_notify(server->async_queue);

  AsyncQuery *query;
  while ((query = async_queue_pop(server->async_queue)) != NULL) {
    cJSON *request_id = async_query_get_request_id(query);
    AsyncQueryStatus status = async_query_status(query);

    if (status == ASYNC_QUERY_COMPLETED) {
      cJSON *result = async_query_take_result(query);
      send_result(output, request_id, result);
    } else {
      /* Error or cancelled */
      char *error_msg = async_query_take_error(query);
      int error_code = async_query_get_error_code(query);
      send_error(output, request_id, error_code,
                 error_msg ? error_msg : "Query failed");
      free(error_msg);
    }

    async_query_free(query);
  }
}

/* Non-blocking line read - returns NULL if no complete line available */
static char *try_read_line(int fd, char **partial_buf, size_t *partial_len,
                           size_t *partial_cap) {
  /* Read available data */
  char temp[4096];
  ssize_t n = read(fd, temp, sizeof(temp));

  if (n <= 0) {
    if (n == 0) {
      /* EOF - return any partial data as final line */
      if (*partial_len > 0) {
        char *line = *partial_buf;
        line[*partial_len] = '\0';
        *partial_buf = NULL;
        *partial_len = 0;
        *partial_cap = 0;
        return line;
      }
    }
    return NULL;
  }

  /* Ensure buffer capacity */
  size_t needed = *partial_len + (size_t)n + 1;
  if (needed > *partial_cap) {
    size_t new_cap = *partial_cap ? *partial_cap * 2 : 4096;
    while (new_cap < needed) new_cap *= 2;
    char *new_buf = realloc(*partial_buf, new_cap);
    if (!new_buf) {
      return NULL;
    }
    *partial_buf = new_buf;
    *partial_cap = new_cap;
  }

  /* Append new data */
  memcpy(*partial_buf + *partial_len, temp, (size_t)n);
  *partial_len += (size_t)n;

  /* Look for newline */
  for (size_t i = 0; i < *partial_len; i++) {
    if ((*partial_buf)[i] == '\n') {
      /* Extract complete line */
      size_t line_len = i;
      char *line = malloc(line_len + 1);
      if (!line) {
        return NULL;
      }
      memcpy(line, *partial_buf, line_len);
      line[line_len] = '\0';

      /* Shift remaining data */
      size_t remaining = *partial_len - i - 1;
      if (remaining > 0) {
        memmove(*partial_buf, *partial_buf + i + 1, remaining);
      }
      *partial_len = remaining;

      return line;
    }
  }

  return NULL; /* No complete line yet */
}

int laced_server_run_stdio(LacedServer *server,
                           volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  /* Set stdin to non-blocking */
  int stdin_fd = fileno(stdin);
  int flags = fcntl(stdin_fd, F_GETFL, 0);
  fcntl(stdin_fd, F_SETFL, flags | O_NONBLOCK);

  /* Set stdout to line-buffered */
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* Partial line buffer */
  char *partial_buf = NULL;
  size_t partial_len = 0;
  size_t partial_cap = 0;

  int max_fd = stdin_fd > server->async_notify_fd ? stdin_fd : server->async_notify_fd;

  while (!*shutdown_flag) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(stdin_fd, &read_fds);
    FD_SET(server->async_notify_fd, &read_fds);

    /* Short timeout so we can check shutdown flag */
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000}; /* 100ms */

    int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (ready < 0) {
      if (errno == EINTR) {
        continue; /* Interrupted by signal */
      }
      break; /* Error */
    }

    /* Check for async completions */
    if (ready > 0 && FD_ISSET(server->async_notify_fd, &read_fds)) {
      process_async_completions(server, stdout);
    }

    /* Check for stdin input */
    if (ready > 0 && FD_ISSET(stdin_fd, &read_fds)) {
      char *line;
      while ((line = try_read_line(stdin_fd, &partial_buf, &partial_len,
                                   &partial_cap)) != NULL) {
        /* Skip empty lines */
        if (line[0] != '\0') {
          process_request(server, stdout, line);
        }
        free(line);
      }

      /* Check for EOF */
      if (feof(stdin)) {
        break;
      }
    }
  }

  free(partial_buf);

  /* Restore stdin to blocking */
  fcntl(stdin_fd, F_SETFL, flags);

  return 0;
}
