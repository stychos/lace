/*
 * liblace - Lace Client Library
 * JSON-RPC marshaling implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "rpc.h"
#include "../include/util/mem.h"
#include "../include/util/str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_internal.h"
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Set error message in client */
static void rpc_set_error(lace_client_t *client, const char *msg) {
  (void)client;
  lace_set_thread_error(msg);
}

/* ==========================================================================
 * Link I/O
 * ========================================================================== */

/* Milliseconds left until deadline_ms (monotonic); -1 = no deadline */
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

    struct pollfd pfd = {.fd = l->rfd, .events = POLLIN};
    int r = poll(&pfd, 1, remaining_ms(deadline_ms));
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
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
    ssize_t n = read(l->rfd, l->buf + l->len, l->cap - l->len);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
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
    struct pollfd pfd = {.fd = l->wfd, .events = POLLOUT};
    int r = poll(&pfd, 1, remaining_ms(deadline_ms));
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      return false;
    }
    ssize_t n = write(l->wfd, data, len);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
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
  struct pollfd pfd = {.fd = l->rfd, .events = POLLIN};
  int r = poll(&pfd, 1, 0);
  if (r <= 0) {
    return false; /* Nothing pending: healthy idle link */
  }
  if (pfd.revents & (POLLERR | POLLNVAL)) {
    return true;
  }
  if (!l->is_socket) {
    /* Pipe to a stdio daemon: hung up once the writer is gone and no data
     * is left to read */
    return (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN);
  }
  char c;
  ssize_t n = recv(l->rfd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n == 0) {
    return true; /* Orderly shutdown by daemon */
  }
  return n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

/* ==========================================================================
 * RPC Exchange
 * ========================================================================== */

/* Reply timeout for a method: queries may legitimately run for hours (they
 * can be cancelled), connects include SSH tunnels; everything else is a
 * quick control call that a healthy daemon answers at once. */
