/*
 * Lace
 * Database type implementations
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "db_types.h"
#include "../util/str.h"
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

DbValueType db_value_type_from_name(const char *name) {
  if (!name)
    return DB_TYPE_NULL;

  /* Use case-insensitive matching (strcasestr is GNU extension) */
  if (strcasestr(name, "INT")) {
    return DB_TYPE_INT;
  } else if (strcasestr(name, "SERIAL")) {
    return DB_TYPE_INT;
  } else if (strcasestr(name, "FLOAT") || strcasestr(name, "DOUBLE") ||
             strcasestr(name, "REAL") || strcasestr(name, "NUMERIC") ||
             strcasestr(name, "DECIMAL")) {
    return DB_TYPE_FLOAT;
  } else if (strcasestr(name, "BOOL")) {
    return DB_TYPE_BOOL;
  } else if (strcasestr(name, "BLOB") || strcasestr(name, "BYTEA") ||
             strcasestr(name, "BINARY")) {
    return DB_TYPE_BLOB;
  } else if (strcasecmp(name, "DATE") == 0) {
    return DB_TYPE_DATE;
  } else if (strcasestr(name, "TIMESTAMP") || strcasestr(name, "DATETIME")) {
    return DB_TYPE_TIMESTAMP;
  }
  return DB_TYPE_TEXT; /* Default */
}

/* Value creation helpers */

DbValue db_value_null(void) {
  DbValue v = {.type = DB_TYPE_NULL, .is_null = true};
  return v;
}

DbValue db_value_int(int64_t val) {
  DbValue v = {.type = DB_TYPE_INT, .int_val = val, .is_null = false};
  return v;
}

DbValue db_value_float(double val) {
  DbValue v = {.type = DB_TYPE_FLOAT, .float_val = val, .is_null = false};
  return v;
}

DbValue db_value_text(const char *str) {
  DbValue v = {.type = DB_TYPE_TEXT, .is_null = false};
  /* Explicitly initialize text fields */
  v.text.data = NULL;
  v.text.len = 0;
  if (str) {
    v.text.len = strlen(str);
    v.text.data = str_dup(str);
  } else {
    v.is_null = true;
  }
  return v;
}

DbValue db_value_text_len(const char *str, size_t len) {
  DbValue v = {.type = DB_TYPE_TEXT, .is_null = false};
  /* Explicitly initialize text fields */
  v.text.data = NULL;
  v.text.len = 0;
  if (str) {
    v.text.len = len;
    v.text.data = str_ndup(str, len);
  } else {
    v.is_null = true;
  }
  return v;
}

DbValue db_value_blob(const uint8_t *data, size_t len) {
  DbValue v = {.type = DB_TYPE_BLOB, .is_null = false};
  /* Explicitly initialize blob fields */
  v.blob.data = NULL;
  v.blob.len = 0;
  if (data && len > 0) {
    v.blob.data = malloc(len);
    if (v.blob.data) {
      memcpy(v.blob.data, data, len);
      v.blob.len = len;
    } else {
      /* Malloc failed - return null value */
      v.is_null = true;
    }
  } else {
    v.is_null = true;
  }
  return v;
}

