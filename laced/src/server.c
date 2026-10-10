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
#include "log.h"
#include "session.h"
#include <util/mem.h>
#include <util/str.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Default max clients */
#define DEFAULT_MAX_CLIENTS 64

/* Idle client timeout in seconds (5 minutes) */
#define CLIENT_IDLE_TIMEOUT 300

/* How long a client's DB connections outlive its last socket (enough for
 * liblace to reattach after a transient drop or daemon hiccup) */
#define LACED_ORPHAN_GRACE_S 60

/* How long shutdown waits for running queries to stop after cancelling them */
#define LACED_SHUTDOWN_WAIT_MS 5000

/* Server structure */
struct LacedServer {
  LacedSession *session;   /* Connection pool */
  AsyncQueue *async_queue; /* Async query queue */
  int async_notify_fd;     /* Pipe fd for async notifications */
  bool initialized;

  /* Socket mode fields */
  LacedTransport transport;  /* Current transport mode */
  int listen_fd;             /* Listening socket fd (-1 if stdio) */
  char *socket_path;         /* Unix socket path (for cleanup) */
  gid_t socket_group;        /* Group also allowed to connect ((gid_t)-1 = none) */
  char token[65];            /* TCP: clients must present it in hello ("" = Unix) */
  char *token_path;          /* TCP: file holding the token (removed on exit) */
  LacedClient *clients;      /* Linked list of connected clients */
  size_t num_clients;        /* Current client count */
  size_t max_clients;        /* Maximum allowed clients */

  /* Shutdown */
  volatile sig_atomic_t *shutdown_flag; /* Pointer to main shutdown flag */

  /* Idle shutdown */
  int idle_timeout;          /* Seconds with no clients before shutdown (0=disabled) */
  time_t idle_since;         /* Timestamp when last client disconnected (0=has clients) */
};

/* Socket utility functions (implemented in socket.c) */
extern int laced_create_unix_socket(const char *path, gid_t group);
extern bool laced_peer_allowed(int fd, gid_t group);
extern char *laced_get_default_socket_path(void);
extern char *lace_default_token_path(int port);
extern int laced_create_tcp_socket(const char *bind_addr, int port);
extern LacedClient *laced_accept_client(int listen_fd);
extern void laced_client_free(LacedClient *client);
extern bool laced_client_write_line(LacedClient *client, const char *line);
extern char *laced_client_try_read_line(LacedClient *client);
extern bool laced_client_is_connected(LacedClient *client);

/* ==========================================================================
 * Server Lifecycle
 * ========================================================================== */

LacedServer *laced_server_create(void) {
  LacedServer *server = safe_calloc(1, sizeof(LacedServer));
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

  /* Initialize socket mode fields */
  server->transport = LACED_TRANSPORT_STDIO;
  server->listen_fd = -1;
  server->socket_path = NULL;
  server->socket_group = (gid_t)-1;
  server->clients = NULL;
  server->num_clients = 0;
  server->max_clients = DEFAULT_MAX_CLIENTS;

  server->initialized = true;
  return server;
}

void laced_server_destroy(LacedServer *server) {
  if (!server) {
    return;
  }

  /* Close all client connections */
  LacedClient *client = server->clients;
  while (client) {
    LacedClient *next = client->next;
    laced_client_free(client);
    client = next;
  }
  server->clients = NULL;
  server->num_clients = 0;

  /* Close listening socket */
  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  /* Remove Unix socket file */
  if (server->socket_path) {
    unlink(server->socket_path);
    free(server->socket_path);
    server->socket_path = NULL;
  }

  /* Stop running queries and wait for their workers before freeing what
   * they use (the queue, the session's connections). If a worker is stuck
   * in a driver call, leave both allocated: the process is exiting, and
   * freeing them under the worker would crash it. */
  bool idle = async_queue_shutdown(server->async_queue, server->session,
                                   LACED_SHUTDOWN_WAIT_MS);
  if (!idle) {
    LOG_WARN("Workers still running after %d ms; exiting without teardown",
             LACED_SHUTDOWN_WAIT_MS);
    free(server);
    return;
  }

  if (server->async_queue) {
    async_queue_destroy(server->async_queue);
  }

  if (server->session) {
    laced_session_destroy(server->session);
  }

  if (server->token_path) {
    unlink(server->token_path);
    free(server->token_path);
  }
  memset(server->token, 0, sizeof(server->token));
  free(server);
}

