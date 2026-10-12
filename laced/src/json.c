/*
 * laced - Lace Database Daemon
 * JSON serialization helpers implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "json.h"
#include <math.h>
#include <util/mem.h>
#include <stdio.h>
#include <limits.h>
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
    /* JSON numbers are doubles: integers beyond +/-2^53 would be rounded,
     * and a rounded primary key makes edits hit the wrong row. Send those
     * as {"$int": "<decimal>"} so the client restores an exact integer.
     * (A plain string is not enough: MySQL compares a string literal with
     * a BIGINT column as double, which matches neighbouring ids.) */
    if (val->int_val > 9007199254740992LL || val->int_val < -9007199254740992LL) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
      cJSON *obj = cJSON_CreateObject();
      if (obj) {
        cJSON_AddStringToObject(obj, "$int", buf);
      }
      return obj;
    }
    return cJSON_CreateNumber((double)val->int_val);

  case DB_TYPE_FLOAT:
    /* JSON has no NaN/Infinity (cJSON would emit null, i.e. NULL) */
    if (isnan(val->float_val) || isinf(val->float_val)) {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "$float",
                              isnan(val->float_val) ? "nan"
                              : val->float_val > 0  ? "inf"
                                                    : "-inf");
      return obj;
    }
    return cJSON_CreateNumber(val->float_val);

  case DB_TYPE_DECIMAL: {
    /* Exact numeric: as its digits, never through a double */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "$num", val->text.data ? val->text.data : "0");
    return obj;
  }

  case DB_TYPE_TEXT:
  case DB_TYPE_DATE:
  case DB_TYPE_TIMESTAMP:
    if (val->omitted) {
      /* Too large to load: a label the client shows but must not write */
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "$omitted", val->text.data ? val->text.data : "");
      cJSON_AddNumberToObject(obj, "bytes", (double)val->omitted);
      return obj;
    }
    if (val->text.data) {
      return cJSON_CreateString(val->text.data);
    }
    return cJSON_CreateNull();

  case DB_TYPE_BLOB: {
    /* Bytes as {"$blob":"<hex>"}: a bare hex string was taken for text, so
     * BLOB keys never matched and edits wrote the hex as text */
    size_t n = val->blob.data ? val->blob.len : 0;
    char *hex = safe_malloc(n * 2 + 1);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
      hex[2 * i] = digits[val->blob.data[i] >> 4];
      hex[2 * i + 1] = digits[val->blob.data[i] & 15];
    }
    hex[2 * n] = '\0';
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "$blob", hex);
    free(hex);
    return obj;
  }

  case DB_TYPE_BOOL:
    return cJSON_CreateBool(val->bool_val);

  default:
    return cJSON_CreateNull();
  }
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
  if (rs->truncated) {
    cJSON_AddBoolToObject(result, "truncated", true);
  }
  if (rs->rows_affected >= 0) {
    cJSON_AddNumberToObject(result, "rows_affected", (double)rs->rows_affected);
  }

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

  /* Only an exact integer in range: valueint truncates 1.9 to 1 and
   * saturates out-of-range values, which would quietly address another
   * connection */
  double v = item->valuedouble;
  if (!(v >= INT_MIN && v <= INT_MAX) || v != (double)(int)v) {
    return false;
  }
  *out = (int)v;
  return true;
}

