/*
 * liblace - Lace Client Library
 * JSON-RPC marshaling implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "rpc.h"
#include "../include/util/mem.h"
#include "../include/util/sha256.h"
#include "os/os.h"
#include "../include/util/str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_internal.h"
#include <errno.h>
#include <stdint.h>

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Set error message in client */
/* A JSON number as an integer: converting a double outside the target's
 * range is undefined, and the daemon (or whatever answered) is not
 * trusted to stay in range. False for non-numbers, non-integers, NaN and
 * values out of range. */
static bool json_int64(const cJSON *n, int64_t *out) {
  if (!n || !cJSON_IsNumber(n))
    return false;
  double d = n->valuedouble;
  /* -2^63 is exact as a double; 2^63 is the first value past INT64_MAX */
  if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0) || d != (double)(int64_t)d)
    return false;
  *out = (int64_t)d;
  return true;
}

static bool json_size(const cJSON *n, size_t *out) {
  int64_t v;
  if (!json_int64(n, &v) || v < 0 || (uint64_t)v > SIZE_MAX)
    return false;
  *out = (size_t)v;
  return true;
}

static void rpc_set_error(lace_client_t *client, const char *msg) {
  (void)client;
  lace_set_thread_error(msg);
}

/* ==========================================================================
 * Link I/O
 * ========================================================================== */

/* Milliseconds left until deadline_ms (monotonic); -1 = no deadline */
static int64_t now_ms(void) { return lace_os_now_ms(); }

static int remaining_ms(int64_t deadline_ms) {
  if (deadline_ms < 0) {
    return -1;
  }
  int64_t left = deadline_ms - now_ms();
  return left > 0 ? (int)(left > INT32_MAX ? INT32_MAX : left) : 0;
}

/* Read one newline-terminated line through the link's buffer.
 * Returns LACE_OK (*out allocated), LACE_ERR_TIMEOUT or
 * LACE_ERR_CONNECTION_LOST. */
static int link_read_line(LaceLink *l, int64_t deadline_ms, char **out) {
  *out = NULL;
  for (;;) {
    for (size_t i = l->scanned; i < l->len; i++) {
      if (l->buf[i] == '\n') {
        char *line = safe_malloc(i + 1);
        memcpy(line, l->buf, i);
        line[i] = '\0';
        size_t rest = l->len - i - 1;
        memmove(l->buf, l->buf + i + 1, rest);
        l->len = rest;
        l->scanned = 0;
        *out = line;
        return LACE_OK;
      }
    }
    l->scanned = l->len;

    int r = lace_os_wait(l->rfd, l->is_socket, false, remaining_ms(deadline_ms));
    if (r < 0) {
      return LACE_ERR_CONNECTION_LOST;
    }
    if (r == 0) {
      return LACE_ERR_TIMEOUT;
    }

    if (l->cap - l->len < 65536) {
      size_t new_cap = l->cap ? l->cap * 2 : 65536;
      while (new_cap - l->len < 65536) {
        new_cap *= 2;
      }
      l->buf = safe_realloc(l->buf, new_cap);
      l->cap = new_cap;
    }
    int64_t n = lace_os_read(l->rfd, l->is_socket, l->buf + l->len, l->cap - l->len);
    if (n == -2) {
      continue;
    }
    if (n <= 0) {
      return LACE_ERR_CONNECTION_LOST;
    }
    l->len += (size_t)n;
  }
}

/* Write all of data. Returns false on any failure (the daemon cannot have
 * seen a request that was not fully written). */
static bool link_write_all(LaceLink *l, const char *data, size_t len,
                           int64_t deadline_ms) {
  while (len > 0) {
    if (lace_os_wait(l->wfd, l->is_socket, true, remaining_ms(deadline_ms)) <= 0) {
      return false;
    }
    int64_t n = lace_os_write(l->wfd, l->is_socket, data, len);
    if (n == -2) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    data += n;
    len -= (size_t)n;
  }
  return true;
}

/* True if the daemon end of an idle link has closed (EOF or error pending).
 * Never blocks. Data waiting to be read is not a hangup. */
static bool link_is_stale(const LaceLink *l) {
  if (l->len > 0) {
    return false; /* Buffered bytes: nothing to read means alive */
  }
  return lace_os_hung_up(l->rfd, l->is_socket);
}

/* ==========================================================================
 * RPC Exchange
 * ========================================================================== */

/* Reply timeout for a method: queries may legitimately run for hours (they
 * can be cancelled), connects include SSH tunnels; everything else is a
 * quick control call that a healthy daemon answers at once. */
static int64_t method_deadline(const lace_client_t *client, const char *method) {
  int ms = client->timeout_ms; /* Atomic: read once */
  if (strcmp(method, "query") == 0) {
    ms = client->query_timeout_ms;
  } else if (strcmp(method, "connect") == 0 && ms < 60000) {
    ms = 60000;
  }
  return ms > 0 ? now_ms() + ms : -1;
}

