/*
 * liblace - Lace Client Library
 * Type utility implementations
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/types.h"
#include "../include/util/mem.h"
#include "../include/util/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <math.h>

/* ==========================================================================
 * Type Name Functions
 * ========================================================================== */

const char *lace_type_name(LaceValueType type) {
  switch (type) {
  case LACE_TYPE_NULL:      return "NULL";
  case LACE_TYPE_INT:       return "INTEGER";
  case LACE_TYPE_FLOAT:     return "FLOAT";
  case LACE_TYPE_TEXT:      return "TEXT";
  case LACE_TYPE_DECIMAL:   return "DECIMAL";
  case LACE_TYPE_BLOB:      return "BLOB";
  case LACE_TYPE_BOOL:      return "BOOLEAN";
  case LACE_TYPE_DATE:      return "DATE";
  case LACE_TYPE_TIMESTAMP: return "TIMESTAMP";
  default:                  return "UNKNOWN";
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
    v.text.data = str_dup(str);
    v.text.len = strlen(str);
    v.is_null = false;
  } else {
    v.is_null = true;
  }
  return v;
}

LaceValue lace_value_text_len(const char *str, size_t len) {
  LaceValue v = {0};
  v.type = LACE_TYPE_TEXT;
  if (str) {
    v.text.data = str_ndup(str, len);
    v.text.len = len;
    v.is_null = false;
  } else {
    v.is_null = true;
  }
  return v;
}

