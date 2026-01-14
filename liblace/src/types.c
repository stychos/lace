/*
 * liblace - Lace Client Library
 * Type utility implementations
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Safe string duplication */
static char *lace_strdup(const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char *copy = malloc(len + 1);
  if (copy) {
    memcpy(copy, s, len + 1);
  }
  return copy;
}

/* Safe string duplication with length */
static char *lace_strndup(const char *s, size_t n) {
  if (!s) return NULL;
  char *copy = malloc(n + 1);
  if (copy) {
    memcpy(copy, s, n);
    copy[n] = '\0';
  }
  return copy;
}

/* ==========================================================================
 * Type Name Functions
 * ========================================================================== */

const char *lace_type_name(LaceValueType type) {
  switch (type) {
  case LACE_TYPE_NULL:      return "NULL";
  case LACE_TYPE_INT:       return "INTEGER";
  case LACE_TYPE_FLOAT:     return "FLOAT";
  case LACE_TYPE_TEXT:      return "TEXT";
  case LACE_TYPE_BLOB:      return "BLOB";
  case LACE_TYPE_BOOL:      return "BOOLEAN";
  case LACE_TYPE_DATE:      return "DATE";
  case LACE_TYPE_TIMESTAMP: return "TIMESTAMP";
  default:                  return "UNKNOWN";
  }
}

const char *lace_filter_op_name(LaceFilterOp op) {
  switch (op) {
  case LACE_FILTER_EQ:           return "equals";
  case LACE_FILTER_NE:           return "not equals";
  case LACE_FILTER_GT:           return "greater than";
  case LACE_FILTER_GE:           return "greater or equal";
  case LACE_FILTER_LT:           return "less than";
  case LACE_FILTER_LE:           return "less or equal";
  case LACE_FILTER_IN:           return "in";
  case LACE_FILTER_CONTAINS:     return "contains";
  case LACE_FILTER_REGEX:        return "regex";
  case LACE_FILTER_BETWEEN:      return "between";
  case LACE_FILTER_IS_EMPTY:     return "is empty";
  case LACE_FILTER_IS_NOT_EMPTY: return "is not empty";
  case LACE_FILTER_IS_NULL:      return "is null";
  case LACE_FILTER_IS_NOT_NULL:  return "is not null";
  case LACE_FILTER_RAW:          return "raw";
  default:                       return "unknown";
  }
}

const char *lace_filter_op_sql(LaceFilterOp op) {
  switch (op) {
  case LACE_FILTER_EQ:           return "=";
  case LACE_FILTER_NE:           return "<>";
  case LACE_FILTER_GT:           return ">";
  case LACE_FILTER_GE:           return ">=";
  case LACE_FILTER_LT:           return "<";
  case LACE_FILTER_LE:           return "<=";
  case LACE_FILTER_IN:           return "IN";
  case LACE_FILTER_CONTAINS:     return "LIKE";
  case LACE_FILTER_REGEX:        return "~";      /* PostgreSQL style */
  case LACE_FILTER_BETWEEN:      return "BETWEEN";
  case LACE_FILTER_IS_EMPTY:     return "= ''";
  case LACE_FILTER_IS_NOT_EMPTY: return "<> ''";
  case LACE_FILTER_IS_NULL:      return "IS NULL";
  case LACE_FILTER_IS_NOT_NULL:  return "IS NOT NULL";
  case LACE_FILTER_RAW:          return "";
  default:                       return "=";
  }
}

bool lace_filter_op_needs_value(LaceFilterOp op) {
  switch (op) {
  case LACE_FILTER_IS_EMPTY:
  case LACE_FILTER_IS_NOT_EMPTY:
  case LACE_FILTER_IS_NULL:
  case LACE_FILTER_IS_NOT_NULL:
    return false;
  default:
    return true;
  }
}

/* ==========================================================================
 * Value Creation Helpers
 * ========================================================================== */

LaceValue lace_value_null(void) {
  LaceValue v = {0};
  v.type = LACE_TYPE_NULL;
  v.is_null = true;
  return v;
}