/* Send a request on a link. Returns LACE_OK, or LACE_ERR_PIPE_ERROR if it
 * could not be written (so the daemon cannot have seen it). */
static int rpc_send(LaceLink *l, int64_t req_id, const char *method,
                    cJSON *params, int64_t deadline_ms) {
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(req, "id", (double)req_id);
  cJSON_AddStringToObject(req, "method", method);
  if (params) {
    cJSON_AddItemReferenceToObject(req, "params", params);
  }
  char *req_str = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!req_str) {
    return LACE_ERR_OUT_OF_MEMORY;
  }
  size_t n = strlen(req_str);
  req_str[n] = '\n'; /* Overwrites the terminator: send exactly n+1 bytes */
  bool ok = link_write_all(l, req_str, n + 1, deadline_ms);
  req_str[n] = '\0';
  str_secure_free(req_str); /* May contain passwords or cell data */
  return ok ? LACE_OK : LACE_ERR_PIPE_ERROR;
}

/* Read until the reply carrying req_id arrives. Anything else (a late reply
 * to an abandoned request) is discarded. *broken: the link itself failed
 * (no reply, timeout, garbage), as opposed to an error the daemon sent:
 * only then is it discarded. The daemon's own codes include
 * LACE_ERR_CONNECTION_LOST for a lost *database* connection, which must
 * not drop the link (for a stdio daemon that ends the daemon). */
static int rpc_receive(lace_client_t *client, LaceLink *l, int64_t req_id,
                       int64_t deadline_ms, cJSON **result, bool *broken) {
  *broken = false;
  cJSON *resp = NULL;
  for (;;) {
    char *line = NULL;
    int rc = link_read_line(l, deadline_ms, &line);
    if (rc == LACE_ERR_TIMEOUT) {
      rpc_set_error(client, "Timed out waiting for the lace daemon");
      *broken = true; /* A late reply must not answer a later request */
      return rc;
    }
    if (rc != LACE_OK) {
      rpc_set_error(client, "No response from lace daemon");
      *broken = true;
      return LACE_ERR_CONNECTION_LOST;
    }
    resp = cJSON_Parse(line);
    free(line);
    if (!resp) {
      rpc_set_error(client, "Invalid JSON response from lace daemon");
      *broken = true;
      return LACE_ERR_PARSE_ERROR;
    }
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    int64_t got_id;
    if (json_int64(id, &got_id) && got_id == req_id) {
      break;
    }
    if (id && cJSON_IsNull(id) && cJSON_GetObjectItem(resp, "error")) {
      break; /* Daemon could not parse our request, or refused us */
    }
    cJSON_Delete(resp);
  }

  cJSON *error = cJSON_GetObjectItem(resp, "error");
  if (error && cJSON_IsObject(error)) {
    cJSON *msg = cJSON_GetObjectItem(error, "message");
    cJSON *code = cJSON_GetObjectItem(error, "code");
    rpc_set_error(client, (msg && cJSON_IsString(msg)) ? msg->valuestring
                                                       : "Daemon error");
    /* A failed statement still reports the transaction it left behind */
    cJSON *data = cJSON_GetObjectItem(error, "data");
    cJSON *in_tx = data ? cJSON_GetObjectItem(data, "in_transaction") : NULL;
    if (in_tx && cJSON_IsBool(in_tx)) {
      lace_set_thread_tx_state(
          !cJSON_IsTrue(in_tx) ? 0
          : cJSON_IsTrue(cJSON_GetObjectItem(data, "transaction_failed")) ? 2
                                                                          : 1);
    }
    int err_code = LACE_ERR_INTERNAL_ERROR;
    if (code && cJSON_IsNumber(code)) {
      int json_code = code->valueint;
      /* Pass through every code liblace defines; keep unknown ones generic */
      if ((json_code <= -32000 && json_code >= -32099) ||
          json_code == LACE_ERR_PARSE_ERROR ||
          json_code == LACE_ERR_INVALID_REQUEST ||
          json_code == LACE_ERR_METHOD_NOT_FOUND ||
          json_code == LACE_ERR_INVALID_PARAMS) {
        err_code = json_code;
      }
    }
    cJSON_Delete(resp);
    return err_code;
  }

  if (result) {
    /* Detach instead of duplicating: results can be very large */
    *result = cJSON_DetachItemFromObject(resp, "result");
  }
  cJSON_Delete(resp);
  return LACE_OK;
}

/* One complete exchange on a link */
static int rpc_exchange(lace_client_t *client, LaceLink *l, const char *method,
                        cJSON *params, cJSON **result) {
  int64_t deadline = method_deadline(client, method);
  int64_t req_id = l->next_id++;
  int rc = rpc_send(l, req_id, method, params, deadline);
  if (rc != LACE_OK) {
    rpc_set_error(client, "Failed to send request to lace daemon");
    return rc;
  }
  bool broken = false;
  return rpc_receive(client, l, req_id, deadline, result, &broken);
}