DbValue db_value_bool(bool val) {
  DbValue v = {.type = DB_TYPE_BOOL, .bool_val = val, .is_null = false};
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
      v.text.data = malloc(src->text.len + 1);
      if (v.text.data) {
        memcpy(v.text.data, src->text.data, src->text.len);
        v.text.data[src->text.len] = '\0';
        v.text.len = src->text.len;
      } else {
        v.is_null = true;
      }
    }
    break;
  case DB_TYPE_BLOB:
    if (src->blob.data && src->blob.len > 0) {
      v.blob.data = malloc(src->blob.len);
      if (v.blob.data) {
        memcpy(v.blob.data, src->blob.data, src->blob.len);
        v.blob.len = src->blob.len;
      } else {
        v.is_null = true;
      }
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
  case DB_TYPE_TEXT:
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

  for (size_t i = 0; i < row->num_cells; i++) {
    db_value_free(&row->cells[i]);
  }
  free(row->cells);
  row->cells = NULL;
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

void db_index_free(IndexDef *idx) {
  if (!idx)
    return;

  free(idx->name);
  free(idx->type);
  for (size_t i = 0; i < idx->num_columns; i++) {
    free(idx->columns[i]);
  }
  free(idx->columns);
  memset(idx, 0, sizeof(IndexDef));
}

void db_fk_free(ForeignKeyDef *fk) {
  if (!fk)
    return;

  free(fk->name);
  for (size_t i = 0; i < fk->num_columns; i++) {
    free(fk->columns[i]);
  }
  free(fk->columns);
  free(fk->ref_table);
  for (size_t i = 0; i < fk->num_ref_columns; i++) {
    free(fk->ref_columns[i]);
  }
  free(fk->ref_columns);
  free(fk->on_delete);
  free(fk->on_update);
  memset(fk, 0, sizeof(ForeignKeyDef));
}

void db_schema_free(TableSchema *schema) {
  if (!schema)
    return;

  free(schema->name);
  free(schema->schema);

  for (size_t i = 0; i < schema->num_columns; i++) {
    db_column_free(&schema->columns[i]);
  }
  free(schema->columns);

  for (size_t i = 0; i < schema->num_indexes; i++) {
    db_index_free(&schema->indexes[i]);
  }
  free(schema->indexes);

  for (size_t i = 0; i < schema->num_foreign_keys; i++) {
    db_fk_free(&schema->foreign_keys[i]);
  }
  free(schema->foreign_keys);

  free(schema);
}

void db_result_free(ResultSet *rs) {
  if (!rs)
    return;

  for (size_t i = 0; i < rs->num_columns; i++) {
    db_column_free(&rs->columns[i]);
  }
  free(rs->columns);

  for (size_t i = 0; i < rs->num_rows; i++) {
    db_row_free(&rs->rows[i]);
  }
  free(rs->rows);

  free(rs->error);
  free(rs);
}

/* Value conversion */

char *db_value_to_string(const DbValue *val) {
  if (!val || val->is_null) {
    return str_dup("NULL");
  }

  switch (val->type) {
  case DB_TYPE_NULL:
    return str_dup("NULL");

  case DB_TYPE_INT:
    return str_printf("%lld", (long long)val->int_val);

  case DB_TYPE_FLOAT:
    return str_printf("%g", val->float_val);

  case DB_TYPE_TEXT:
    return val->text.data ? str_dup(val->text.data) : str_dup("");

  case DB_TYPE_BLOB: {
    /* Try to display as text if it's valid UTF-8 with printable chars */
    if (val->blob.data && val->blob.len > 0) {
      bool is_text = true;
      for (size_t i = 0; i < val->blob.len; i++) {
        unsigned char c = val->blob.data[i];
        /* Allow printable ASCII, tab, newline, and UTF-8 continuation bytes */
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') {
          is_text = false;
          break;
        }
        if (c == 127) { /* DEL character */
          is_text = false;
          break;
        }
      }
      if (is_text) {
        /* Return as string */
        char *str = malloc(val->blob.len + 1);
        if (str) {
          memcpy(str, val->blob.data, val->blob.len);
          str[val->blob.len] = '\0';
          return str;
        }
      }
    }

    /* Convert to hex for binary data (show max 32 bytes) */
    /* Handle empty or null blob data */
    if (!val->blob.data || val->blob.len == 0) {
      return str_dup("x''");
    }
    size_t display_len = val->blob.len > 32 ? 32 : val->blob.len;
    /* Format: x'HEXHEX...' + optional "..." = 2 + display_len*2 + 1 + 3 + 1
     * With display_len <= 32, hex_len is at most 2 + 64 + 1 + 3 + 1 = 71 bytes
     */
    size_t hex_len =
        2 + (display_len * 2) + 1 + (val->blob.len > 32 ? 3 : 0) + 1;
    char *hex = malloc(hex_len);
    if (!hex)
      return NULL;

    size_t pos = 0;
    hex[pos++] = 'x';
    hex[pos++] = '\'';
    for (size_t i = 0; i < display_len; i++) {
      int written =
          snprintf(hex + pos, hex_len - pos, "%02x", val->blob.data[i]);
      if (written < 0 || pos + (size_t)written >= hex_len)
        break;
      pos += (size_t)written;
    }
    if (val->blob.len > 32) {
      int written = snprintf(hex + pos, hex_len - pos, "...");
      if (written > 0 && pos + (size_t)written < hex_len)
        pos += (size_t)written;
    }
    hex[pos++] = '\'';
    hex[pos] = '\0';
    return hex;
  }

  case DB_TYPE_BOOL:
    return str_dup(val->bool_val ? "true" : "false");

  case DB_TYPE_DATE:
  case DB_TYPE_TIMESTAMP:
    /* These are typically stored as text */
    return val->text.data ? str_dup(val->text.data) : str_dup("");

  default:
    return str_dup("???");
  }
}

bool db_value_to_bool(const DbValue *val) {
  if (!val || val->is_null)
    return false;

  switch (val->type) {
  case DB_TYPE_BOOL:
    return val->bool_val;
  case DB_TYPE_INT:
    return val->int_val != 0;
  case DB_TYPE_FLOAT:
    return val->float_val != 0.0;
  case DB_TYPE_TEXT:
    if (!val->text.data)
      return false;
    return str_eq_nocase(val->text.data, "true") ||
           str_eq_nocase(val->text.data, "yes") ||
           str_eq_nocase(val->text.data, "1");
  default:
    return false;
  }
}

int64_t db_value_to_int(const DbValue *val) {
  if (!val || val->is_null)
    return 0;

  switch (val->type) {
  case DB_TYPE_INT:
    return val->int_val;
  case DB_TYPE_FLOAT:
    return (int64_t)val->float_val;
  case DB_TYPE_BOOL:
    return val->bool_val ? 1 : 0;
  case DB_TYPE_TEXT: {
    int64_t result = 0;
    if (val->text.data) {
      str_to_int64(val->text.data, &result);
    }
    return result;
  }
  default:
    return 0;
  }
}

double db_value_to_float(const DbValue *val) {
  if (!val || val->is_null)
    return 0.0;

  switch (val->type) {
  case DB_TYPE_FLOAT:
    return val->float_val;
  case DB_TYPE_INT:
    return (double)val->int_val;
  case DB_TYPE_BOOL:
    return val->bool_val ? 1.0 : 0.0;
  case DB_TYPE_TEXT: {
    double result = 0.0;
    if (val->text.data) {
      str_to_double(val->text.data, &result);
    }
    return result;
  }
  default:
    return 0.0;
  }
}
