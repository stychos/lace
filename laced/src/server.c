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
#include "../../liblace/include/error.h"
#include "../../liblace/include/lace.h"
#include <util/mem.h>
#include <util/sha256.h>
#include <util/str.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <sys/stat.h>
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
  bool token_written;        /* This daemon wrote token_path (it removes it) */
  int lock_fd;               /* Unix mode: the held <socket>.lock (-1: none) */
  int orphan_grace_s;        /* How long a client's DB connections outlive its
                                last socket (--orphan-grace) */
  time_t idle_since;         /* Timestamp when last client disconnected (0=has clients) */
};

/* Socket utility functions (implemented in socket.c) */

/* ==========================================================================
 * Server Lifecycle
 * ========================================================================== */

void laced_server_set_orphan_grace(LacedServer *server, int seconds) {
  if (server && seconds >= 0)
    server->orphan_grace_s = seconds;
}

LacedServer *laced_server_create(void) {
  LacedServer *server = safe_calloc(1, sizeof(LacedServer));
  if (!server) {
    return NULL;
  }
  server->orphan_grace_s = LACED_ORPHAN_GRACE_S;
  server->lock_fd = -1;

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
  server->max_clients = LACED_DEFAULT_MAX_CLIENTS;

  server->initialized = true;
  return server;
}

void laced_server_destroy(LacedServer *server) {
  if (!server) {
    return;
  }

  /* Let queued replies (the answer to "shutdown", for one) reach their
   * clients, briefly */
  uint64_t flush_until = laced_mono_ms() + 500;
  for (bool pending = true; pending && laced_mono_ms() < flush_until;) {
    pending = false;
    for (LacedClient *c = server->clients; c; c = c->next) {
      if (c->out_len > c->out_off && laced_client_flush(c) &&
          c->out_len > c->out_off)
        pending = true;
    }
    if (pending)
      usleep(10000);
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
  /* Then let a new daemon take the path at once: the teardown below can
   * take seconds, and a client finding no socket spawns a replacement */
  if (server->lock_fd >= 0) {
    close(server->lock_fd);
    server->lock_fd = -1;
  }

  /* Stop running queries and wait for their workers before freeing what
   * they use (the queue, the session's connections). If a worker is stuck
   * in a driver call, leave both allocated: the process is exiting, and
   * freeing them under the worker would crash it. */
  /* The token file goes first: it must not outlive the daemon, even when
   * a stuck worker makes the teardown below give up */
  if (server->token_path && server->token_written) {
    unlink(server->token_path);
  }

  bool idle = async_queue_shutdown(server->async_queue, server->session,
                                   LACED_SHUTDOWN_WAIT_MS);
  if (!idle) {
    LOG_WARN("Workers still running after %d ms; exiting without teardown",
             LACED_SHUTDOWN_WAIT_MS);
    /* ssh runs in its own session: it would outlive the daemon */
    laced_session_stop_tunnels(server->session);
    free(server);
    return;
  }

  if (server->async_queue) {
    async_queue_destroy(server->async_queue);
  }

  if (server->session) {
    laced_session_destroy(server->session);
  }

  free(server->token_path);
  memset(server->token, 0, sizeof(server->token));
  free(server);
}

void laced_server_set_token_file(LacedServer *server, const char *path) {
  if (server && path) {
    free(server->token_path);
    server->token_path = str_dup(path);
  }
}

/* TCP: at LACED_MAX_UNAUTHED_CLIENTS pending handshakes, close the oldest
 * (removed on this pass), so connections that never authenticate cannot
 * keep legitimate clients out */
static void evict_oldest_unauthed(LacedServer *server) {
  size_t pending = 0;
  LacedClient *oldest = NULL;
  for (LacedClient *c = server->clients; c; c = c->next) {
    if (c->authed || c->closed) {
      continue;
    }
    pending++;
    if (!oldest || c->connected_at < oldest->connected_at ||
        (c->connected_at == oldest->connected_at && c->serial < oldest->serial)) {
      oldest = c;
    }
  }
  if (pending >= LACED_MAX_UNAUTHED_CLIENTS && oldest) {
    LOG_WARN("Dropping client fd=%d: too many unauthenticated connections",
             oldest->fd);
    oldest->closed = true;
  }
}

/* n random bytes as lowercase hex into out (2n + 1 bytes) */
static bool random_hex(char *out, size_t n) {
  unsigned char raw[32];
  if (n > sizeof(raw)) {
    return false;
  }
  int rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  bool ok = rfd >= 0 && read(rfd, raw, n) == (ssize_t)n;
  if (rfd >= 0) {
    close(rfd);
  }
  if (!ok) {
    return false;
  }
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = hex[raw[i] >> 4];
    out[2 * i + 1] = hex[raw[i] & 15];
  }
  out[2 * n] = '\0';
  memset(raw, 0, sizeof(raw));
  return true;
}