/* ==========================================================================
 * Link Pool
 * ========================================================================== */

/* Prepare a new link: name this client to the daemon, and check whether the
 * daemon is still the one this client knew. A different daemon (respawned
 * by us, or by another frontend sharing the socket) has an unrelated
 * connection pool whose ids restart at 1, so any conn_id from before would
 * address someone else's database: move the generation on. */
/* The token for a TCP link: set explicitly, else $LACE_TOKEN, else the
 * daemon's default token file for this port. Caller frees. */
static char *tcp_token(lace_client_t *client) {
  pthread_mutex_lock(&client->lock);
  char *tok = client->token ? str_dup(client->token) : NULL;
  int port = client->tcp_port;
  pthread_mutex_unlock(&client->lock);
  if (tok) {
    return tok;
  }
  const char *env = getenv("LACE_TOKEN");
  if (env && env[0]) {
    return str_dup(env);
  }
  char *path = lace_default_token_path(port);
  tok = lace_read_token_file(path);
  free(path);
  return tok;
}

/* TCP: prove to each other that both sides know the token, without sending
 * it. The daemon answers hello {client_nonce} with its nonce and
 * HMAC(token, "laced-daemon:Nc:Nd"); only if that checks out does the client
 * send auth {HMAC(token, "lace-client:Nc:Nd")}. A process squatting on the
 * port gets neither the token nor, later, any database password. */
static int tcp_handshake(lace_client_t *client, LaceLink *l) {
  char *token = tcp_token(client);
  if (!token || !token[0]) {
    str_secure_free(token);
    rpc_set_error(client, "No token for the lace daemon's TCP port "
                          "(set LACE_TOKEN or the token file)");
    return LACE_ERR_AUTH_REQUIRED;
  }
  uint8_t raw[16];
  if (!lace_os_random(raw, sizeof(raw))) {
    str_secure_free(token);
    rpc_set_error(client, "Cannot generate a nonce");
    return LACE_ERR_INTERNAL_ERROR;
  }
  char nc[2 * sizeof(raw) + 1];
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(raw); i++) {
    nc[2 * i] = hex[raw[i] >> 4];
    nc[2 * i + 1] = hex[raw[i] & 15];
  }
  nc[2 * sizeof(raw)] = '\0';

  cJSON *hello = cJSON_CreateObject();
  cJSON_AddStringToObject(hello, "client_id", client->client_id);
  cJSON_AddStringToObject(hello, "client_nonce", nc);
  cJSON *res = NULL;
  int rc = rpc_exchange(client, l, "hello", hello, &res);
  cJSON_Delete(hello);
  if (rc != LACE_OK) {
    cJSON_Delete(res);
    str_secure_free(token);
    return rc;
  }
  cJSON *ch = res ? cJSON_GetObjectItem(res, "challenge") : NULL;
  cJSON *nd = ch ? cJSON_GetObjectItem(ch, "daemon_nonce") : NULL;
  cJSON *proof = ch ? cJSON_GetObjectItem(ch, "proof") : NULL;
  char want[2 * LACE_SHA256_LEN + 1];
  bool good = false;
  if (nd && cJSON_IsString(nd) && proof && cJSON_IsString(proof)) {
    lace_auth_proof(token, "laced-daemon", nc, nd->valuestring, want);
    good = lace_secure_equal(proof->valuestring, want);
  }
  if (!good) {
    cJSON_Delete(res);
    str_secure_free(token);
    rpc_set_error(client, "The process on the lace daemon's TCP port did not "
                          "prove it knows our token (a stale token, or not "
                          "the lace daemon): refusing to use it");
    return LACE_ERR_AUTH_FAILED;
  }
  lace_auth_proof(token, "lace-client", nc, nd->valuestring, want);
  cJSON_Delete(res);
  str_secure_free(token);

  cJSON *auth = cJSON_CreateObject();
  cJSON_AddStringToObject(auth, "proof", want);
  res = NULL;
  rc = rpc_exchange(client, l, "auth", auth, &res);
  cJSON_Delete(auth);
  cJSON_Delete(res);
  return rc;
}