LaceValue lace_value_blob(const uint8_t *data, size_t len) {
  LaceValue v = {0};
  v.type = LACE_TYPE_BLOB;
  if (!data) {
    v.is_null = true; /* No bytes at all: NULL. len 0 with data: empty BLOB */
  } else if (len > 0) {
    v.blob.data = safe_malloc(len);
    memcpy(v.blob.data, data, len);
    v.blob.len = len;
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
  v.omitted = src->omitted;

  if (src->is_null) {
    return v;
  }

  switch (src->type) {
  case LACE_TYPE_DECIMAL:
  case LACE_TYPE_TEXT:
  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    if (src->text.data) {
      v.text.data = safe_malloc(src->text.len + 1);
      memcpy(v.text.data, src->text.data, src->text.len);
      v.text.data[src->text.len] = '\0';
      v.text.len = src->text.len;
    }
    break;

  case LACE_TYPE_BLOB:
    if (src->blob.data && src->blob.len > 0) {
      v.blob.data = safe_malloc(src->blob.len);
      memcpy(v.blob.data, src->blob.data, src->blob.len);
      v.blob.len = src->blob.len;
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
    return str_dup("NULL");
  }

  char buf[64];

  switch (val->type) {
  case LACE_TYPE_NULL:
    return str_dup("NULL");

  case LACE_TYPE_INT:
    snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
    return str_dup(buf);

  case LACE_TYPE_FLOAT:
    /* Exact (shortest round-trip, '.' whatever the locale): "%g" showed 6
     * digits, and edits wrote the rounded value back */
    lace_format_double(val->float_val, buf, sizeof(buf));
    return str_dup(buf);

  case LACE_TYPE_DECIMAL:
  case LACE_TYPE_TEXT:
    return val->text.data ? str_dup(val->text.data) : str_dup("");

  case LACE_TYPE_BLOB:
    if (!val->blob.data || val->blob.len == 0) {
      return str_dup("x''");
    }
    /* Return hex representation for first 32 bytes */
    {
      size_t display_len = val->blob.len > 32 ? 32 : val->blob.len;
      size_t hex_len = 3 + display_len * 2 + (val->blob.len > 32 ? 3 : 0) + 1;
      char *hex = safe_malloc(hex_len);

      size_t pos = 0;
      hex[pos++] = 'x';
      hex[pos++] = '\'';
      for (size_t i = 0; i < display_len; i++) {
        snprintf(hex + pos, 3, "%02x", val->blob.data[i]);
        pos += 2;
      }
      if (val->blob.len > 32) {
        memcpy(hex + pos, "...", 3);
        pos += 3;
      }
      hex[pos++] = '\'';
      hex[pos] = '\0';
      return hex;
    }

  case LACE_TYPE_BOOL:
    return str_dup(val->bool_val ? "true" : "false");

  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    return val->text.data ? str_dup(val->text.data) : str_dup("");

  default:
    return str_dup("???");
  }
}

void lace_format_double(double v, char *buf, size_t size) {
  if (isnan(v)) {
    snprintf(buf, size, "NaN");
    return;
  }
  if (isinf(v)) {
    snprintf(buf, size, v > 0 ? "Infinity" : "-Infinity");
    return;
  }
  snprintf(buf, size, "%.15g", v);
  const char *dp = localeconv()->decimal_point;
  char sep = (dp && dp[0]) ? dp[0] : '.';
  if (strtod(buf, NULL) != v) {
    snprintf(buf, size, "%.17g", v);
  }
  if (sep != '.') {
    for (char *p = buf; *p; p++) {
      if (*p == sep) {
        *p = '.';
      }
    }
  }
}

char *lace_value_to_edit_text(const LaceValue *val) {
  if (!val || val->is_null) {
    return str_dup("");
  }
  if (val->type == LACE_TYPE_BLOB) {
    /* Every byte as hex (the display form shows only the first 32) */
    static const char hex[] = "0123456789abcdef";
    size_t n = val->blob.data ? val->blob.len : 0;
    char *out = safe_malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) {
      out[2 * i] = hex[val->blob.data[i] >> 4];
      out[2 * i + 1] = hex[val->blob.data[i] & 15];
    }
    out[2 * n] = '\0';
    return out;
  }
  return lace_value_to_string(val);
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* A number as SQL accepts it: [+-]digits[.digits][e[+-]digits] */
static bool is_plain_number(const char *s) {
  const char *p = s;
  if (*p == '+' || *p == '-') p++;
  bool digits = false;
  while (isdigit((unsigned char)*p)) { p++; digits = true; }
  if (*p == '.') {
    p++;
    while (isdigit((unsigned char)*p)) { p++; digits = true; }
  }
  if (!digits) return false;
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
  }
  return *p == '\0';
}

bool lace_value_from_edit_text(const char *text, LaceValueType like,
                               LaceValue *out, char **err) {
  memset(out, 0, sizeof(*out));
  if (!text) {
    text = "";
  }
  switch (like) {
  case LACE_TYPE_BLOB: {
    /* Hex digits, optionally as x'..', X'..', \x.. or 0x.. */
    const char *p = text;
    size_t len = strlen(p);
    if ((p[0] == 'x' || p[0] == 'X') && p[1] == '\'' && len >= 3 &&
        p[len - 1] == '\'') {
      p += 2;
      len -= 3;
    } else if ((p[0] == '\\' && (p[1] == 'x' || p[1] == 'X')) ||
               (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) {
      p += 2;
      len -= 2;
    }
    if (len % 2 != 0) {
      if (err) *err = str_dup("BLOB value: enter an even number of hex digits");
      return false;
    }
    uint8_t *bytes = safe_malloc(len / 2 + 1);
    for (size_t i = 0; i < len / 2; i++) {
      int hi = hex_digit(p[2 * i]), lo = hex_digit(p[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        free(bytes);
        if (err) *err = str_dup("BLOB value: only hex digits (0-9, a-f) are allowed");
        return false;
      }
      bytes[i] = (uint8_t)(hi << 4 | lo);
    }
    out->type = LACE_TYPE_BLOB;
    out->blob.data = bytes;
    out->blob.len = len / 2;
    return true;
  }
  case LACE_TYPE_DECIMAL:
    if (is_plain_number(text)) {
      out->type = LACE_TYPE_DECIMAL; /* Sent unquoted: exact on every DB */
      out->text.data = str_dup(text);
      out->text.len = strlen(text);
      return true;
    }
    break;
  case LACE_TYPE_INT: {
    char *end = NULL;
    errno = 0;
    long long n = strtoll(text, &end, 10);
    if (*text && errno == 0 && end && *end == '\0') {
      out->type = LACE_TYPE_INT;
      out->int_val = n;
      return true;
    }
    break;
  }
  default:
    break;
  }
  /* Anything else as text: the database converts it to the column's type
   * (or rejects it), exactly as typed */
  out->type = LACE_TYPE_TEXT;
  out->text.data = str_dup(text);
  out->text.len = strlen(text);
  return true;
}

/* ==========================================================================
 * Memory Management
 * ========================================================================== */

void lace_value_free(LaceValue *val) {
  if (!val) return;

  switch (val->type) {
  case LACE_TYPE_DECIMAL:
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

void lace_conn_info_free(LaceConnInfo *info) {
  if (!info) return;

  free(info->database);
  free(info->host);
  free(info->user);
  memset(info, 0, sizeof(LaceConnInfo));
}