/* True if nothing is at path, or a stale token file of ours: a regular file
 * we own holding hex digits and a newline */
static bool stale_token_file(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size > 129) {
    return false;
  }
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char buf[130];
  ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n <= 0) {
    return n == 0;
  }
  for (ssize_t i = 0; i < n; i++) {
    if (!isxdigit((unsigned char)buf[i]) && !(buf[i] == '\n' && i == n - 1)) {
      return false;
    }
  }
  return true;
}

/* Create the TCP token and write it to a private file */
static bool write_token_file(LacedServer *server, int port) {
  if (!random_hex(server->token, 16)) {
    LOG_ERROR("Cannot generate the TCP token");
    return false;
  }

  if (!server->token_path) {
    char *sock = laced_get_default_socket_path(); /* Creates the directory */
    free(sock);
    server->token_path = lace_default_token_path(port);
  }
  if (!server->token_path) {
    return false;
  }
  /* A stale token from an earlier run is replaced; anything else at that
   * path (a mistyped --token-file) is left alone */
  if (!stale_token_file(server->token_path)) {
    LOG_ERROR("Refusing to replace %s: it is not a lace token file",
              server->token_path);
    return false;
  }
  unlink(server->token_path);
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
  server->token_written = ok;
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

/* Is s a lowercase or uppercase hex string of min..max digits? */
static bool is_hex_nonce(const char *s, size_t min, size_t max) {
  size_t n = 0;
  for (; s[n]; n++) {
    if (!isxdigit((unsigned char)s[n]) || n >= max) {
      return false;
    }
  }
  return n >= min;
}

/* TCP handshake, step 1: hello {client_nonce} gets {challenge: {daemon_nonce,
 * proof}}, where proof = HMAC(token, "laced-daemon:Nc:Nd") shows the client
 * this daemon knows the token. Nothing is registered yet. */
static bool tcp_issue_challenge(LacedServer *server, LacedClient *client,
                                cJSON *params, cJSON *id, bool is_notification) {
  cJSON *nc = params ? cJSON_GetObjectItem(params, "client_nonce") : NULL;
  client->auth_failures++; /* A client needs one; three end the connection */
  client->auth_nc[0] = '\0';
  if (!nc || !cJSON_IsString(nc) || !is_hex_nonce(nc->valuestring, 32, 64)) {
    LOG_WARN("Client fd=%d: hello without a valid client_nonce", client->fd);
    return is_notification ||
           send_error_to_client(client, id, LACE_ERR_AUTH_FAILED,
                                "Authentication failed: this daemon needs a "
                                "challenge handshake (client too old?)");
  }
  if (!random_hex(client->auth_nd, 16)) {
    return is_notification ||
           send_error_to_client(client, id, LACE_ERR_INTERNAL_ERROR,
                                "Cannot generate a nonce");
  }
  snprintf(client->auth_nc, sizeof(client->auth_nc), "%s", nc->valuestring);
  char proof[2 * LACE_SHA256_LEN + 1];
  lace_auth_proof(server->token, "laced-daemon", client->auth_nc,
                  client->auth_nd, proof);
  if (is_notification) {
    return true;
  }
  cJSON *res = cJSON_CreateObject();
  cJSON *ch = cJSON_AddObjectToObject(res, "challenge");
  cJSON_AddStringToObject(ch, "daemon_nonce", client->auth_nd);
  cJSON_AddStringToObject(ch, "proof", proof);
  return send_result_to_client(client, id, res);
}

/* TCP handshake, step 2: auth {proof = HMAC(token, "lace-client:Nc:Nd")}.
 * One try per challenge. */
static bool tcp_check_proof(LacedServer *server, LacedClient *client,
                            cJSON *params, cJSON *id, bool is_notification) {
  cJSON *p = params ? cJSON_GetObjectItem(params, "proof") : NULL;
  bool good = false;
  if (client->auth_nc[0] && p && cJSON_IsString(p)) {
    char want[2 * LACE_SHA256_LEN + 1];
    lace_auth_proof(server->token, "lace-client", client->auth_nc,
                    client->auth_nd, want);
    good = lace_secure_equal(p->valuestring, want);
    memset(want, 0, sizeof(want));
  }
  client->auth_nc[0] = '\0';
  client->auth_nd[0] = '\0';
  if (!good) {
    client->auth_failures++;
    LOG_WARN("Client fd=%d: bad or missing proof (%d)", client->fd,
             client->auth_failures);
    return is_notification ||
           send_error_to_client(client, id, LACE_ERR_AUTH_FAILED,
                                "Authentication failed");
  }
  client->authed = true;
  client->auth_failures = 0;
  if (is_notification) {
    return true;
  }
  cJSON *res = cJSON_CreateObject();
  cJSON_AddBoolToObject(res, "authenticated", true);
  return send_result_to_client(client, id, res);
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
    return send_error_to_client(client, NULL, LACE_ERR_PARSE_ERROR, "Parse error");
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
    return send_error_to_client(client, id, LACE_ERR_INVALID_REQUEST,
                                 "Invalid Request: must be JSON-RPC 2.0");
  }

  if (!method || !cJSON_IsString(method)) {
    cJSON_Delete(req);
    return send_error_to_client(client, id, LACE_ERR_INVALID_REQUEST,
                                 "Invalid Request: missing method");
  }

  /* Check if this is a notification (no id) */
  bool is_notification = (id == NULL);

  /* TCP: a mutual challenge before anything else. The token itself never
   * crosses the wire, so a process squatting on the port learns nothing
   * it could use, and the client sends nothing (passwords included) to a
   * daemon that cannot prove it knows the token. */
  if (!client->authed && server->token[0]) {
    bool ok;
    if (strcmp(method->valuestring, "hello") == 0) {
      ok = tcp_issue_challenge(server, client, params, id, is_notification);
    } else if (strcmp(method->valuestring, "auth") == 0) {
      ok = tcp_check_proof(server, client, params, id, is_notification);
    } else {
      /* Counts as a failed attempt: a peer cannot hold a slot by sending
       * anything but the handshake */
      client->auth_failures++;
      ok = is_notification ||
           send_error_to_client(client, id, LACE_ERR_AUTH_REQUIRED,
                                "Authentication required: hello, then auth");
    }
    if (client->auth_failures >= 3) {
      client->closed = true;
    }
    cJSON_Delete(req);
    return ok;
  }

  /* hello: the client names itself (liblace sends a random id per client
   * handle, shared by all its sockets). Connections it opens stay alive as
   * long as any socket with that id is connected. */
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
           send_error_to_client(client, id, LACE_ERR_INVALID_PARAMS, "Invalid 'client_id'");
    } else {
      snprintf(client->owner, sizeof(client->owner), "u:%s", cid->valuestring);
      cJSON *res = cJSON_CreateObject();
      cJSON_AddNumberToObject(res, "orphan_grace_seconds", server->orphan_grace_s);
      ok = is_notification || send_result_to_client(client, id, res);
      if (is_notification) {
        cJSON_Delete(res);
      }
    }
    cJSON_Delete(req);
    return ok;
  }

  /* Handle the request; jobs it starts belong to this client */
  LacedRequest rq = {
      .session = server->session,
      .queue = server->async_queue,
      .params = params,
      .id = id,
      .from = {.serial = client->serial, .owner = client->owner},
      .shutdown_flag = server->shutdown_flag,
  };
  LacedHandlerResult result = laced_handler_dispatch(&rq, method->valuestring);

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
        char *line = async_query_take_reply_line(query);
        if (line) {
          laced_client_write_line(client, line);
          free(line);
        } else if (status == ASYNC_QUERY_COMPLETED) {
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
  LacedSession *session;
  int conn_id;
  bool ran; /* The job started (else its reaping mark must be released) */
} ReapArg;