int lace_link_initialize(lace_client_t *client, LaceLink *l, bool respawned) {
  bool tcp = l->is_socket && l->tcp;
  if (tcp) {
    int rc = tcp_handshake(client, l);
    if (rc != LACE_OK) {
      return rc;
    }
  }
  /* Now name this client (on TCP the daemon registers it only after the
   * handshake) */
  cJSON *hello = cJSON_CreateObject();
  cJSON_AddStringToObject(hello, "client_id", client->client_id);
  cJSON *res = NULL;
  int rc = rpc_exchange(client, l, "hello", hello, &res);
  cJSON_Delete(hello);
  cJSON_Delete(res);
  if (rc == LACE_ERR_TIMEOUT || rc == LACE_ERR_CONNECTION_LOST ||
      rc == LACE_ERR_PIPE_ERROR || rc == LACE_ERR_PARSE_ERROR) {
    return rc;
  }
  /* An older daemon answers "method not found": fine */

  char inst[sizeof(client->daemon_instance)] = "";
  res = NULL;
  rc = rpc_exchange(client, l, "version", NULL, &res);
  if (rc != LACE_OK) {
    cJSON_Delete(res);
    return rc;
  }
  cJSON *iv = res ? cJSON_GetObjectItem(res, "instance") : NULL;
  if (iv && cJSON_IsString(iv) && iv->valuestring) {
    snprintf(inst, sizeof(inst), "%s", iv->valuestring);
  }
  cJSON_Delete(res);

  pthread_mutex_lock(&client->lock);
  /* A new generation only for a daemon different from the one known: a
   * thread that reaches a just-respawned daemon first has already moved
   * the generation on, and a second bump would orphan the conn_ids opened
   * in between */
  if (!client->daemon_instance[0]) {
    /* First link of this client: learn the daemon's identity (a respawn
     * before any was known still starts a new generation) */
    snprintf(client->daemon_instance, sizeof(client->daemon_instance), "%s", inst);
    if (respawned) {
      client->generation++;
      client->num_connections = 0;
    }
  } else if (inst[0] == '\0' || strcmp(inst, client->daemon_instance) != 0) {
    client->generation++;
    client->num_connections = 0;
    snprintf(client->daemon_instance, sizeof(client->daemon_instance), "%s", inst);
  }
  pthread_mutex_unlock(&client->lock);

  l->initialized = true;
  return LACE_OK;
}

/* Get a link for one exchange: an idle one, or a new one. With wait=false a
 * single-link (stdio) client that is busy fails with LACE_ERR_TIMEOUT. */
static int borrow_link(lace_client_t *client, bool wait, LaceLink **out) {
  *out = NULL;
  pthread_mutex_lock(&client->lock);
  for (;;) {
    while (client->idle_links) {
      LaceLink *l = client->idle_links;
      client->idle_links = l->next;
      client->num_idle--;
      l->next = NULL;
      if (l->epoch == client->link_epoch && !link_is_stale(l)) {
        pthread_mutex_unlock(&client->lock);
        *out = l;
        break;
      }
      /* The daemon dropped it (restart, reaping), or detect has moved on to
       * another daemon: discard and try another.
       * (client->lock then write_lock: nothing takes them the other way) */
      lace_client_forget_stdio(client, l);
      lace_link_free(l);
      client->num_links--;
    }
    if (*out) {
      break;
    }
    if (client->single_link && client->num_links > 0) {
      if (!wait) {
        pthread_mutex_unlock(&client->lock);
        rpc_set_error(client, "Another call is in progress");
        return LACE_ERR_TIMEOUT;
      }
      pthread_cond_wait(&client->link_released, &client->lock);
      continue;
    }
    pthread_mutex_unlock(&client->lock);

    /* Open a new link outside the lock (connecting may take a while); it
     * comes back already counted in num_links */
    LaceLink *l = NULL;
    int how = lace_client_open_link(client, &l);
    if (how == LACE_OPEN_LINK_WAIT) {
      pthread_mutex_lock(&client->lock); /* Back to waiting for the link */
      continue;
    }
    if (how == 0 || !l) {
      rpc_set_error(client, "Lost connection to lace daemon");
      return LACE_ERR_CONNECTION_LOST;
    }
    *out = l;
    if (how == 2) {
      /* A fresh daemon: initialize now so the generation moves on */
      int rc = lace_link_initialize(client, l, true);
      if (rc != LACE_OK) {
        pthread_mutex_lock(&client->lock);
        client->num_links--;
        pthread_cond_broadcast(&client->link_released);
        pthread_mutex_unlock(&client->lock);
        lace_client_forget_stdio(client, l);
        lace_link_free(l);
        *out = NULL;
        return rc;
      }
    }
    break;
  }

  if (!(*out)->initialized) {
    int rc = lace_link_initialize(client, *out, false);
    if (rc != LACE_OK) {
      pthread_mutex_lock(&client->lock);
      client->num_links--;
      pthread_cond_broadcast(&client->link_released);
      pthread_mutex_unlock(&client->lock);
      lace_client_forget_stdio(client, *out);
      lace_link_free(*out);
      *out = NULL;
      return rc;
    }
  }
  return LACE_OK;
}

/* Return a link to the pool, or drop it when it is broken or out of sync
 * (a timed-out request may still get its reply later on that link). */