static int64_t method_deadline(const lace_client_t *client, const char *method) {
  int ms;
  if (strcmp(method, "query") == 0) {
    ms = client->query_timeout_ms;
  } else if (strcmp(method, "connect") == 0) {
    ms = client->timeout_ms > 60000 ? client->timeout_ms : 60000;
  } else {
    ms = client->timeout_ms;
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
 * to an abandoned request) is discarded. */
static int rpc_receive(lace_client_t *client, LaceLink *l, int64_t req_id,
                       int64_t deadline_ms, cJSON **result) {
  cJSON *resp = NULL;
  for (;;) {
    char *line = NULL;
    int rc = link_read_line(l, deadline_ms, &line);
    if (rc == LACE_ERR_TIMEOUT) {
      rpc_set_error(client, "Timed out waiting for the lace daemon");
      return rc;
    }
    if (rc != LACE_OK) {
      rpc_set_error(client, "No response from lace daemon");
      return LACE_ERR_CONNECTION_LOST;
    }
    resp = cJSON_Parse(line);
    free(line);
    if (!resp) {
      rpc_set_error(client, "Invalid JSON response from lace daemon");
      return LACE_ERR_PARSE_ERROR;
    }
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    if (id && cJSON_IsNumber(id) && (int64_t)id->valuedouble == req_id) {
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
  return rpc_receive(client, l, req_id, deadline, result);
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

static int link_initialize(lace_client_t *client, LaceLink *l, bool respawned) {
  cJSON *hello = cJSON_CreateObject();
  cJSON_AddStringToObject(hello, "client_id", client->client_id);
  bool tcp = l->is_socket && client->conn_mode == LACE_CONN_TCP;
  char *token = tcp ? tcp_token(client) : NULL;
  if (token) {
    cJSON_AddStringToObject(hello, "token", token);
  }
  cJSON *res = NULL;
  int rc = rpc_exchange(client, l, "hello", hello, &res);
  cJSON_Delete(hello); /* Holds a copy of the token */
  str_secure_free(token);
  cJSON_Delete(res);
  if (rc == LACE_ERR_TIMEOUT || rc == LACE_ERR_CONNECTION_LOST ||
      rc == LACE_ERR_PIPE_ERROR || rc == LACE_ERR_PARSE_ERROR) {
    return rc;
  }
  /* An older daemon answers "method not found": fine. Anything else on TCP
   * (a wrong or missing token) means this link cannot be used. */
  if (rc != LACE_OK && rc != LACE_ERR_METHOD_NOT_FOUND && tcp) {
    return rc;
  }

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
  if (!client->daemon_instance[0] && !respawned) {
    /* First link of this client: learn the daemon's identity */
    snprintf(client->daemon_instance, sizeof(client->daemon_instance), "%s", inst);
  } else if (respawned || inst[0] == '\0' ||
             strcmp(inst, client->daemon_instance) != 0) {
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
      if (!link_is_stale(l)) {
        pthread_mutex_unlock(&client->lock);
        *out = l;
        break;
      }
      /* The daemon dropped it (restart, reaping): discard and try another */
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

    /* Open a new link outside the lock (connecting may take a while) */
    LaceLink *l = NULL;
    int how = lace_client_open_link(client, &l);
    if (how == 0 || !l) {
      rpc_set_error(client, "Lost connection to lace daemon");
      return LACE_ERR_CONNECTION_LOST;
    }
    pthread_mutex_lock(&client->lock);
    client->num_links++;
    pthread_mutex_unlock(&client->lock);
    if (how == 2) {
      /* A fresh daemon: initialize now so the generation moves on */
      int rc = link_initialize(client, l, true);
      if (rc != LACE_OK) {
        pthread_mutex_lock(&client->lock);
        client->num_links--;
        pthread_mutex_unlock(&client->lock);
        lace_link_free(l);
        return rc;
      }
    }
    *out = l;
    return LACE_OK;
  }

  if (!(*out)->initialized) {
    int rc = link_initialize(client, *out, false);
    if (rc != LACE_OK) {
      pthread_mutex_lock(&client->lock);
      client->num_links--;
      pthread_cond_broadcast(&client->link_released);
      pthread_mutex_unlock(&client->lock);
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
  if (broken || (!client->single_link &&
                 client->num_idle >= LACE_CLIENT_MAX_IDLE_LINKS)) {
    client->num_links--;
    pthread_cond_broadcast(&client->link_released);
    pthread_mutex_unlock(&client->lock);
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

static bool rc_breaks_link(int rc) {
  return rc == LACE_ERR_TIMEOUT || rc == LACE_ERR_CONNECTION_LOST ||
         rc == LACE_ERR_PIPE_ERROR || rc == LACE_ERR_PARSE_ERROR;
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
    rc = rpc_send(l, req_id, method, params, deadline);
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
    rc = rpc_receive(client, l, req_id, deadline, result);
    release_link(client, l, rc_breaks_link(rc));
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
    /* Check if it's an integer */
    if (num == (double)(int64_t)num && num >= (double)INT64_MIN && num <= (double)INT64_MAX) {
      val->type = LACE_TYPE_INT;
      val->int_val = (int64_t)num;
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
      val->omitted = (bytes && cJSON_IsNumber(bytes) && bytes->valuedouble > 0)
                         ? (size_t)bytes->valuedouble
                         : 1;
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
  cJSON *total = cJSON_GetObjectItem(json, "total_rows");
  if (total && cJSON_IsNumber(total)) {
    result->total_rows = (size_t)total->valuedouble;
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
  cJSON *row_count = cJSON_GetObjectItem(json, "row_count");
  if (row_count && cJSON_IsNumber(row_count)) {
    schema->row_count = (int64_t)row_count->valuedouble;
  }

  return schema;
}

/* ==========================================================================
 * Types to JSON Conversion
 * ========================================================================== */

cJSON *lace_rpc_value_to_json(const LaceValue *val) {
  if (!val || val->is_null) {
    return cJSON_CreateNull();
  }

  switch (val->type) {
  case LACE_TYPE_NULL:
    return cJSON_CreateNull();

  case LACE_TYPE_INT:
    return cJSON_CreateNumber((double)val->int_val);

  case LACE_TYPE_FLOAT:
    return cJSON_CreateNumber(val->float_val);

  case LACE_TYPE_TEXT:
  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    if (val->text.data) {
      return cJSON_CreateString(val->text.data);
    }
    return cJSON_CreateNull();

  case LACE_TYPE_BLOB:
    /* Encode blob as hex string */
    if (val->blob.data && val->blob.len > 0) {
      size_t hex_len = val->blob.len * 2 + 1;
      char *hex = safe_malloc(hex_len);
      for (size_t i = 0; i < val->blob.len; i++) {
        snprintf(hex + i * 2, 3, "%02x", val->blob.data[i]);
      }
      cJSON *result = cJSON_CreateString(hex);
      free(hex);
      return result;
    }
    return cJSON_CreateNull();

  case LACE_TYPE_BOOL:
    return cJSON_CreateBool(val->bool_val);

  default:
    return cJSON_CreateNull();
  }
}