void laced_server_set_token_file(LacedServer *server, const char *path) {
  if (server && path) {
    free(server->token_path);
    server->token_path = str_dup(path);
  }
}

/* Constant-time comparison: the token must not leak through timing */
static bool token_equal(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  unsigned char diff = (unsigned char)(la != lb);
  for (size_t i = 0; i < la && i < lb; i++) {
    diff |= (unsigned char)(a[i] ^ b[i]);
  }
  return diff == 0;
}

/* Create the TCP token and write it to a private file */
static bool write_token_file(LacedServer *server, int port) {
  unsigned char raw[16];
  int rfd = open("/dev/urandom", O_RDONLY);
  if (rfd < 0 || read(rfd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
    if (rfd >= 0) {
      close(rfd);
    }
    LOG_ERROR("Cannot generate the TCP token");
    return false;
  }
  close(rfd);
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(raw); i++) {
    server->token[2 * i] = hex[raw[i] >> 4];
    server->token[2 * i + 1] = hex[raw[i] & 15];
  }
  server->token[2 * sizeof(raw)] = '\0';

  if (!server->token_path) {
    char *sock = laced_get_default_socket_path(); /* Creates the directory */
    free(sock);
    server->token_path = lace_default_token_path(port);
  }
  if (!server->token_path) {
    return false;
  }
  unlink(server->token_path); /* A stale file from an earlier run */
  int fd = open(server->token_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                0600);
  if (fd < 0) {
    LOG_ERROR("Cannot create token file %s: %s", server->token_path,
              strerror(errno));
    return false;
  }
  bool ok = write(fd, server->token, strlen(server->token)) ==
                (ssize_t)strlen(server->token) &&
            write(fd, "\n", 1) == 1;
  close(fd);
  if (ok) {
    LOG_INFO("TCP clients authenticate with the token in %s",
             server->token_path);
  }
  return ok;
}

void laced_server_set_socket_group(LacedServer *server, gid_t group) {
  if (server) {
    server->socket_group = group;
  }
}

/* Get async queue (for handler use) */
AsyncQueue *laced_server_get_async_queue(LacedServer *server) {
  return server ? server->async_queue : NULL;
}

/* ==========================================================================
 * Line-based I/O
 * ========================================================================== */

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
static bool send_error_data(FILE *output, cJSON *id, int code,
                            const char *message, cJSON *data);

static bool send_error(FILE *output, cJSON *id, int code, const char *message) {
  return send_error_data(output, id, code, message, NULL);
}

/* Takes ownership of data (may be NULL) */
static bool send_error_data(FILE *output, cJSON *id, int code,
                            const char *message, cJSON *data) {
  cJSON *resp = cJSON_CreateObject();
  if (!resp) {
    cJSON_Delete(data);
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
    if (data) {
      cJSON_AddItemToObject(error, "data", data);
      data = NULL;
    }
    cJSON_AddItemToObject(resp, "error", error);
  }
  cJSON_Delete(data);

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
  if (strcmp(method->valuestring, "hello") == 0) {
    /* Owner keys only matter for socket clients; stdio has one client */
    bool ok = is_notification || send_result(output, id, cJSON_CreateObject());
    cJSON_Delete(req);
    return ok;
  }

  LacedHandlerResult result = laced_handler_dispatch(
      server->session, server->async_queue, method->valuestring, params, id,
      server->shutdown_flag, "stdio");

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

    if (!request_id) {
      /* Notification: JSON-RPC sends no reply */
    } else if (status == ASYNC_QUERY_COMPLETED) {
      cJSON *result = async_query_take_result(query);
      send_result(output, request_id, result);
    } else {
      /* Error or cancelled */
      char *error_msg = async_query_take_error(query);
      int error_code = async_query_get_error_code(query);
      send_error_data(output, request_id, error_code,
                      error_msg ? error_msg : "Query failed",
                      async_query_error_data(query));
      free(error_msg);
    }

    async_query_free(query);
  }
}

/* Non-blocking line read - returns NULL if no complete line available.
 * Sets *eof_flag to true if EOF is detected. */