static void release_link(lace_client_t *client, LaceLink *l, bool broken) {
  pthread_mutex_lock(&client->lock);
  /* A link from before detect moved to another daemon is never reused: its
   * daemon may hold other databases under the same conn_ids */
  if (broken || l->epoch != client->link_epoch ||
      (!client->single_link && client->num_idle >= LACE_CLIENT_MAX_IDLE_LINKS)) {
    client->num_links--;
    pthread_cond_broadcast(&client->link_released);
    pthread_mutex_unlock(&client->lock);
    lace_client_forget_stdio(client, l);
    lace_link_free(l);
    return;
  }
  l->next = client->idle_links;
  client->idle_links = l;
  client->num_idle++;
  pthread_cond_broadcast(&client->link_released);
  pthread_mutex_unlock(&client->lock);
}

/* Methods whose params carry a conn_id from a specific daemon generation */
static bool method_uses_conn_id(const char *method) {
  return strcmp(method, "query") == 0 || strcmp(method, "disconnect") == 0 ||
         strcmp(method, "cancel") == 0;
}

static int rpc_call_impl(lace_client_t *client, const char *method,
                         cJSON *params, cJSON **result, bool wait) {
  if (result) {
    *result = NULL;
  }
  if (!client || !method) {
    return LACE_ERR_INVALID_PARAMS;
  }
  if (!client->connected) {
    rpc_set_error(client, "Client is not connected to lace daemon");
    return LACE_ERR_CONNECTION_CLOSED;
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    LaceLink *l = NULL;
    int rc = borrow_link(client, wait, &l);
    if (rc != LACE_OK) {
      return rc;
    }

    /* A caller that captured its conn_id under an older generation (the
     * daemon was replaced meanwhile) must not send it to the new daemon,
     * where the same number may belong to a different database. */
    if (method_uses_conn_id(method) && lace_thread_generation_mismatch(client)) {
      release_link(client, l, false);
      rpc_set_error(client, "Lace daemon was restarted; reconnecting");
      return LACE_ERR_CONNECTION_LOST;
    }

    int64_t deadline = method_deadline(client, method);
    int64_t req_id = l->next_id++;
    /* The stdio pipe also carries notifications from other threads */
    if (!l->is_socket)
      pthread_mutex_lock(&client->write_lock);
    rc = rpc_send(l, req_id, method, params, deadline);
    if (!l->is_socket)
      pthread_mutex_unlock(&client->write_lock);
    if (rc == LACE_ERR_PIPE_ERROR && attempt == 0) {
      /* Not written, so the daemon never saw it: retry on a fresh link */
      release_link(client, l, true);
      continue;
    }
    if (rc != LACE_OK) {
      release_link(client, l, true);
      rpc_set_error(client, "Failed to send request to lace daemon");
      return rc;
    }
    bool broken = false;
    rc = rpc_receive(client, l, req_id, deadline, result, &broken);
    release_link(client, l, broken);
    return rc;
  }
  return LACE_ERR_PIPE_ERROR;
}

int lace_rpc_call(lace_client_t *client, const char *method, cJSON *params,
                  cJSON **result) {
  return rpc_call_impl(client, method, params, result, true);
}

int lace_rpc_try_call(lace_client_t *client, const char *method,
                      cJSON *params, cJSON **result) {
  return rpc_call_impl(client, method, params, result, false);
}

int lace_rpc_notify(lace_client_t *client, const char *method, cJSON *params) {
  if (!client || !method)
    return LACE_ERR_INVALID_REQUEST;
  pthread_mutex_lock(&client->lock);
  bool single = client->single_link;
  pthread_mutex_unlock(&client->lock);
  if (!single)
    return LACE_ERR_INVALID_REQUEST;
  /* As for calls: a conn_id from before a daemon change must not reach the
   * new daemon (detect mode can replace a stdio daemon too) */
  if (method_uses_conn_id(method) && lace_thread_generation_mismatch(client)) {
    rpc_set_error(client, "Connection lost: the lace daemon was replaced");
    return LACE_ERR_CONNECTION_LOST;
  }
  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddStringToObject(req, "method", method);
  if (params)
    cJSON_AddItemReferenceToObject(req, "params", params);
  char *line = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!line)
    return LACE_ERR_OUT_OF_MEMORY;
  size_t n = strlen(line);
  line[n] = '\n';
  /* Under write_lock: the fd is cleared there before the link is freed */
  pthread_mutex_lock(&client->write_lock);
  LaceLink pipe_end = {.rfd = LACE_OS_INVALID, .wfd = client->stdio_wfd};
  bool ok = pipe_end.wfd != LACE_OS_INVALID &&
            link_write_all(&pipe_end, line, n + 1,
                           now_ms() + LACE_DEFAULT_CONNECT_TIMEOUT_MS);
  pthread_mutex_unlock(&client->write_lock);
  line[n] = '\0';
  free(line);
  return ok ? LACE_OK : LACE_ERR_PIPE_ERROR;
}

