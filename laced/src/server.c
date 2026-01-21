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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Default max clients */
#define DEFAULT_MAX_CLIENTS 64

/* Idle client timeout in seconds (5 minutes) */
#define CLIENT_IDLE_TIMEOUT 300

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
  LacedClient *clients;      /* Linked list of connected clients */
  size_t num_clients;        /* Current client count */
  size_t max_clients;        /* Maximum allowed clients */
};

/* Socket utility functions (implemented in socket.c) */
extern int laced_create_unix_socket(const char *path);
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

  /* Ensure buffer capacity */
  size_t needed = *partial_len + (size_t)n + 1;
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
  time_t now = time(NULL);
  client->connected_at = now;
  client->last_activity = now;
  client->requests_count = 0;
  client->next = server->clients;
  server->clients = client;
  server->num_clients++;
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
  }
  laced_client_free(client);
}

/* Send error response to a socket client */
static bool send_error_to_client(LacedClient *client, cJSON *id, int code,
                                  const char *message) {
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

  /* Handle the request */
  LacedHandlerResult result = laced_handler_dispatch(
      server->session, server->async_queue, method->valuestring, params, id);

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
    LacedClient *client = async_query_get_client(query);

    /* Only send response if client is still connected */
    if (client) {
      /* Verify client is still in our list */
      bool client_valid = false;
      for (LacedClient *c = server->clients; c; c = c->next) {
        if (c == client) {
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
          send_error_to_client(client, request_id, error_code,
                               error_msg ? error_msg : "Query failed");
          free(error_msg);
        }
      }
    }

    async_query_free(query);
  }
}

/* Unified socket event loop */
static int run_socket_event_loop(LacedServer *server,
                                 volatile sig_atomic_t *shutdown_flag) {
  while (!*shutdown_flag) {
    fd_set read_fds;
    FD_ZERO(&read_fds);

    /* Add async notify fd */
    FD_SET(server->async_notify_fd, &read_fds);
    int max_fd = server->async_notify_fd;

    /* Add listening socket */
    FD_SET(server->listen_fd, &read_fds);
    if (server->listen_fd > max_fd) {
      max_fd = server->listen_fd;
    }

    /* Add all client fds */
    for (LacedClient *c = server->clients; c; c = c->next) {
      FD_SET(c->fd, &read_fds);
      if (c->fd > max_fd) {
        max_fd = c->fd;
      }
    }

    /* Short timeout for checking shutdown flag */
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000}; /* 100ms */

    int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (ready < 0) {
      if (errno == EINTR) {
        continue; /* Interrupted by signal */
      }
      return 1; /* Error */
    }

    /* Handle async completions */
    if (ready > 0 && FD_ISSET(server->async_notify_fd, &read_fds)) {
      process_async_completions_socket(server);
    }

    /* Handle new connections */
    if (ready > 0 && FD_ISSET(server->listen_fd, &read_fds)) {
      if (server->num_clients < server->max_clients) {
        LacedClient *client = laced_accept_client(server->listen_fd);
        if (client) {
          add_client(server, client);
        }
      }
    }

    /* Handle client requests */
    time_t now = time(NULL);
    LacedClient *client = server->clients;
    while (client) {
      LacedClient *next = client->next; /* Save next in case we remove */
      bool should_remove = false;

      if (FD_ISSET(client->fd, &read_fds)) {
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

      /* Check for idle timeout */
      if (!should_remove && (now - client->last_activity) > CLIENT_IDLE_TIMEOUT) {
        LOG_INFO("Client fd=%d idle timeout (%lds)", client->fd,
                 (long)(now - client->last_activity));
        should_remove = true;
      }

      if (should_remove) {
        remove_client(server, client);
      }

      client = next;
    }
  }

  return 0;
}

int laced_server_run_unix(LacedServer *server, const char *socket_path,
                          size_t max_clients,
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
  int listen_fd = laced_create_unix_socket(path);
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

  LOG_INFO("Listening on Unix socket: %s (max_clients=%zu)", path,
           server->max_clients);

  /* Run event loop */
  int result = run_socket_event_loop(server, shutdown_flag);

  /* Cleanup is handled by laced_server_destroy */
  return result;
}

int laced_server_run_tcp(LacedServer *server, const char *bind_addr, int port,
                         size_t max_clients,
                         volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  /* Use default port if not specified */
  int actual_port = port > 0 ? port : 7433;
  const char *actual_addr = bind_addr ? bind_addr : "localhost";

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

  LOG_INFO("Listening on TCP socket: %s:%d (max_clients=%zu)", actual_addr,
           actual_port, server->max_clients);

  /* Run event loop */
  int result = run_socket_event_loop(server, shutdown_flag);

  /* Cleanup is handled by laced_server_destroy */
  return result;
}
