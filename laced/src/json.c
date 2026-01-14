/*
 * laced - Lace Database Daemon
 * JSON serialization helpers implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Value Serialization
 * ========================================================================== */

cJSON *laced_json_from_value(const DbValue *val) {
  if (!val || val->is_null) {
    return cJSON_CreateNull();
  }

  switch (val->type) {
  case DB_TYPE_NULL:
    return cJSON_CreateNull();

  case DB_TYPE_INT:
    return cJSON_CreateNumber((double)val->int_val);

  case DB_TYPE_FLOAT:
    return cJSON_CreateNumber(val->float_val);

  case DB_TYPE_TEXT:
  case DB_TYPE_DATE:
  case DB_TYPE_TIMESTAMP:
    if (val->text.data) {
      return cJSON_CreateString(val->text.data);
    }
    return cJSON_CreateNull();

  case DB_TYPE_BLOB:
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

  case DB_TYPE_BOOL:
    return cJSON_CreateBool(val->bool_val);

  default:
    return cJSON_CreateNull();
  }
}

bool laced_json_to_value(cJSON *json, DbValue *val) {
  if (!json || !val) {
    return false;
  }

  memset(val, 0, sizeof(DbValue));

  if (cJSON_IsNull(json)) {
    val->type = DB_TYPE_NULL;
    val->is_null = true;
    return true;
  }

  if (cJSON_IsBool(json)) {
    val->type = DB_TYPE_BOOL;
    val->bool_val = cJSON_IsTrue(json);
    return true;
  }

  if (cJSON_IsNumber(json)) {
    double num = json->valuedouble;
    /* Check if it's an integer */
    if (num == (double)(int64_t)num && num >= INT64_MIN && num <= INT64_MAX) {
      val->type = DB_TYPE_INT;
      val->int_val = (int64_t)num;
    } else {
      val->type = DB_TYPE_FLOAT;
      val->float_val = num;
    }
    return true;
  }

  if (cJSON_IsString(json)) {
    val->type = DB_TYPE_TEXT;
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

/* ==========================================================================
 * Result Set Serialization
 * ========================================================================== */

cJSON *laced_json_from_result(const ResultSet *rs) {
  if (!rs) {
    return NULL;
  }

  cJSON *result = cJSON_CreateObject();
  if (!result) {
    return NULL;
  }

  /* Add column names */
  cJSON *columns = cJSON_CreateArray();
  if (columns) {
    for (size_t i = 0; i < rs->num_columns; i++) {
      if (rs->columns && rs->columns[i].name) {
        cJSON_AddItemToArray(columns, cJSON_CreateString(rs->columns[i].name));
      } else {
        cJSON_AddItemToArray(columns, cJSON_CreateNull());
      }
    }
    cJSON_AddItemToObject(result, "columns", columns);
  }

  /* Add column types */
  cJSON *types = cJSON_CreateArray();
  if (types) {
    for (size_t i = 0; i < rs->num_columns; i++) {
      if (rs->columns && rs->columns[i].type_name) {
        cJSON_AddItemToArray(types, cJSON_CreateString(rs->columns[i].type_name));
      } else {
        const char *type_name = db_value_type_name(
            rs->columns ? rs->columns[i].type : DB_TYPE_NULL);
        cJSON_AddItemToArray(types, cJSON_CreateString(type_name));
      }
    }
    cJSON_AddItemToObject(result, "types", types);
  }

  /* Add rows */
  cJSON *rows = cJSON_CreateArray();
  if (rows) {
    for (size_t i = 0; i < rs->num_rows; i++) {
      cJSON *row = cJSON_CreateArray();
      if (row && rs->rows) {
        for (size_t j = 0; j < rs->rows[i].num_cells; j++) {
          cJSON_AddItemToArray(row, laced_json_from_value(&rs->rows[i].cells[j]));
        }
        cJSON_AddItemToArray(rows, row);
      }
    }
    cJSON_AddItemToObject(result, "rows", rows);
  }

  /* Add metadata */
  cJSON_AddNumberToObject(result, "num_rows", (double)rs->num_rows);
  cJSON_AddNumberToObject(result, "total_rows", (double)rs->total_rows);
  if (rs->rows_affected >= 0) {
    cJSON_AddNumberToObject(result, "rows_affected", (double)rs->rows_affected);
  }

  return result;
}

/* ==========================================================================
 * Schema Serialization
 * ========================================================================== */

cJSON *laced_json_from_schema(const TableSchema *schema) {
  if (!schema) {
    return NULL;
  }

  cJSON *result = cJSON_CreateObject();
  if (!result) {
    return NULL;
  }

  if (schema->name) {
    cJSON_AddStringToObject(result, "name", schema->name);
  }
  if (schema->schema) {
    cJSON_AddStringToObject(result, "schema", schema->schema);
  }

  /* Add columns */
  cJSON *columns = cJSON_CreateArray();
  if (columns) {
    for (size_t i = 0; i < schema->num_columns; i++) {
      cJSON *col = cJSON_CreateObject();
      if (col && schema->columns) {
        ColumnDef *c = &schema->columns[i];
        if (c->name) {
          cJSON_AddStringToObject(col, "name", c->name);
        }
        cJSON_AddStringToObject(col, "type", db_value_type_name(c->type));
        if (c->type_name) {
          cJSON_AddStringToObject(col, "type_name", c->type_name);
        }
        cJSON_AddBoolToObject(col, "nullable", c->nullable);
        cJSON_AddBoolToObject(col, "primary_key", c->primary_key);
        cJSON_AddBoolToObject(col, "auto_increment", c->auto_increment);
        if (c->default_val) {
          cJSON_AddStringToObject(col, "default", c->default_val);
        }
        if (c->foreign_key) {
          cJSON_AddStringToObject(col, "foreign_key", c->foreign_key);
        }
        if (c->max_length > 0) {
          cJSON_AddNumberToObject(col, "max_length", c->max_length);
        }
        cJSON_AddItemToArray(columns, col);
      }
    }
    cJSON_AddItemToObject(result, "columns", columns);
  }

  /* Add indexes */
  cJSON *indexes = cJSON_CreateArray();
  if (indexes) {
    for (size_t i = 0; i < schema->num_indexes; i++) {
      cJSON *idx = cJSON_CreateObject();
      if (idx && schema->indexes) {
        IndexDef *ix = &schema->indexes[i];
        if (ix->name) {
          cJSON_AddStringToObject(idx, "name", ix->name);
        }
        cJSON_AddBoolToObject(idx, "unique", ix->unique);
        cJSON_AddBoolToObject(idx, "primary", ix->primary);
        if (ix->type) {
          cJSON_AddStringToObject(idx, "type", ix->type);
        }

        cJSON *idx_cols = cJSON_CreateArray();
        if (idx_cols && ix->columns) {
          for (size_t j = 0; j < ix->num_columns; j++) {
            if (ix->columns[j]) {
              cJSON_AddItemToArray(idx_cols, cJSON_CreateString(ix->columns[j]));
            }
          }
          cJSON_AddItemToObject(idx, "columns", idx_cols);
        }
        cJSON_AddItemToArray(indexes, idx);
      }
    }
    cJSON_AddItemToObject(result, "indexes", indexes);
  }

  /* Add foreign keys */
  cJSON *fks = cJSON_CreateArray();
  if (fks) {
    for (size_t i = 0; i < schema->num_foreign_keys; i++) {
      cJSON *fk = cJSON_CreateObject();
      if (fk && schema->foreign_keys) {
        ForeignKeyDef *f = &schema->foreign_keys[i];
        if (f->name) {
          cJSON_AddStringToObject(fk, "name", f->name);
        }

        cJSON *fk_cols = cJSON_CreateArray();
        if (fk_cols && f->columns) {
          for (size_t j = 0; j < f->num_columns; j++) {
            if (f->columns[j]) {
              cJSON_AddItemToArray(fk_cols, cJSON_CreateString(f->columns[j]));
            }
          }
          cJSON_AddItemToObject(fk, "columns", fk_cols);
        }

        if (f->ref_table) {
          cJSON_AddStringToObject(fk, "ref_table", f->ref_table);
        }

        cJSON *ref_cols = cJSON_CreateArray();
        if (ref_cols && f->ref_columns) {
          for (size_t j = 0; j < f->num_ref_columns; j++) {
            if (f->ref_columns[j]) {
              cJSON_AddItemToArray(ref_cols, cJSON_CreateString(f->ref_columns[j]));
            }
          }
          cJSON_AddItemToObject(fk, "ref_columns", ref_cols);
        }

        if (f->on_delete) {
          cJSON_AddStringToObject(fk, "on_delete", f->on_delete);
        }
        if (f->on_update) {
          cJSON_AddStringToObject(fk, "on_update", f->on_update);
        }
        cJSON_AddItemToArray(fks, fk);
      }
    }
    cJSON_AddItemToObject(result, "foreign_keys", fks);
  }

  cJSON_AddNumberToObject(result, "row_count", (double)schema->row_count);

  return result;
}

/* ==========================================================================
 * Parameter Extraction
 * ========================================================================== */

bool laced_json_get_string(cJSON *params, const char *name, const char **out) {
  if (!params || !name || !out) {
    return false;
  }

  cJSON *item = cJSON_GetObjectItem(params, name);
  if (!item || !cJSON_IsString(item)) {
    return false;
  }

  *out = item->valuestring;
  return true;
}

bool laced_json_get_int(cJSON *params, const char *name, int *out) {
  if (!params || !name || !out) {
    return false;
  }

  cJSON *item = cJSON_GetObjectItem(params, name);
  if (!item || !cJSON_IsNumber(item)) {
    return false;
  }

  *out = item->valueint;
  return true;
}

bool laced_json_get_size(cJSON *params, const char *name, size_t *out) {
  if (!params || !name || !out) {
    return false;
  }

  cJSON *item = cJSON_GetObjectItem(params, name);
  if (!item || !cJSON_IsNumber(item)) {
    return false;
  }

  double val = item->valuedouble;
  if (val < 0) {
    return false;
  }

  *out = (size_t)val;
  return true;
}

bool laced_json_get_bool(cJSON *params, const char *name, bool *out) {
  if (!params || !name || !out) {
    return false;
  }

  cJSON *item = cJSON_GetObjectItem(params, name);
  if (!item || !cJSON_IsBool(item)) {
    return false;
  }

  *out = cJSON_IsTrue(item);
  return true;
}