LaceValue lace_value_int(int64_t val) {
  LaceValue v = {0};
  v.type = LACE_TYPE_INT;
  v.is_null = false;
  v.int_val = val;
  return v;
}

LaceValue lace_value_float(double val) {
  LaceValue v = {0};
  v.type = LACE_TYPE_FLOAT;
  v.is_null = false;
  v.float_val = val;
  return v;
}

LaceValue lace_value_text(const char *str) {
  LaceValue v = {0};
  v.type = LACE_TYPE_TEXT;
  if (str) {
    v.text.data = lace_strdup(str);
    if (v.text.data) {
      v.text.len = strlen(str);
      v.is_null = false;
    } else {
      v.is_null = true;
    }
  } else {
    v.is_null = true;
  }
  return v;
}

LaceValue lace_value_text_len(const char *str, size_t len) {
  LaceValue v = {0};
  v.type = LACE_TYPE_TEXT;
  if (str) {
    v.text.data = lace_strndup(str, len);
    if (v.text.data) {
      v.text.len = len;
      v.is_null = false;
    } else {
      v.is_null = true;
    }
  } else {
    v.is_null = true;
  }
  return v;
}

LaceValue lace_value_blob(const uint8_t *data, size_t len) {
  LaceValue v = {0};
  v.type = LACE_TYPE_BLOB;
  if (data && len > 0) {
    v.blob.data = malloc(len);
    if (v.blob.data) {
      memcpy(v.blob.data, data, len);
      v.blob.len = len;
      v.is_null = false;
    } else {
      v.is_null = true;
    }
  } else {
    v.is_null = true;
  }
  return v;
}

LaceValue lace_value_bool(bool val) {
  LaceValue v = {0};
  v.type = LACE_TYPE_BOOL;
  v.is_null = false;
  v.bool_val = val;
  return v;
}