/* ==========================================================================
 * JSON to Types Conversion
 * ========================================================================== */

bool lace_rpc_parse_value(cJSON *json, LaceValue *val) {
  if (!val) {
    return false;
  }

  memset(val, 0, sizeof(LaceValue));

  if (!json || cJSON_IsNull(json)) {
    val->type = LACE_TYPE_NULL;
    val->is_null = true;
    return true;
  }

  if (cJSON_IsBool(json)) {
    val->type = LACE_TYPE_BOOL;
    val->bool_val = cJSON_IsTrue(json);
    return true;
  }

  if (cJSON_IsNumber(json)) {
    double num = json->valuedouble;
    int64_t whole;
    if (json_int64(json, &whole)) { /* An integer if it is whole and fits */
      val->type = LACE_TYPE_INT;
      val->int_val = whole;
    } else {
      val->type = LACE_TYPE_FLOAT;
      val->float_val = num;
    }
    return true;
  }

  /* Tagged values the daemon cannot send as plain JSON without losing them */
  if (cJSON_IsObject(json)) {
    cJSON *num = cJSON_GetObjectItem(json, "$num");
    if (num && cJSON_IsString(num) && num->valuestring) {
      val->type = LACE_TYPE_DECIMAL; /* Exact NUMERIC/DECIMAL digits */
      val->text.data = str_dup(num->valuestring);
      val->text.len = strlen(num->valuestring);
      return true;
    }
    cJSON *blob = cJSON_GetObjectItem(json, "$blob");
    if (blob && cJSON_IsString(blob) && blob->valuestring) {
      LaceValue tmp;
      char *e = NULL;
      if (lace_value_from_edit_text(blob->valuestring, LACE_TYPE_BLOB, &tmp, &e)) {
        *val = tmp;
        return true;
      }
      free(e);
      return false;
    }
    cJSON *fl = cJSON_GetObjectItem(json, "$float");
    if (fl && cJSON_IsString(fl) && fl->valuestring) {
      val->type = LACE_TYPE_FLOAT;
      val->float_val = strcmp(fl->valuestring, "nan") == 0   ? NAN
                       : strcmp(fl->valuestring, "-inf") == 0 ? -INFINITY
                                                              : INFINITY;
      return true;
    }
    cJSON *om = cJSON_GetObjectItem(json, "$omitted");
    if (om && cJSON_IsString(om) && om->valuestring) {
      cJSON *bytes = cJSON_GetObjectItem(json, "bytes");
      val->type = LACE_TYPE_TEXT;
      val->text.data = str_dup(om->valuestring);
      val->text.len = strlen(om->valuestring);
      if (!json_size(bytes, &val->omitted) || val->omitted == 0)
        val->omitted = 1; /* Still a value that was not loaded */
      return true;
    }
    /* Exact 64-bit integer that does not fit a JSON double: {"$int": "..."} */
    cJSON *iv = cJSON_GetObjectItem(json, "$int");
    if (iv && cJSON_IsString(iv) && iv->valuestring) {
      char *end = NULL;
      errno = 0;
      long long n = strtoll(iv->valuestring, &end, 10);
      if (errno == 0 && end && *end == '\0' && end != iv->valuestring) {
        val->type = LACE_TYPE_INT;
        val->int_val = (int64_t)n;
        return true;
      }
    }
    return false;
  }

  if (cJSON_IsString(json)) {
    val->type = LACE_TYPE_TEXT;
    if (json->valuestring) {
      val->text.len = strlen(json->valuestring);
      val->text.data = safe_malloc(val->text.len + 1);
      memcpy(val->text.data, json->valuestring, val->text.len + 1);
    }
    return true;
  }

  return false;
}

