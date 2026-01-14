/*
 * liblace - Lace Client Library
 * JSON-RPC marshaling implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Access to client internals */
struct lace_client {
  pid_t daemon_pid;
  FILE *to_daemon;
  FILE *from_daemon;
  int timeout_ms;
  char *last_error;
  int64_t next_id;
  bool connected;
};

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Set error message in client */
static void rpc_set_error(lace_client_t *client, const char *msg) {
  free(client->last_error);
  client->last_error = msg ? strdup(msg) : NULL;
}

/* Read a line from daemon (newline-delimited) */
static char *read_response_line(FILE *from_daemon) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    return NULL;
  }

  while (1) {
    int c = fgetc(from_daemon);
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

int lace_rpc_call(lace_client_t *client, const char *method,
                  cJSON *params, cJSON **result) {
  if (!client || !method) {
    return LACE_ERR_INVALID_PARAMS;
  }

  if (!client->connected || !client->to_daemon || !client->from_daemon) {
    return LACE_ERR_CONNECTION_CLOSED;
  }

  /* Build request */
  cJSON *req = cJSON_CreateObject();
  if (!req) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddStringToObject(req, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(req, "id", (double)client->next_id++);
  cJSON_AddStringToObject(req, "method", method);

  if (params) {
    cJSON_AddItemToObject(req, "params", cJSON_Duplicate(params, true));
  }

  /* Serialize to JSON */
  char *req_str = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);

  if (!req_str) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  /* Send request */
  if (fprintf(client->to_daemon, "%s\n", req_str) < 0) {
    free(req_str);
    rpc_set_error(client, "Failed to send request");
    return LACE_ERR_PIPE_ERROR;
  }
  fflush(client->to_daemon);
  free(req_str);

  /* Read response */
  char *resp_str = read_response_line(client->from_daemon);
  if (!resp_str) {
    rpc_set_error(client, "No response from daemon");
    client->connected = false;
    return LACE_ERR_CONNECTION_LOST;
  }

  /* Parse response */
  cJSON *resp = cJSON_Parse(resp_str);
  free(resp_str);

  if (!resp) {
    rpc_set_error(client, "Invalid JSON response");
    return LACE_ERR_PARSE_ERROR;
  }

  /* Check for error */
  cJSON *error = cJSON_GetObjectItem(resp, "error");
  if (error && cJSON_IsObject(error)) {
    cJSON *msg = cJSON_GetObjectItem(error, "message");
    cJSON *code = cJSON_GetObjectItem(error, "code");

    if (msg && cJSON_IsString(msg)) {
      rpc_set_error(client, msg->valuestring);
    }

    int err_code = LACE_ERR_INTERNAL_ERROR;
    if (code && cJSON_IsNumber(code)) {
      /* Map JSON-RPC error codes to lace error codes */
      int json_code = code->valueint;
      switch (json_code) {
      case -32700: err_code = LACE_ERR_PARSE_ERROR; break;
      case -32600: err_code = LACE_ERR_INVALID_REQUEST; break;
      case -32601: err_code = LACE_ERR_METHOD_NOT_FOUND; break;
      case -32602: err_code = LACE_ERR_INVALID_PARAMS; break;
      case -32603: err_code = LACE_ERR_INTERNAL_ERROR; break;
      default: err_code = LACE_ERR_INTERNAL_ERROR; break;
      }
    }

    cJSON_Delete(resp);
    return err_code;
  }

  /* Extract result */
  cJSON *res = cJSON_GetObjectItem(resp, "result");
  if (result) {
    if (res) {
      *result = cJSON_Duplicate(res, true);
    } else {
      *result = NULL;
    }
  }

  cJSON_Delete(resp);
  return LACE_OK;
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

  if (cJSON_IsString(json)) {
    val->type = LACE_TYPE_TEXT;
    if (json->valuestring) {
      val->text.len = strlen(json->valuestring);
      val->text.data = malloc(val->text.len + 1);
      if (!val->text.data) {
        return false;
      }
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

  LaceResult *result = calloc(1, sizeof(LaceResult));
  if (!result) {
    return NULL;
  }

  /* Parse columns */
  cJSON *columns = cJSON_GetObjectItem(json, "columns");
  cJSON *types = cJSON_GetObjectItem(json, "types");
  if (columns && cJSON_IsArray(columns)) {
    int num_cols = cJSON_GetArraySize(columns);
    if (num_cols > 0) {
      result->columns = calloc((size_t)num_cols, sizeof(LaceColumn));
      if (!result->columns) {
        free(result);
        return NULL;
      }
      result->num_columns = (size_t)num_cols;

      for (int i = 0; i < num_cols; i++) {
        cJSON *col = cJSON_GetArrayItem(columns, i);
        if (col && cJSON_IsString(col)) {
          result->columns[i].name = strdup(col->valuestring);
        }

        /* Get type from types array */
        if (types && cJSON_IsArray(types)) {
          cJSON *type = cJSON_GetArrayItem(types, i);
          if (type && cJSON_IsString(type)) {
            result->columns[i].type_name = strdup(type->valuestring);
            /* TODO: Parse type to LaceValueType */
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
      result->rows = calloc((size_t)num_rows, sizeof(LaceRow));
      if (!result->rows) {
        lace_result_free(result);
        return NULL;
      }
      result->num_rows = (size_t)num_rows;

      for (int i = 0; i < num_rows; i++) {
        cJSON *row = cJSON_GetArrayItem(rows, i);
        if (row && cJSON_IsArray(row)) {
          int num_cells = cJSON_GetArraySize(row);
          if (num_cells > 0) {
            result->rows[i].cells = calloc((size_t)num_cells, sizeof(LaceValue));
            if (!result->rows[i].cells) {
              lace_result_free(result);
              return NULL;
            }
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

  LaceSchema *schema = calloc(1, sizeof(LaceSchema));
  if (!schema) {
    return NULL;
  }

  /* Parse name */
  cJSON *name = cJSON_GetObjectItem(json, "name");
  if (name && cJSON_IsString(name)) {
    schema->name = strdup(name->valuestring);
  }

  /* Parse schema name */
  cJSON *sch = cJSON_GetObjectItem(json, "schema");
  if (sch && cJSON_IsString(sch)) {
    schema->schema = strdup(sch->valuestring);
  }

  /* Parse columns */
  cJSON *columns = cJSON_GetObjectItem(json, "columns");
  if (columns && cJSON_IsArray(columns)) {
    int num_cols = cJSON_GetArraySize(columns);
    if (num_cols > 0) {
      schema->columns = calloc((size_t)num_cols, sizeof(LaceColumn));
      if (!schema->columns) {
        lace_schema_free(schema);
        return NULL;
      }
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
          schema->columns[i].name = strdup(col_name->valuestring);
        }
        if (col_type && cJSON_IsString(col_type)) {
          schema->columns[i].type_name = strdup(col_type->valuestring);
        }
        schema->columns[i].nullable = nullable && cJSON_IsTrue(nullable);
        schema->columns[i].primary_key = pk && cJSON_IsTrue(pk);
        schema->columns[i].auto_increment = auto_inc && cJSON_IsTrue(auto_inc);
        if (def_val && cJSON_IsString(def_val)) {
          schema->columns[i].default_val = strdup(def_val->valuestring);
        }
        if (fk && cJSON_IsString(fk)) {
          schema->columns[i].foreign_key = strdup(fk->valuestring);
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
      schema->indexes = calloc((size_t)num_idx, sizeof(LaceIndex));
      if (!schema->indexes) {
        lace_schema_free(schema);
        return NULL;
      }
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
          schema->indexes[i].name = strdup(idx_name->valuestring);
        }
        schema->indexes[i].unique = idx_unique && cJSON_IsTrue(idx_unique);
        schema->indexes[i].primary = idx_primary && cJSON_IsTrue(idx_primary);
        if (idx_type && cJSON_IsString(idx_type)) {
          schema->indexes[i].type = strdup(idx_type->valuestring);
        }

        if (idx_cols && cJSON_IsArray(idx_cols)) {
          int num_idx_cols = cJSON_GetArraySize(idx_cols);
          if (num_idx_cols > 0) {
            schema->indexes[i].columns = calloc((size_t)num_idx_cols, sizeof(char *));
            if (schema->indexes[i].columns) {
              schema->indexes[i].num_columns = (size_t)num_idx_cols;
              for (int j = 0; j < num_idx_cols; j++) {
                cJSON *col = cJSON_GetArrayItem(idx_cols, j);
                if (col && cJSON_IsString(col)) {
                  schema->indexes[i].columns[j] = strdup(col->valuestring);
                }
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
      schema->foreign_keys = calloc((size_t)num_fk, sizeof(LaceForeignKey));
      if (!schema->foreign_keys) {
        lace_schema_free(schema);
        return NULL;
      }
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
          schema->foreign_keys[i].name = strdup(fk_name->valuestring);
        }
        if (fk_ref_table && cJSON_IsString(fk_ref_table)) {
          schema->foreign_keys[i].ref_table = strdup(fk_ref_table->valuestring);
        }
        if (fk_on_delete && cJSON_IsString(fk_on_delete)) {
          schema->foreign_keys[i].on_delete = strdup(fk_on_delete->valuestring);
        }
        if (fk_on_update && cJSON_IsString(fk_on_update)) {
          schema->foreign_keys[i].on_update = strdup(fk_on_update->valuestring);
        }

        if (fk_cols && cJSON_IsArray(fk_cols)) {
          int num_fk_cols = cJSON_GetArraySize(fk_cols);
          if (num_fk_cols > 0) {
            schema->foreign_keys[i].columns = calloc((size_t)num_fk_cols, sizeof(char *));
            if (schema->foreign_keys[i].columns) {
              schema->foreign_keys[i].num_columns = (size_t)num_fk_cols;
              for (int j = 0; j < num_fk_cols; j++) {
                cJSON *col = cJSON_GetArrayItem(fk_cols, j);
                if (col && cJSON_IsString(col)) {
                  schema->foreign_keys[i].columns[j] = strdup(col->valuestring);
                }
              }
            }
          }
        }

        if (fk_ref_cols && cJSON_IsArray(fk_ref_cols)) {
          int num_ref_cols = cJSON_GetArraySize(fk_ref_cols);
          if (num_ref_cols > 0) {
            schema->foreign_keys[i].ref_columns = calloc((size_t)num_ref_cols, sizeof(char *));
            if (schema->foreign_keys[i].ref_columns) {
              schema->foreign_keys[i].num_ref_columns = (size_t)num_ref_cols;
              for (int j = 0; j < num_ref_cols; j++) {
                cJSON *col = cJSON_GetArrayItem(fk_ref_cols, j);
                if (col && cJSON_IsString(col)) {
                  schema->foreign_keys[i].ref_columns[j] = strdup(col->valuestring);
                }
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
      char *hex = malloc(hex_len);
      if (!hex) {
        return cJSON_CreateNull();
      }
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

cJSON *lace_rpc_filter_to_json(const LaceFilter *filter) {
  if (!filter) {
    return NULL;
  }

  cJSON *obj = cJSON_CreateObject();
  if (!obj) {
    return NULL;
  }

  cJSON_AddNumberToObject(obj, "column", (double)filter->column);
  cJSON_AddNumberToObject(obj, "op", (double)filter->op);
  if (filter->value) {
    cJSON_AddStringToObject(obj, "value", filter->value);
  }
  if (filter->value2) {
    cJSON_AddStringToObject(obj, "value2", filter->value2);
  }

  return obj;
}

cJSON *lace_rpc_sort_to_json(const LaceSort *sort) {
  if (!sort) {
    return NULL;
  }

  cJSON *obj = cJSON_CreateObject();
  if (!obj) {
    return NULL;
  }

  cJSON_AddNumberToObject(obj, "column", (double)sort->column);
  cJSON_AddBoolToObject(obj, "descending", sort->dir == LACE_SORT_DESC);

  return obj;
}