static char *try_read_line(int fd, char **partial_buf, size_t *partial_len,
                           size_t *partial_cap, bool *eof_flag) {
  /* First check if we already have a complete line in the buffer */
  for (size_t i = 0; i < *partial_len; i++) {
    if ((*partial_buf)[i] == '\n') {
      /* Extract complete line */
      size_t line_len = i;
      char *line = safe_malloc(line_len + 1);
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

  /* No complete line in buffer, try to read more */
  char temp[4096];
  ssize_t n = read(fd, temp, sizeof(temp));

  if (n <= 0) {
    if (n == 0) {
      /* EOF detected - stdin pipe closed */
      *eof_flag = true;
      LOG_DEBUG("EOF detected on stdin");
      /* Return any partial data as final line */
      if (*partial_len > 0) {
        char *line = *partial_buf;
        line[*partial_len] = '\0';
        *partial_buf = NULL;
        *partial_len = 0;
        *partial_cap = 0;
        return line;
      }
    }
    /* n < 0: EAGAIN or error - no data available */
    return NULL;
  }

  /* Ensure buffer capacity; a request without a newline may not grow the
   * buffer forever (64 MB): treat the peer as broken and stop */
  size_t needed = *partial_len + (size_t)n + 1;
  if (needed > 64u * 1024u * 1024u) {
    LOG_ERROR("Request on stdin exceeds 64 MB without a newline; stopping");
    free(*partial_buf);
    *partial_buf = NULL;
    *partial_len = 0;
    *partial_cap = 0;
    *eof_flag = true;
    return NULL;
  }
  if (needed > *partial_cap) {
    size_t new_cap = *partial_cap ? *partial_cap * 2 : 4096;
    while (new_cap < needed) new_cap *= 2;
    char *new_buf = safe_realloc(*partial_buf, new_cap);
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
      char *line = safe_malloc(line_len + 1);
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

  server->shutdown_flag = shutdown_flag;

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
  bool stdin_eof = false;

  int max_fd = stdin_fd > server->async_notify_fd ? stdin_fd : server->async_notify_fd;

  while (!*shutdown_flag && !stdin_eof) {
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
                                   &partial_cap, &stdin_eof)) != NULL) {
        /* Skip empty lines */
        if (line[0] != '\0') {
          process_request(server, stdout, line);
        }
        free(line);
      }
    }
  }

  free(partial_buf);

  /* Restore stdin to blocking */
  fcntl(stdin_fd, F_SETFL, flags);

  return 0;
}

/* ==========================================================================
 * Socket Mode Support
 * ========================================================================== */

/* Add a client to the server's client list */
static void add_client(LacedServer *server, LacedClient *client) {
  static uint64_t next_serial = 1;
  client->serial = next_serial++;
  snprintf(client->owner, sizeof(client->owner), "s:%llu",
           (unsigned long long)client->serial);
  time_t now = time(NULL);
  client->connected_at = now;
  client->last_activity = now;
  client->requests_count = 0;
  client->next = server->clients;
  server->clients = client;
  server->num_clients++;
  server->idle_since = 0; /* Reset idle timer */
  LOG_INFO("Client connected (fd=%d, total=%zu)", client->fd, server->num_clients);
}

/* Remove a client from the server's client list */
static void remove_client(LacedServer *server, LacedClient *client) {
  LacedClient **pp = &server->clients;
  while (*pp && *pp != client) {
    pp = &(*pp)->next;
  }
  if (*pp) {
    *pp = client->next;
    server->num_clients--;
    time_t connected_secs = time(NULL) - client->connected_at;
    LOG_INFO("Client disconnected (fd=%d, connected=%lds, requests=%llu, remaining=%zu)",
             client->fd, (long)connected_secs,
             (unsigned long long)client->requests_count, server->num_clients);

    /* Start idle timer when last client disconnects */
    if (server->num_clients == 0 && server->idle_timeout > 0) {
      server->idle_since = time(NULL);
      LOG_INFO("No clients remaining, idle shutdown in %ds", server->idle_timeout);
    }
  }
  laced_client_free(client);
}

/* Send error response to a socket client */
static bool send_error_to_client_data(LacedClient *client, cJSON *id,
                                       int code, const char *message,
                                       cJSON *data);