LaceResult *lace_rpc_parse_result(cJSON *json) {
  if (!json) {
    return NULL;
  }

  LaceResult *result = safe_calloc(1, sizeof(LaceResult));

  /* Parse columns */
  cJSON *columns = cJSON_GetObjectItem(json, "columns");
  cJSON *types = cJSON_GetObjectItem(json, "types");
  if (columns && cJSON_IsArray(columns)) {
    int num_cols = cJSON_GetArraySize(columns);
    if (num_cols > 0) {
      result->columns = safe_calloc((size_t)num_cols, sizeof(LaceColumn));
      result->num_columns = (size_t)num_cols;

      /* Walk the lists: cJSON arrays are linked lists, so indexing each
       * element made parsing quadratic (minutes for a million rows) */
      cJSON *col = columns->child;
      cJSON *type = (types && cJSON_IsArray(types)) ? types->child : NULL;
      for (int i = 0; i < num_cols && col; i++, col = col->next) {
        if (cJSON_IsString(col)) {
          result->columns[i].name = str_dup(col->valuestring);
        }
        if (type) {
          if (cJSON_IsString(type)) {
            result->columns[i].type_name = str_dup(type->valuestring);
          }
          type = type->next;
        }
      }
    }
  }

  /* Parse rows */
  cJSON *rows = cJSON_GetObjectItem(json, "rows");
  if (rows && cJSON_IsArray(rows)) {
    int num_rows = cJSON_GetArraySize(rows);
    if (num_rows > 0) {
      result->rows = safe_calloc((size_t)num_rows, sizeof(LaceRow));
      result->num_rows = (size_t)num_rows;

      cJSON *row = rows->child;
      for (int i = 0; i < num_rows && row; i++, row = row->next) {
        if (cJSON_IsArray(row)) {
          int num_cells = cJSON_GetArraySize(row);
          if (num_cells > 0) {
            result->rows[i].cells = safe_calloc((size_t)num_cells, sizeof(LaceValue));
            result->rows[i].num_cells = (size_t)num_cells;
            cJSON *cell = row->child;
            for (int j = 0; j < num_cells && cell; j++, cell = cell->next) {
              lace_rpc_parse_value(cell, &result->rows[i].cells[j]);
            }
          }
        }
      }
    }
  }

  /* Parse metadata */
  result->truncated = cJSON_IsTrue(cJSON_GetObjectItem(json, "truncated"));
  size_t total;
  if (json_size(cJSON_GetObjectItem(json, "total_rows"), &total)) {
    result->total_rows = total;
  }

  return result;
}