LaceValue lace_value_copy(const LaceValue *src) {
  LaceValue v = {0};
  if (!src) {
    v.type = LACE_TYPE_NULL;
    v.is_null = true;
    return v;
  }

  v.type = src->type;
  v.is_null = src->is_null;

  if (src->is_null) {
    return v;
  }

  switch (src->type) {
  case LACE_TYPE_TEXT:
  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    if (src->text.data) {
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

  case LACE_TYPE_BLOB:
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

  case LACE_TYPE_INT:
    v.int_val = src->int_val;
    break;

  case LACE_TYPE_FLOAT:
    v.float_val = src->float_val;
    break;

  case LACE_TYPE_BOOL:
    v.bool_val = src->bool_val;
    break;

  default:
    break;
  }

  return v;
}

/* ==========================================================================
 * Value Conversion
 * ========================================================================== */

char *lace_value_to_string(const LaceValue *val) {
  if (!val || val->is_null) {
    return lace_strdup("NULL");
  }

  char buf[64];

  switch (val->type) {
  case LACE_TYPE_NULL:
    return lace_strdup("NULL");

  case LACE_TYPE_INT:
    snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
    return lace_strdup(buf);

  case LACE_TYPE_FLOAT:
    snprintf(buf, sizeof(buf), "%g", val->float_val);
    return lace_strdup(buf);

  case LACE_TYPE_TEXT:
    return val->text.data ? lace_strdup(val->text.data) : lace_strdup("");

  case LACE_TYPE_BLOB:
    if (!val->blob.data || val->blob.len == 0) {
      return lace_strdup("x''");
    }
    /* Return hex representation for first 32 bytes */
    {
      size_t display_len = val->blob.len > 32 ? 32 : val->blob.len;
      size_t hex_len = 3 + display_len * 2 + (val->blob.len > 32 ? 3 : 0) + 1;
      char *hex = malloc(hex_len);
      if (!hex) return lace_strdup("x'...'");

      size_t pos = 0;
      hex[pos++] = 'x';
      hex[pos++] = '\'';
      for (size_t i = 0; i < display_len; i++) {
        snprintf(hex + pos, 3, "%02x", val->blob.data[i]);
        pos += 2;
      }
      if (val->blob.len > 32) {
        strcpy(hex + pos, "...");
        pos += 3;
      }
      hex[pos++] = '\'';
      hex[pos] = '\0';
      return hex;
    }

  case LACE_TYPE_BOOL:
    return lace_strdup(val->bool_val ? "true" : "false");

  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    return val->text.data ? lace_strdup(val->text.data) : lace_strdup("");

  default:
    return lace_strdup("???");
  }
}

/* ==========================================================================
 * Memory Management
 * ========================================================================== */

void lace_value_free(LaceValue *val) {
  if (!val) return;

  switch (val->type) {
  case LACE_TYPE_TEXT:
  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    free(val->text.data);
    val->text.data = NULL;
    val->text.len = 0;
    break;

  case LACE_TYPE_BLOB:
    free(val->blob.data);
    val->blob.data = NULL;
    val->blob.len = 0;
    break;

  default:
    break;
  }
  val->is_null = true;
}

void lace_row_free(LaceRow *row) {
  if (!row) return;

  if (row->cells) {
    for (size_t i = 0; i < row->num_cells; i++) {
      lace_value_free(&row->cells[i]);
    }
    free(row->cells);
    row->cells = NULL;
  }
  row->num_cells = 0;
}

static void lace_column_free(LaceColumn *col) {
  if (!col) return;

  free(col->name);
  free(col->type_name);
  free(col->default_val);
  free(col->foreign_key);
  memset(col, 0, sizeof(LaceColumn));
}

static void lace_index_free(LaceIndex *idx) {
  if (!idx) return;

  free(idx->name);
  free(idx->type);
  if (idx->columns) {
    for (size_t i = 0; i < idx->num_columns; i++) {
      free(idx->columns[i]);
    }
    free(idx->columns);
  }
  memset(idx, 0, sizeof(LaceIndex));
}

static void lace_fk_free(LaceForeignKey *fk) {
  if (!fk) return;

  free(fk->name);
  if (fk->columns) {
    for (size_t i = 0; i < fk->num_columns; i++) {
      free(fk->columns[i]);
    }
    free(fk->columns);
  }
  free(fk->ref_table);
  if (fk->ref_columns) {
    for (size_t i = 0; i < fk->num_ref_columns; i++) {
      free(fk->ref_columns[i]);
    }
    free(fk->ref_columns);
  }
  free(fk->on_delete);
  free(fk->on_update);
  memset(fk, 0, sizeof(LaceForeignKey));
}

void lace_schema_free(LaceSchema *schema) {
  if (!schema) return;

  free(schema->name);
  free(schema->schema);

  if (schema->columns) {
    for (size_t i = 0; i < schema->num_columns; i++) {
      lace_column_free(&schema->columns[i]);
    }
    free(schema->columns);
  }

  if (schema->indexes) {
    for (size_t i = 0; i < schema->num_indexes; i++) {
      lace_index_free(&schema->indexes[i]);
    }
    free(schema->indexes);
  }

  if (schema->foreign_keys) {
    for (size_t i = 0; i < schema->num_foreign_keys; i++) {
      lace_fk_free(&schema->foreign_keys[i]);
    }
    free(schema->foreign_keys);
  }

  free(schema);
}

void lace_result_free(LaceResult *result) {
  if (!result) return;

  if (result->columns) {
    for (size_t i = 0; i < result->num_columns; i++) {
      lace_column_free(&result->columns[i]);
    }
    free(result->columns);
  }

  if (result->rows) {
    for (size_t i = 0; i < result->num_rows; i++) {
      lace_row_free(&result->rows[i]);
    }
    free(result->rows);
  }

  free(result->source_table);
  free(result);
}

void lace_filter_free(LaceFilter *filter) {
  if (!filter) return;

  free(filter->value);
  free(filter->value2);
  memset(filter, 0, sizeof(LaceFilter));
}

void lace_conn_info_free(LaceConnInfo *info) {
  if (!info) return;

  free(info->database);
  free(info->host);
  free(info->user);
  memset(info, 0, sizeof(LaceConnInfo));
}