/* Freed with the job: one that never ran (refused for lack of workers, or
 * never started) gives the connection back to the next reaper pass */
static void reap_arg_free(void *p) {
  ReapArg *a = p;
  if (a && !a->ran)
    laced_session_reap_failed(a->session, a->conn_id);
  free(a);
}

static cJSON *reap_job(LacedSession *session, void *p, char **err,
                       int *err_code) {
  ReapArg *a = p;
  a->ran = true;
  for (int attempt = 0; attempt < 150; attempt++) { /* ~30 s */
    if (!laced_session_reap_continue(session, a->conn_id)) {
      /* Its owner came back (or someone else closed it): leave it be */
      return cJSON_CreateObject();
    }
    char *derr = NULL;
    if (laced_session_disconnect(session, a->conn_id, &derr)) {
      LOG_INFO("conn_id=%d closed: its client is gone", a->conn_id);
      return cJSON_CreateObject();
    }
    free(derr);
    if (attempt % 25 == 0) {
      char *cerr = NULL;
      (void)async_cancel_by_conn_id(a->queue, session, a->conn_id, NULL, NULL,
                                    &cerr);
      free(cerr);
    }
    usleep(200000);
  }
  laced_session_reap_failed(session, a->conn_id);
  *err_code = LACE_ERR_INTERNAL_ERROR;
  *err = str_dup("Orphaned connection did not stop; will retry");
  return NULL;
}

