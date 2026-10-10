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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client_internal.h"
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Set error message in client */
static void rpc_set_error(lace_client_t *client, const char *msg) {
  (void)client;
  lace_set_thread_error(msg);
}

/* Read a line from daemon (newline-delimited) */
static char *read_response_line(FILE *from_daemon) {
  if (!from_daemon) {
    return NULL;
  }
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    return NULL;
  }

  while (1) {
    int c = fgetc(from_daemon);
    if (c == EOF && ferror(from_daemon) && errno == EINTR) {
      clearerr(from_daemon);
      continue;
    }
    if (c == EOF || c == '\n') {
      if (c == EOF && len == 0) {
        free(buf);
        return NULL;
      }
      break;
    }

    /* Grow buffer if needed */
    if (len + 1 >= cap) {
      size_t new_cap = cap * 2;
      if (new_cap < cap) { /* Overflow */
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

/* ==========================================================================
 * RPC Call
 * ========================================================================== */

/* Socket helpers (socket.c) */
extern int lace_connect_unix(const char *path, int timeout_ms);
extern char *lace_get_default_socket_path(void);

/* True if the daemon end of the socket has closed (EOF or error pending).
 * Never blocks. Data waiting to be read is not a hangup. */
static bool socket_is_stale(int fd) {
  if (fd < 0) {
    return true;
  }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int r = poll(&pfd, 1, 0);
  if (r <= 0) {
    return false; /* Nothing pending: healthy idle socket */
  }
  if (pfd.revents & (POLLERR | POLLNVAL)) {
    return true;
  }
  char c;
  ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n == 0) {
    return true; /* Orderly shutdown by daemon */
  }
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    return true;
  }
  return false;
}

static bool rpc_reconnect(lace_client_t *client);

/* Send one request line. Returns false on any write failure. */
static bool rpc_send_line(lace_client_t *client, const char *line) {
  if (!client->to_daemon) {
    return false;
  }
  if (fprintf(client->to_daemon, "%s\n", line) < 0) {
    return false;
  }
  return fflush(client->to_daemon) == 0;
}

/* One request/response exchange on the current link. `allow_resend`
 * permits reconnecting and resending once if the write fails (the daemon
 * cannot have seen a request whose write failed). Caller holds the lock. */
static int rpc_exchange(lace_client_t *client, const char *method,
                        cJSON *params, cJSON **result, bool allow_resend) {
  int64_t req_id = client->next_id++;

  /* Build request */
  cJSON *req = cJSON_CreateObject();
  if (!req) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(req, "id", (double)req_id);
  cJSON_AddStringToObject(req, "method", method);

  if (params) {
    cJSON_AddItemToObject(req, "params", cJSON_Duplicate(params, true));
  }

  char *req_str = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!req_str) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  if (!rpc_send_line(client, req_str)) {
    /* Write failed before the daemon could have seen the request, so it is
     * safe to reconnect and send it once more. */
    if (!allow_resend || !rpc_reconnect(client) ||
        !rpc_send_line(client, req_str)) {
      free(req_str);
      lace_client_close_streams(client);
      rpc_set_error(client, "Failed to send request to lace daemon");
      return LACE_ERR_PIPE_ERROR;
    }
  }
  free(req_str);

  /* Read lines until the response carrying our id arrives. Anything else
   * (notifications, a late reply to an abandoned request) is discarded. */
  cJSON *resp = NULL;
  for (;;) {
    char *resp_str = read_response_line(client->from_daemon);
    if (!resp_str) {
      rpc_set_error(client, "No response from lace daemon");
      lace_client_close_streams(client); /* Reconnect on next call */
      return LACE_ERR_CONNECTION_LOST;
    }
    resp = cJSON_Parse(resp_str);
    free(resp_str);
    if (!resp) {
      rpc_set_error(client, "Invalid JSON response from lace daemon");
      lace_client_close_streams(client); /* Stream is out of sync */
      return LACE_ERR_PARSE_ERROR;
    }
    cJSON *id = cJSON_GetObjectItem(resp, "id");
    if (id && cJSON_IsNumber(id) && (int64_t)id->valuedouble == req_id) {
      break;
    }
    if (id && cJSON_IsNull(id) && cJSON_GetObjectItem(resp, "error")) {
      break; /* Daemon could not parse our request; report its error */
    }
    cJSON_Delete(resp);
  }

  /* Check for error */
  cJSON *error = cJSON_GetObjectItem(resp, "error");
  if (error && cJSON_IsObject(error)) {
    cJSON *msg = cJSON_GetObjectItem(error, "message");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    rpc_set_error(client, (msg && cJSON_IsString(msg)) ? msg->valuestring
                                                       : "Daemon error");

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

  cJSON *res = cJSON_GetObjectItem(resp, "result");
  if (result) {
    *result = res ? cJSON_Duplicate(res, true) : NULL;
  }

  cJSON_Delete(resp);
  return LACE_OK;
}

/* Ask the daemon for its instance id ("" if it does not report one). */
static void rpc_fetch_instance(lace_client_t *client, char *out, size_t out_size) {
  out[0] = '\0';
  cJSON *res = NULL;
  if (rpc_exchange(client, "version", NULL, &res, false) == LACE_OK && res) {
    cJSON *inst = cJSON_GetObjectItem(res, "instance");
    if (inst && cJSON_IsString(inst) && inst->valuestring) {
      snprintf(out, out_size, "%s", inst->valuestring);
    }
  }
  cJSON_Delete(res);
}

/* Re-establish the daemon link and decide whether it is still the same
 * daemon. A different daemon (respawned by us, or by another frontend
 * sharing the socket) has an unrelated connection pool whose ids restart
 * at 1, so any conn_id from before would address someone else's database:
 * bump the generation so callers re-open their connections first.
 * Caller holds client->lock. */
static bool rpc_reconnect(lace_client_t *client) {
  int how = lace_client_reattach(client);
  if (how == 0) {
    return false;
  }
  char inst[sizeof(client->daemon_instance)];
  rpc_fetch_instance(client, inst, sizeof(inst));
  bool changed = how == 2 || strcmp(inst, client->daemon_instance) != 0 ||
                 inst[0] == '\0';
  if (changed) {
    client->generation++;
    client->num_connections = 0;
  }
  snprintf(client->daemon_instance, sizeof(client->daemon_instance), "%s", inst);
  return true;
}

/* Methods whose params carry a conn_id from a specific daemon generation */
static bool method_uses_conn_id(const char *method) {
  return strcmp(method, "query") == 0 || strcmp(method, "disconnect") == 0 ||
         strcmp(method, "cancel") == 0;
}

/* Caller must hold client->lock. */
static int rpc_call_locked(lace_client_t *client, const char *method,
                           cJSON *params, cJSON **result) {
  if (!client->connected) {
    rpc_set_error(client, "Client is not connected to lace daemon");
    return LACE_ERR_CONNECTION_CLOSED;
  }

  /* Learn the daemon's identity once, so a later reconnect can tell
   * whether it reached the same daemon. */
  if (!client->daemon_instance[0] && client->to_daemon &&
      !socket_is_stale(client->socket_fd)) {
    rpc_fetch_instance(client, client->daemon_instance,
                       sizeof(client->daemon_instance));
  }

  /* The daemon may have dropped this socket (restart, idle reaping). Detect
   * that before sending so a request is never written into a dead link. */
  if (!client->to_daemon || socket_is_stale(client->socket_fd)) {
    if (!rpc_reconnect(client)) {
      rpc_set_error(client, "Lost connection to lace daemon");
      return LACE_ERR_CONNECTION_LOST;
    }
  }

  /* A caller that captured its conn_id under an older generation (another
   * thread reconnected meanwhile) must not send it to the new daemon,
   * where the same number may belong to a different database. */
  if (method_uses_conn_id(method) && lace_thread_generation_mismatch(client)) {
    rpc_set_error(client, "Lace daemon was restarted; reconnecting");
    return LACE_ERR_CONNECTION_LOST;
  }

  return rpc_exchange(client, method, params, result, true);
}

int lace_rpc_call(lace_client_t *client, const char *method, cJSON *params,
                  cJSON **result) {
  if (result) {
    *result = NULL;
  }
  if (!client || !method) {
    return LACE_ERR_INVALID_PARAMS;
  }
  pthread_mutex_lock(&client->lock);
  int err = rpc_call_locked(client, method, params, result);
  pthread_mutex_unlock(&client->lock);
  return err;
}

int lace_rpc_try_call(lace_client_t *client, const char *method,
                      cJSON *params, cJSON **result) {
  if (result) {
    *result = NULL;
  }
  if (!client || !method) {
    return LACE_ERR_INVALID_PARAMS;
  }
  if (pthread_mutex_trylock(&client->lock) != 0) {
    return LACE_ERR_TIMEOUT; /* Busy: another call is in flight */
  }
  int err = rpc_call_locked(client, method, params, result);
  pthread_mutex_unlock(&client->lock);
  return err;
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

  /* Exact 64-bit integer that does not fit a JSON double: {"$int": "..."} */
  if (cJSON_IsObject(json)) {
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

      for (int i = 0; i < num_cols; i++) {
        cJSON *col = cJSON_GetArrayItem(columns, i);
        if (col && cJSON_IsString(col)) {
          result->columns[i].name = str_dup(col->valuestring);
        }

        /* Get type from types array */
        if (types && cJSON_IsArray(types)) {
          cJSON *type = cJSON_GetArrayItem(types, i);
          if (type && cJSON_IsString(type)) {
            result->columns[i].type_name = str_dup(type->valuestring);
          }
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

      for (int i = 0; i < num_rows; i++) {
        cJSON *row = cJSON_GetArrayItem(rows, i);
        if (row && cJSON_IsArray(row)) {
          int num_cells = cJSON_GetArraySize(row);
          if (num_cells > 0) {
            result->rows[i].cells = safe_calloc((size_t)num_cells, sizeof(LaceValue));
            result->rows[i].num_cells = (size_t)num_cells;

            for (int j = 0; j < num_cells; j++) {
              cJSON *cell = cJSON_GetArrayItem(row, j);
              lace_rpc_parse_value(cell, &result->rows[i].cells[j]);
            }
          }
        }
      }
    }
  }

  /* Parse metadata */
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