LaceSchema *lace_rpc_parse_schema(cJSON *json) {
  if (!json) {
    return NULL;
  }

  LaceSchema *schema = safe_calloc(1, sizeof(LaceSchema));

  /* Parse name */
  cJSON *name = cJSON_GetObjectItem(json, "name");
  if (name && cJSON_IsString(name)) {
    schema->name = str_dup(name->valuestring);
  }

  /* Parse schema name */
  cJSON *sch = cJSON_GetObjectItem(json, "schema");
  if (sch && cJSON_IsString(sch)) {
    schema->schema = str_dup(sch->valuestring);
  }

  /* Parse columns */
  cJSON *columns = cJSON_GetObjectItem(json, "columns");
  if (columns && cJSON_IsArray(columns)) {
    int num_cols = cJSON_GetArraySize(columns);
    if (num_cols > 0) {
      schema->columns = safe_calloc((size_t)num_cols, sizeof(LaceColumn));
      schema->num_columns = (size_t)num_cols;

      for (int i = 0; i < num_cols; i++) {
        cJSON *col = cJSON_GetArrayItem(columns, i);
        if (!col) continue;

        cJSON *col_name = cJSON_GetObjectItem(col, "name");
        cJSON *col_type = cJSON_GetObjectItem(col, "type_name");
        cJSON *nullable = cJSON_GetObjectItem(col, "nullable");
        cJSON *pk = cJSON_GetObjectItem(col, "primary_key");
        cJSON *auto_inc = cJSON_GetObjectItem(col, "auto_increment");
        cJSON *def_val = cJSON_GetObjectItem(col, "default");
        cJSON *fk = cJSON_GetObjectItem(col, "foreign_key");
        cJSON *max_len = cJSON_GetObjectItem(col, "max_length");

        if (col_name && cJSON_IsString(col_name)) {
          schema->columns[i].name = str_dup(col_name->valuestring);
        }
        if (col_type && cJSON_IsString(col_type)) {
          schema->columns[i].type_name = str_dup(col_type->valuestring);
        }
        schema->columns[i].nullable = nullable && cJSON_IsTrue(nullable);
        schema->columns[i].primary_key = pk && cJSON_IsTrue(pk);
        schema->columns[i].auto_increment = auto_inc && cJSON_IsTrue(auto_inc);
        if (def_val && cJSON_IsString(def_val)) {
          schema->columns[i].default_val = str_dup(def_val->valuestring);
        }
        if (fk && cJSON_IsString(fk)) {
          schema->columns[i].foreign_key = str_dup(fk->valuestring);
        }
        if (max_len && cJSON_IsNumber(max_len)) {
          schema->columns[i].max_length = max_len->valueint;
        }
      }
    }
  }

  /* Parse indexes */
  cJSON *indexes = cJSON_GetObjectItem(json, "indexes");
  if (indexes && cJSON_IsArray(indexes)) {
    int num_idx = cJSON_GetArraySize(indexes);
    if (num_idx > 0) {
      schema->indexes = safe_calloc((size_t)num_idx, sizeof(LaceIndex));
      schema->num_indexes = (size_t)num_idx;

      for (int i = 0; i < num_idx; i++) {
        cJSON *idx = cJSON_GetArrayItem(indexes, i);
        if (!idx) continue;

        cJSON *idx_name = cJSON_GetObjectItem(idx, "name");
        cJSON *idx_unique = cJSON_GetObjectItem(idx, "unique");
        cJSON *idx_primary = cJSON_GetObjectItem(idx, "primary");
        cJSON *idx_type = cJSON_GetObjectItem(idx, "type");
        cJSON *idx_cols = cJSON_GetObjectItem(idx, "columns");

        if (idx_name && cJSON_IsString(idx_name)) {
          schema->indexes[i].name = str_dup(idx_name->valuestring);
        }
        schema->indexes[i].unique = idx_unique && cJSON_IsTrue(idx_unique);
        schema->indexes[i].primary = idx_primary && cJSON_IsTrue(idx_primary);
        if (idx_type && cJSON_IsString(idx_type)) {
          schema->indexes[i].type = str_dup(idx_type->valuestring);
        }

        if (idx_cols && cJSON_IsArray(idx_cols)) {
          int num_idx_cols = cJSON_GetArraySize(idx_cols);
          if (num_idx_cols > 0) {
            schema->indexes[i].columns = safe_calloc((size_t)num_idx_cols, sizeof(char *));
            schema->indexes[i].num_columns = (size_t)num_idx_cols;
            for (int j = 0; j < num_idx_cols; j++) {
              cJSON *col = cJSON_GetArrayItem(idx_cols, j);
              if (col && cJSON_IsString(col)) {
                schema->indexes[i].columns[j] = str_dup(col->valuestring);
              }
            }
          }
        }
      }
    }
  }

  /* Parse foreign keys */
  cJSON *fks = cJSON_GetObjectItem(json, "foreign_keys");
  if (fks && cJSON_IsArray(fks)) {
    int num_fk = cJSON_GetArraySize(fks);
    if (num_fk > 0) {
      schema->foreign_keys = safe_calloc((size_t)num_fk, sizeof(LaceForeignKey));
      schema->num_foreign_keys = (size_t)num_fk;

      for (int i = 0; i < num_fk; i++) {
        cJSON *fk = cJSON_GetArrayItem(fks, i);
        if (!fk) continue;

        cJSON *fk_name = cJSON_GetObjectItem(fk, "name");
        cJSON *fk_cols = cJSON_GetObjectItem(fk, "columns");
        cJSON *fk_ref_table = cJSON_GetObjectItem(fk, "ref_table");
        cJSON *fk_ref_cols = cJSON_GetObjectItem(fk, "ref_columns");
        cJSON *fk_on_delete = cJSON_GetObjectItem(fk, "on_delete");
        cJSON *fk_on_update = cJSON_GetObjectItem(fk, "on_update");

        if (fk_name && cJSON_IsString(fk_name)) {
          schema->foreign_keys[i].name = str_dup(fk_name->valuestring);
        }
        if (fk_ref_table && cJSON_IsString(fk_ref_table)) {
          schema->foreign_keys[i].ref_table = str_dup(fk_ref_table->valuestring);
        }
        if (fk_on_delete && cJSON_IsString(fk_on_delete)) {
          schema->foreign_keys[i].on_delete = str_dup(fk_on_delete->valuestring);
        }
        if (fk_on_update && cJSON_IsString(fk_on_update)) {
          schema->foreign_keys[i].on_update = str_dup(fk_on_update->valuestring);
        }

        if (fk_cols && cJSON_IsArray(fk_cols)) {
          int num_fk_cols = cJSON_GetArraySize(fk_cols);
          if (num_fk_cols > 0) {
            schema->foreign_keys[i].columns = safe_calloc((size_t)num_fk_cols, sizeof(char *));
            schema->foreign_keys[i].num_columns = (size_t)num_fk_cols;
            for (int j = 0; j < num_fk_cols; j++) {
              cJSON *col = cJSON_GetArrayItem(fk_cols, j);
              if (col && cJSON_IsString(col)) {
                schema->foreign_keys[i].columns[j] = str_dup(col->valuestring);
              }
            }
          }
        }

        if (fk_ref_cols && cJSON_IsArray(fk_ref_cols)) {
          int num_ref_cols = cJSON_GetArraySize(fk_ref_cols);
          if (num_ref_cols > 0) {
            schema->foreign_keys[i].ref_columns = safe_calloc((size_t)num_ref_cols, sizeof(char *));
            schema->foreign_keys[i].num_ref_columns = (size_t)num_ref_cols;
            for (int j = 0; j < num_ref_cols; j++) {
              cJSON *col = cJSON_GetArrayItem(fk_ref_cols, j);
              if (col && cJSON_IsString(col)) {
                schema->foreign_keys[i].ref_columns[j] = str_dup(col->valuestring);
              }
            }
          }
        }
      }
    }
  }

  /* Parse row count */
  int64_t row_count;
  if (json_int64(cJSON_GetObjectItem(json, "row_count"), &row_count)) {
    schema->row_count = row_count;
  }

  return schema;
}