static void reap_orphans(LacedServer *server) {
  /* Every open client counts: an owner left out of this list loses its
   * connections (and open transactions) after the grace period */
  size_t n = 0;
  for (LacedClient *c = server->clients; c; c = c->next) {
    n++;
  }
  const char **live = safe_malloc((n ? n : 1) * sizeof(*live));
  n = 0;
  for (LacedClient *c = server->clients; c; c = c->next) {
    if (!c->closed) {
      live[n++] = c->owner;
    }
  }
  int ids[LACED_MAX_CONNECTIONS];
  size_t found = laced_session_collect_orphans(
      server->session, live, n, server->orphan_grace_s, ids,
      LACED_MAX_CONNECTIONS);
  free(live);
  for (size_t i = 0; i < found; i++) {
    ReapArg *arg = safe_calloc(1, sizeof(ReapArg));
    arg->queue = server->async_queue;
    arg->session = server->session;
    arg->conn_id = ids[i];
    /* No request id: nobody is waiting for this reply. If the job never
     * runs, reap_arg_free releases the connection */
    (void)async_job_start(server->async_queue, server->session, NULL, NULL,
                          reap_job, arg, reap_arg_free);
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
    size_t want = 2 + 2 * server->num_clients;
    if (want > pfds_cap) {
      pfds_cap = want * 2;
      pfds = safe_realloc(pfds, pfds_cap * sizeof(struct pollfd));
    }
    size_t nfds = 0;
    pfds[nfds++] = (struct pollfd){.fd = server->async_notify_fd, .events = POLLIN};
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms = (uint64_t)tv_now.tv_sec * 1000 + (uint64_t)tv_now.tv_usec / 1000;
    /* No listening socket in stdio mode: its one client is all there is */
    bool listen_polled =
        server->listen_fd >= 0 && now_ms >= accept_paused_until;
    if (listen_polled) {
      pfds[nfds++] = (struct pollfd){.fd = server->listen_fd, .events = POLLIN};
    }
    bool requests_waiting = false;
    for (LacedClient *c = server->clients; c; c = c->next) {
      bool pending = c->out_len > c->out_off; /* Flush when it can take more */
      bool split = c->out_fd >= 0 && c->out_fd != c->fd; /* stdin/stdout */
      c->poll_in = (int)nfds;
      pfds[nfds++] = (struct pollfd){
          .fd = c->fd, .events = (short)(POLLIN | (pending && !split ? POLLOUT : 0))};
      c->poll_out = -1;
      if (pending) {
        c->poll_out = split ? (int)nfds : c->poll_in;
        if (split)
          pfds[nfds++] = (struct pollfd){.fd = c->out_fd, .events = POLLOUT};
      }
      requests_waiting |= c->has_buffered;
    }

    /* Short timeout for checking the shutdown flag; none while a client
     * still has requests buffered from its last turn */
    int ready = poll(pfds, (nfds_t)nfds, requests_waiting ? 0 : 100);

    if (ready < 0) {
      if (errno == EINTR) {
        continue; /* Interrupted by signal */
      }
      free(pfds);
      return 1; /* Error */
    }

    /* Record readiness on the clients before the list can change */
    for (LacedClient *c = server->clients; c; c = c->next) {
      c->readable =
          (pfds[c->poll_in].revents & (POLLIN | POLLHUP | POLLERR)) != 0 ||
          c->has_buffered;
      if (c->poll_out >= 0 &&
          (pfds[c->poll_out].revents & (POLLOUT | POLLERR | POLLHUP))) {
        laced_client_flush(c);
      }
      /* Read the clock after the flush: progress it made is newer than any
       * time taken before it (unsigned: never subtract a later time) */
      uint64_t now_ms_c = laced_mono_ms();
      if (c->out_len > c->out_off && !c->closed &&
          now_ms_c > c->out_progress_ms &&
          now_ms_c - c->out_progress_ms > LACED_WRITE_TIMEOUT_MS) {
        LOG_WARN("Client fd=%d stopped reading its replies; dropping it",
                 c->fd);
        c->closed = true;
      }
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
        /* -32051 = LACE_ERR_TOO_MANY_CONNS */
        static const char busy[] =
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32051,"
            "\"message\":\"Too many clients\"}}\n";
        (void)!write(client->fd, busy, sizeof(busy) - 1);
        close(client->fd);
        free(client);
        LOG_WARN("Refused client: limit of %zu reached", server->max_clients);
      } else {
        /* Unix peers passed the credential check; TCP clients still have to
         * complete the handshake */
        client->authed = server->token[0] == '\0';
        if (!client->authed) {
          evict_oldest_unauthed(server);
        }
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

        /* A bounded turn per client: one pipelining thousands of requests
         * must not hold up the others (the rest waits for its next turn) */
        int served = 0;
        client->has_buffered = false;
        while (served < LACED_REQUESTS_PER_TURN &&
               (line = laced_client_try_read_line(client)) != NULL) {
          served++;
          if (line[0] != '\0') {
            if (!process_client_request(server, client, line)) {
              /* Write failed, client probably disconnected */
              client_disconnected = true;
            }
          }
          free(line);
          if (client_disconnected || client->closed) {
            break; /* Nothing more for a client being dropped (3 strikes) */
          }
        }
        client->has_buffered = served == LACED_REQUESTS_PER_TURN;

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
      /* TCP: authenticate soon or leave (no peer credentials to go by,
       * and every slot held is one a legitimate client cannot have) */
      if (!should_remove && !client->authed &&
          now - client->connected_at > LACED_AUTH_TIMEOUT_S) {
        LOG_WARN("Client fd=%d did not authenticate in %ds; dropping it",
                 client->fd, LACED_AUTH_TIMEOUT_S);
        should_remove = true;
      }
      if (!should_remove && client->requests_count == 0 &&
          (now - client->last_activity) > LACED_CLIENT_IDLE_TIMEOUT_S) {
        LOG_INFO("Client fd=%d idle timeout (%lds)", client->fd,
                 (long)(now - client->last_activity));
        should_remove = true;
      }

      if (should_remove) {
        remove_client(server, client);
      }

      client = next;
    }

    /* stdio: the daemon belongs to its one client and ends with it */
    if (server->transport == LACED_TRANSPORT_STDIO && server->num_clients == 0) {
      break;
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

/* stdio mode: the socket event loop with a single client that reads stdin
 * and writes stdout (no listening socket). One request path, one reply path
 * and the same checks ("hello", owners, limits) as the socket modes. */
int laced_server_run_stdio(LacedServer *server,
                           volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  int in_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  int out_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, in_flags | O_NONBLOCK);
  fcntl(STDOUT_FILENO, F_SETFL, out_flags | O_NONBLOCK);

  LacedClient *client = safe_calloc(1, sizeof(LacedClient));
  client->fd = STDIN_FILENO;
  client->out_fd = STDOUT_FILENO;
  client->authed = true; /* Its parent spawned this daemon for itself */

  server->transport = LACED_TRANSPORT_STDIO;
  server->listen_fd = -1;
  server->max_clients = 1;
  server->idle_timeout = 0;
  server->shutdown_flag = shutdown_flag;
  add_client(server, client);

  int result = run_socket_event_loop(server, shutdown_flag);

  /* stdin/stdout are closed with the client; restore if still open */
  fcntl(STDIN_FILENO, F_SETFL, in_flags);
  fcntl(STDOUT_FILENO, F_SETFL, out_flags);
  return result;
}

int laced_server_run_unix(LacedServer *server, const char *socket_path,
                          size_t max_clients, int idle_timeout,
                          volatile sig_atomic_t *shutdown_flag) {
  if (!server || !server->initialized) {
    return 1;
  }

  /* Determine socket path */
  /* The default path (also when a spawning client passes it explicitly)
   * gets its private directory created and checked */
  char *path = NULL;
  char *default_path = lace_get_default_socket_path();
  if (!socket_path || (default_path && strcmp(socket_path, default_path) == 0)) {
    path = laced_get_default_socket_path();
  } else {
    path = str_dup(socket_path);
  }
  free(default_path);
  if (!path) {
    return 1;
  }

  /* One daemon per socket: an exclusive lock on <socket>.lock, held for the
   * daemon's life. The connect probe below cannot tell on its own: a live
   * daemon between bind and listen, or with a full backlog, refuses it,
   * and its socket would be removed from under it. */
  char *lock_path = str_printf("%s.lock", path);
  int lock_fd = lock_path ? open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600) : -1;
  free(lock_path);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    LOG_ERROR("Another daemon is already running at %s", path);
    if (lock_fd >= 0)
      close(lock_fd);
    free(path);
    return 1;
  }
  server->lock_fd = lock_fd;

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
  server->max_clients = max_clients > 0 ? max_clients : LACED_DEFAULT_MAX_CLIENTS;
  server->idle_timeout = idle_timeout;
  server->shutdown_flag = shutdown_flag;

  laced_signal_ready(); /* A waiting "laced -d" parent can exit 0 */
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

  /* Bind first: a second daemon on a taken port must fail without
   * touching the running one's token file */
  int listen_fd = laced_create_tcp_socket(bind_addr, actual_port);
  if (listen_fd < 0) {
    LOG_ERROR("Failed to create TCP socket on %s:%d: %s", actual_addr,
              actual_port, strerror(errno));
    return 1;
  }

  /* TCP has no peer credentials: clients prove themselves with a token.
   * Written before the event loop accepts anyone. */
  if (!write_token_file(server, actual_port)) {
    close(listen_fd);
    return 1;
  }

  /* Configure server */
  server->transport = LACED_TRANSPORT_TCP;
  server->listen_fd = listen_fd;
  server->max_clients = max_clients > 0 ? max_clients : LACED_DEFAULT_MAX_CLIENTS;
  server->idle_timeout = idle_timeout;
  server->shutdown_flag = shutdown_flag;

  laced_signal_ready(); /* A waiting "laced -d" parent can exit 0 */
  LOG_INFO("Listening on TCP socket: %s:%d (max_clients=%zu, idle_timeout=%ds)",
           actual_addr, actual_port, server->max_clients, server->idle_timeout);

  /* Run event loop */
  int result = run_socket_event_loop(server, shutdown_flag);

  /* Cleanup is handled by laced_server_destroy */
  return result;
}