static bool send_error_to_client(LacedClient *client, cJSON *id, int code,
                                  const char *message) {
  return send_error_to_client_data(client, id, code, message, NULL);
}

/* Takes ownership of data (may be NULL) */
static bool send_error_to_client_data(LacedClient *client, cJSON *id,
                                       int code, const char *message,
                                       cJSON *data) {
  cJSON *resp = cJSON_CreateObject();
  if (!resp) {
    cJSON_Delete(data);
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
    if (data) {
      cJSON_AddItemToObject(error, "data", data);
      data = NULL;
    }
    cJSON_AddItemToObject(resp, "error", error);
  }
  cJSON_Delete(data);

  char *json = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);

  if (!json) {
    return false;
  }

  bool ok = laced_client_write_line(client, json);
  free(json);
  return ok;
}

/* Send result response to a socket client */
static bool send_result_to_client(LacedClient *client, cJSON *id, cJSON *result) {
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

  bool ok = laced_client_write_line(client, json);
  free(json);
  return ok;
}

/* Process a JSON-RPC request from a socket client */
static bool process_client_request(LacedServer *server, LacedClient *client,
                                   const char *json_str) {
  /* Update client activity */
  client->last_activity = time(NULL);
  client->requests_count++;

  cJSON *req = cJSON_Parse(json_str);
  if (!req) {
    LOG_WARN("Client fd=%d: parse error", client->fd);
    return send_error_to_client(client, NULL, -32700, "Parse error");
  }

  /* Validate JSON-RPC structure */
  cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
  cJSON *method = cJSON_GetObjectItem(req, "method");
  cJSON *id = cJSON_GetObjectItem(req, "id");
  cJSON *params = cJSON_GetObjectItem(req, "params");

  LOG_DEBUG("Client fd=%d: method=%s", client->fd,
            method && cJSON_IsString(method) ? method->valuestring : "(invalid)");

  if (!jsonrpc || !cJSON_IsString(jsonrpc) ||
      strcmp(jsonrpc->valuestring, "2.0") != 0) {
    cJSON_Delete(req);
    return send_error_to_client(client, id, -32600,
                                 "Invalid Request: must be JSON-RPC 2.0");
  }

  if (!method || !cJSON_IsString(method)) {
    cJSON_Delete(req);
    return send_error_to_client(client, id, -32600,
                                 "Invalid Request: missing method");
  }

  /* Check if this is a notification (no id) */
  bool is_notification = (id == NULL);

  /* hello: the client names itself (liblace sends a random id per client
   * handle, shared by all its sockets). Connections it opens stay alive as
   * long as any socket with that id is connected. */
  if (strcmp(method->valuestring, "hello") == 0 && server->token[0]) {
    cJSON *tok = params ? cJSON_GetObjectItem(params, "token") : NULL;
    if (!tok || !cJSON_IsString(tok) || !token_equal(tok->valuestring, server->token)) {
      client->auth_failures++;
      LOG_WARN("Client fd=%d: bad or missing token (%d)", client->fd,
               client->auth_failures);
      bool ok = send_error_to_client(client, id, -32003, "Authentication failed");
      if (client->auth_failures >= 3) {
        client->closed = true;
      }
      cJSON_Delete(req);
      return ok;
    }
    client->authed = true;
  }
  if (!client->authed && strcmp(method->valuestring, "hello") != 0) {
    bool ok = is_notification ||
              send_error_to_client(client, id, -32003,
                                   "Authentication required: send hello with "
                                   "the daemon's token");
    cJSON_Delete(req);
    return ok;
  }

  if (strcmp(method->valuestring, "hello") == 0) {
    cJSON *cid = params ? cJSON_GetObjectItem(params, "client_id") : NULL;
    bool valid = cid && cJSON_IsString(cid) && cid->valuestring[0] &&
                 strlen(cid->valuestring) <= 48;
    for (const char *c = valid ? cid->valuestring : ""; valid && *c; c++) {
      valid = isalnum((unsigned char)*c) || *c == '-';
    }
    bool ok;
    if (!valid) {
      ok = is_notification ||
           send_error_to_client(client, id, -32602, "Invalid 'client_id'");
    } else {
      snprintf(client->owner, sizeof(client->owner), "u:%s", cid->valuestring);
      cJSON *res = cJSON_CreateObject();
      cJSON_AddNumberToObject(res, "orphan_grace_seconds", LACED_ORPHAN_GRACE_S);
      ok = is_notification || send_result_to_client(client, id, res);
      if (is_notification) {
        cJSON_Delete(res);
      }
    }
    cJSON_Delete(req);
    return ok;
  }

  /* Handle the request */
  LacedHandlerResult result = laced_handler_dispatch(
      server->session, server->async_queue, method->valuestring, params, id,
      server->shutdown_flag, client->owner);

  /* Store client pointer for async response routing */
  if (result.deferred) {
    async_query_set_client(result.deferred_query, client);
  }

  bool ok = true;
  if (!is_notification && !result.deferred) {
    /* Only send response if not deferred (async handlers send response later) */
    if (result.error_code != 0) {
      ok = send_error_to_client(
          client, id, result.error_code,
          result.error_message ? result.error_message : "Internal error");
    } else {
      ok = send_result_to_client(client, id, result.result);
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

/* Process completed async queries for socket mode */
static void process_async_completions_socket(LacedServer *server) {
  async_queue_drain_notify(server->async_queue);

  AsyncQuery *query;
  while ((query = async_queue_pop(server->async_queue)) != NULL) {
    cJSON *request_id = async_query_get_request_id(query);
    AsyncQueryStatus status = async_query_status(query);
    uint64_t serial = async_query_get_client_serial(query);
    LacedClient *client = NULL;

    /* Only send response if the issuing client is still connected
     * (and asked for one: a notification has no id) */
    if (serial != 0 && request_id) {
      bool client_valid = false;
      for (LacedClient *c = server->clients; c; c = c->next) {
        if (c->serial == serial && !c->closed) {
          client = c;
          client_valid = true;
          break;
        }
      }

      if (client_valid) {
        if (status == ASYNC_QUERY_COMPLETED) {
          cJSON *result = async_query_take_result(query);
          send_result_to_client(client, request_id, result);
        } else {
          /* Error or cancelled */
          char *error_msg = async_query_take_error(query);
          int error_code = async_query_get_error_code(query);
          send_error_to_client_data(client, request_id, error_code,
                                    error_msg ? error_msg : "Query failed",
                                    async_query_error_data(query));
          free(error_msg);
        }
      }
    }

    async_query_free(query);
  }
}

/* Close a connection whose owner is gone: stop its query, then disconnect.
 * Closing the connection rolls back any open transaction on every driver. */
typedef struct {
  AsyncQueue *queue;
  int conn_id;
} ReapArg;

static cJSON *reap_job(LacedSession *session, void *p, char **err,
                       int *err_code) {
  ReapArg *a = p;
  for (int attempt = 0; attempt < 150; attempt++) { /* ~30 s */
    char *derr = NULL;
    if (laced_session_disconnect(session, a->conn_id, &derr)) {
      LOG_INFO("conn_id=%d closed: its client is gone", a->conn_id);
      return cJSON_CreateObject();
    }
    free(derr);
    if (attempt % 25 == 0) {
      (void)async_cancel_by_conn_id(a->queue, session, a->conn_id);
    }
    usleep(200000);
  }
  laced_session_reap_failed(session, a->conn_id);
  *err_code = -32603;
  *err = str_dup("Orphaned connection did not stop; will retry");
  return NULL;
}

static void reap_orphans(LacedServer *server) {
  const char *live[1024];
  size_t n = 0;
  for (LacedClient *c = server->clients; c && n < 1024; c = c->next) {
    if (!c->closed) {
      live[n++] = c->owner;
    }
  }
  int ids[64];
  size_t found = laced_session_collect_orphans(
      server->session, live, n, LACED_ORPHAN_GRACE_S, ids, 64);
  for (size_t i = 0; i < found; i++) {
    ReapArg *arg = safe_malloc(sizeof(ReapArg));
    arg->queue = server->async_queue;
    arg->conn_id = ids[i];
    /* No request id: nobody is waiting for this reply */
    if (!async_job_start(server->async_queue, server->session, NULL, reap_job,
                         arg, free)) {
      laced_session_reap_failed(server->session, ids[i]);
    }
  }
}

/* Unified socket event loop */
static int run_socket_event_loop(LacedServer *server,
                                 volatile sig_atomic_t *shutdown_flag) {
  /* The idle period runs from startup too: a daemon whose spawning client
   * died before connecting must not wait for a first client forever */
  if (server->idle_timeout > 0 && server->num_clients == 0) {
    server->idle_since = time(NULL);
  }
  uint64_t accept_paused_until = 0; /* Back-off after EMFILE/ENFILE */
  time_t last_reap_check = 0;
  struct pollfd *pfds = NULL;
  size_t pfds_cap = 0;

  while (!*shutdown_flag) {
    /* poll(), not select(): fds above FD_SETSIZE (1024) would overflow an
     * fd_set once --max-clients is raised */
    size_t want = 2 + server->num_clients;
    if (want > pfds_cap) {
      pfds_cap = want * 2;
      pfds = safe_realloc(pfds, pfds_cap * sizeof(struct pollfd));
    }
    size_t nfds = 0;
    pfds[nfds++] = (struct pollfd){.fd = server->async_notify_fd, .events = POLLIN};
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms = (uint64_t)tv_now.tv_sec * 1000 + (uint64_t)tv_now.tv_usec / 1000;
    bool listen_polled = now_ms >= accept_paused_until;
    if (listen_polled) {
      pfds[nfds++] = (struct pollfd){.fd = server->listen_fd, .events = POLLIN};
    }
    size_t first_client = nfds;
    for (LacedClient *c = server->clients; c; c = c->next) {
      pfds[nfds++] = (struct pollfd){.fd = c->fd, .events = POLLIN};
    }

    /* Short timeout for checking shutdown flag */
    int ready = poll(pfds, (nfds_t)nfds, 100);

    if (ready < 0) {
      if (errno == EINTR) {
        continue; /* Interrupted by signal */
      }
      free(pfds);
      return 1; /* Error */
    }

    /* Record readiness on the clients before the list can change */
    size_t idx = first_client;
    for (LacedClient *c = server->clients; c; c = c->next, idx++) {
      c->readable = (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }

    /* Handle async completions */
    if (pfds[0].revents & POLLIN) {
      process_async_completions_socket(server);
    }

    /* Handle new connections */
    if (listen_polled && (pfds[1].revents & POLLIN)) {
      LacedClient *client = laced_accept_client(server->listen_fd);
      if (!client) {
        if (errno == EMFILE || errno == ENFILE) {
          /* Out of descriptors: the pending connection keeps the listening
           * socket readable, so pause accepting instead of spinning */
          LOG_WARN("accept: %s; pausing new connections", strerror(errno));
          accept_paused_until = now_ms + 500;
        }
      } else if (server->transport == LACED_TRANSPORT_UNIX &&
                 !laced_peer_allowed(client->fd, server->socket_group)) {
        /* Another user reached the socket (e.g. through a group-readable
         * directory): it must not use this user's database connections */
        LOG_WARN("Refused client fd=%d: different user, not in socket group",
                 client->fd);
        close(client->fd);
        free(client);
      } else if (server->num_clients >= server->max_clients) {
        /* At the limit: refuse explicitly. Leaving the connection in the
         * backlog kept the listening socket readable and spun this loop. */
        static const char busy[] =
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,"
            "\"message\":\"Too many clients\"}}\n";
        (void)!write(client->fd, busy, sizeof(busy) - 1);
        close(client->fd);
        free(client);
        LOG_WARN("Refused client: limit of %zu reached", server->max_clients);
      } else {
        /* Unix peers passed the credential check; TCP clients still have to
         * present the token in hello */
        client->authed = server->token[0] == '\0';
        add_client(server, client);
      }
    }

    /* Handle client requests */
    time_t now = time(NULL);
    LacedClient *client = server->clients;
    while (client) {
      LacedClient *next = client->next; /* Save next in case we remove */
      bool should_remove = false;

      if (client->readable) {
        char *line;
        bool client_disconnected = false;

        while ((line = laced_client_try_read_line(client)) != NULL) {
          if (line[0] != '\0') {
            if (!process_client_request(server, client, line)) {
              /* Write failed, client probably disconnected */
              client_disconnected = true;
            }
          }
          free(line);
          if (client_disconnected) {
            break;
          }
        }

        /* Check if client disconnected */
        if (client_disconnected || !laced_client_is_connected(client)) {
          should_remove = true;
        }
      }

      /* A write failure (e.g. while delivering an async result) marks the
       * client closed outside the read path; drop it here as well. */
      if (client->closed) {
        should_remove = true;
      }

      /* Idle timeout applies only to clients that never sent a request
       * (half-open probes). A TUI holds its connection for the whole
       * session and must not be dropped while the user is idle. */
      if (!should_remove && client->requests_count == 0 &&
          (now - client->last_activity) > CLIENT_IDLE_TIMEOUT) {
        LOG_INFO("Client fd=%d idle timeout (%lds)", client->fd,
                 (long)(now - client->last_activity));
        should_remove = true;
      }

      if (should_remove) {
        remove_client(server, client);
      }

      client = next;
    }

    /* Once a second: close connections whose client has been gone longer
     * than the grace period */
    if (now != last_reap_check) {
      last_reap_check = now;
      reap_orphans(server);
    }

    /* Check idle timeout */
    if (server->idle_timeout > 0 && server->num_clients == 0 &&
        server->idle_since > 0) {
      if ((now - server->idle_since) >= server->idle_timeout) {
        LOG_INFO("Idle timeout reached (%ds with no clients), shutting down",
                 server->idle_timeout);
        break;
      }
    }
  }

  free(pfds);
  return 0;
}

int laced_server_run_unix(LacedServer *server, const char *socket_path,
                          size_t max_clients, int idle_timeout,
                          volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  /* Determine socket path */
  char *path = NULL;
  if (socket_path) {
    path = str_dup(socket_path);
  } else {
    path = laced_get_default_socket_path();
  }
  if (!path) {
    return 1;
  }

  /* Remove stale socket if present */
  if (!laced_remove_stale_socket(path)) {
    LOG_ERROR("Another daemon is already running at %s", path);
    free(path);
    return 1;
  }

  /* Create and bind socket */
  int listen_fd = laced_create_unix_socket(path, server->socket_group);
  if (listen_fd < 0) {
    LOG_ERROR("Failed to create Unix socket at %s: %s", path, strerror(errno));
    free(path);
    return 1;
  }

  /* Configure server */
  server->transport = LACED_TRANSPORT_UNIX;
  server->listen_fd = listen_fd;
  server->socket_path = path;
  server->max_clients = max_clients > 0 ? max_clients : DEFAULT_MAX_CLIENTS;
  server->idle_timeout = idle_timeout;
  server->shutdown_flag = shutdown_flag;

  LOG_INFO("Listening on Unix socket: %s (max_clients=%zu, idle_timeout=%ds)",
           path, server->max_clients, server->idle_timeout);

  /* Run event loop */
  int result = run_socket_event_loop(server, shutdown_flag);

  /* Cleanup is handled by laced_server_destroy */
  return result;
}

int laced_server_run_tcp(LacedServer *server, const char *bind_addr, int port,
                         size_t max_clients, int idle_timeout,
                         volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  /* Use default port if not specified */
  int actual_port = port > 0 ? port : 7433;
  const char *actual_addr = bind_addr ? bind_addr : "localhost";

  /* TCP has no peer credentials: clients prove themselves with a token.
   * Written before listening, so no client can arrive before it exists. */
  if (!write_token_file(server, actual_port)) {
    return 1;
  }

  /* Create and bind socket */
  int listen_fd = laced_create_tcp_socket(bind_addr, actual_port);
  if (listen_fd < 0) {
    LOG_ERROR("Failed to create TCP socket on %s:%d: %s", actual_addr,
              actual_port, strerror(errno));
    return 1;
  }

  /* Configure server */
  server->transport = LACED_TRANSPORT_TCP;
  server->listen_fd = listen_fd;
  server->max_clients = max_clients > 0 ? max_clients : DEFAULT_MAX_CLIENTS;
  server->idle_timeout = idle_timeout;
  server->shutdown_flag = shutdown_flag;

  LOG_INFO("Listening on TCP socket: %s:%d (max_clients=%zu, idle_timeout=%ds)",
           actual_addr, actual_port, server->max_clients, server->idle_timeout);

  /* Run event loop */
  int result = run_socket_event_loop(server, shutdown_flag);

  /* Cleanup is handled by laced_server_destroy */
  return result;
}
