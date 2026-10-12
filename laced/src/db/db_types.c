/*
 * Lace
 * Database type implementations
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "db_types.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *db_value_type_name(DbValueType type) {
  switch (type) {
  case DB_TYPE_NULL:
    return "NULL";
  case DB_TYPE_INT:
    return "INTEGER";
  case DB_TYPE_FLOAT:
    return "FLOAT";
  case DB_TYPE_TEXT:
    return "TEXT";
  case DB_TYPE_DECIMAL:
    return "DECIMAL";
  case DB_TYPE_BLOB:
    return "BLOB";
  case DB_TYPE_BOOL:
    return "BOOLEAN";
  case DB_TYPE_DATE:
    return "DATE";
  case DB_TYPE_TIMESTAMP:
    return "TIMESTAMP";
  default:
    return "UNKNOWN";
  }
}

DbValue db_value_oversized_placeholder(const char *type_label, size_t size) {
  DbValue v = {.type = DB_TYPE_TEXT, .is_null = false};
  v.text.data = NULL;
  v.text.len = 0;

  char placeholder[64];
  snprintf(placeholder, sizeof(placeholder), "[%s: %zu bytes]",
           type_label ? type_label : "DATA", size);

  v.text.data = str_dup(placeholder);
  v.omitted = size;
  if (v.text.data) {
    v.text.len = strlen(v.text.data);
  } else {
    v.type = DB_TYPE_NULL;
    v.is_null = true;
  }
  return v;
}

DbValue db_value_copy(const DbValue *src) {
  DbValue v = {0};
  if (!src) {
    v.is_null = true;
    v.type = DB_TYPE_NULL;
    return v;
  }

  v.type = src->type;
  v.is_null = src->is_null;

  if (src->is_null) {
    return v;
  }

  switch (src->type) {
  case DB_TYPE_DECIMAL:
  case DB_TYPE_TEXT:
  case DB_TYPE_DATE:
  case DB_TYPE_TIMESTAMP:
    /* DATE and TIMESTAMP are stored as text */
    if (src->text.data) {
      /* Check for overflow before adding 1 for null terminator */
      if (src->text.len == SIZE_MAX) {
        v.is_null = true;
        break;
      }
      v.text.data = safe_malloc(src->text.len + 1);
      memcpy(v.text.data, src->text.data, src->text.len);
      v.text.data[src->text.len] = '\0';
      v.text.len = src->text.len;
    }
    break;
  case DB_TYPE_BLOB:
    if (src->blob.data && src->blob.len > 0) {
      v.blob.data = safe_malloc(src->blob.len);
      memcpy(v.blob.data, src->blob.data, src->blob.len);
      v.blob.len = src->blob.len;
    }
    break;
  case DB_TYPE_INT:
    v.int_val = src->int_val;
    break;
  case DB_TYPE_FLOAT:
    v.float_val = src->float_val;
    break;
  case DB_TYPE_BOOL:
    v.bool_val = src->bool_val;
    break;
  default:
    break;
  }

  return v;
}

/* Memory management */

void db_value_free(DbValue *val) {
  if (!val)
    return;

  switch (val->type) {
  case DB_TYPE_DECIMAL:
  case DB_TYPE_TEXT:
  case DB_TYPE_DATE:
  case DB_TYPE_TIMESTAMP:
    /* DATE and TIMESTAMP store data in text.data (see db_value_copy) */
    free(val->text.data);
    val->text.data = NULL;
    val->text.len = 0;
    break;
  case DB_TYPE_BLOB:
    free(val->blob.data);
    val->blob.data = NULL;
    val->blob.len = 0;
    break;
  default:
    break;
  }
  val->is_null = true;
}

void db_row_free(Row *row) {
  if (!row)
    return;

  FREE_ARRAY(row->cells, row->num_cells, db_value_free);
  row->num_cells = 0;
}

void db_column_free(ColumnDef *col) {
  if (!col)
    return;

  free(col->name);
  free(col->type_name);
  free(col->default_val);
  free(col->foreign_key);
  memset(col, 0, sizeof(ColumnDef));
}

void db_result_free(ResultSet *rs) {
  if (!rs)
    return;

  FREE_ARRAY(rs->columns, rs->num_columns, db_column_free);
  FREE_ARRAY(rs->rows, rs->num_rows, db_row_free);

  free(rs->error);
  free(rs);
}

ResultSet *db_result_alloc_empty(void) { return safe_calloc(1, sizeof(ResultSet)); }

