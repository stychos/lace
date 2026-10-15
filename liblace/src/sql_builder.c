/*
 * liblace - Lace Client Library
 * SQL synthesis and statement scanning for frontends (see sql_builder.h)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "sql_builder.h"
#include "lace.h"
#include <util/mem.h>
#include <util/str.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Values in an IN (...) list, at most (DoS guard, as the TUI's MAX_IN_VALUES) */
#define SQL_MAX_IN_VALUES 1000

/* Bytes of a BLOB shown as hex in a cell */
#define SQL_BLOB_PREVIEW_BYTES 12

/* ==========================================================================
 * Value display
 * ========================================================================== */

bool lace_type_is_numeric(LaceValueType t) {
  return t == LACE_TYPE_INT || t == LACE_TYPE_FLOAT || t == LACE_TYPE_DECIMAL;
}

/* Cut s to at most max_chars UTF-8 characters, "…" appended when cut */
static char *limit_chars(char *s, size_t max_chars) {
  if (max_chars == 0)
    return s;
  size_t chars = 0;
  for (const char *p = s; *p; p++) {
    if (((unsigned char)*p & 0xC0) != 0x80)
      chars++;
    if (chars > max_chars) {
      size_t at = (size_t)(p - s);
      char *cut = str_printf("%.*s…", (int)at, s);
      free(s);
      return cut;
    }
  }
  return s;
}

char *lace_value_display_text(const LaceValue *v, size_t max_chars) {
  if (!v || v->is_null || v->type == LACE_TYPE_NULL)
    return str_dup("NULL");
  if (v->omitted)
    return str_dup(v->text.data ? v->text.data : "[omitted]");

  switch (v->type) {
  case LACE_TYPE_INT:
    return str_printf("%" PRId64, v->int_val);
  case LACE_TYPE_FLOAT: {
    char buf[64];
    lace_format_double(v->float_val, buf, sizeof(buf));
    return str_dup(buf);
  }
  case LACE_TYPE_BOOL:
    return str_dup(v->bool_val ? "true" : "false");
  case LACE_TYPE_BLOB: {
    StringBuilder *sb = sb_new(64);
    sb_append(sb, "0x");
    size_t n = v->blob.len < SQL_BLOB_PREVIEW_BYTES ? v->blob.len
                                                    : SQL_BLOB_PREVIEW_BYTES;
    for (size_t i = 0; i < n; i++)
      sb_printf(sb, "%02x", v->blob.data[i]);
    if (v->blob.len > n)
      sb_append(sb, "…");
    sb_printf(sb, " (%zu bytes)", v->blob.len);
    return sb_finish(sb);
  }
  default: {
    const char *s = v->text.data ? v->text.data : "";
    size_t len = v->text.len ? v->text.len : strlen(s);
    StringBuilder *sb = sb_new(len + 8);
    for (size_t i = 0; i < len; i++) {
      char c = s[i];
      if (c == '\n')
        sb_append(sb, "⏎");
      else if (c == '\r')
        continue;
      else if (c == '\t')
        sb_append(sb, "  ");
      else if (c == '\0')
        sb_append(sb, "␀");
      else
        sb_append_char(sb, c);
    }
    return limit_chars(sb_finish(sb), max_chars);
  }
  }
}

bool lace_edit_value(const LaceValue *orig, LaceValueType col_type,
                    const char *text, LaceValue *out, bool *unchanged,
                    char **err) {
  *unchanged = false;
  memset(out, 0, sizeof(*out));
  bool orig_null = !orig || orig->is_null || orig->type == LACE_TYPE_NULL;
  if (!text) {
    *unchanged = orig_null;
    *out = lace_value_null();
    return true;
  }
  if (orig_null && text[0] == '\0') {
    *unchanged = true;
    return true;
  }
  if (!orig_null) {
    char *before = lace_value_to_edit_text(orig);
    bool same = before && strcmp(before, text) == 0;
    free(before);
    if (same) {
      *unchanged = true;
      return true;
    }
  }
  LaceValueType like = orig_null ? col_type : orig->type;
  return lace_value_from_edit_text(text, like, out, err);
}

/* ==========================================================================
 * Statement shape
 * ========================================================================== */

static bool word_is(const char *w, size_t len, const char *kw) {
  return strlen(kw) == len && strncasecmp(w, kw, len) == 0;
}

void lace_sql_shape(const char *sql, LaceDriver driver, LaceSqlShape *out) {
  memset(out, 0, sizeof(*out));
  if (!sql)
    return;
  static const char *const verbs[] = {"SELECT", "INSERT", "UPDATE", "DELETE",
                                      "MERGE",  "REPLACE", "VALUES", "TABLE",
                                      "UPSERT", NULL};
  int depth = 0;
  const char *s = sql;
  while (*s) {
    char c = *s;
    bool comment;
    size_t skip = lace_sql_skip(s, driver, &comment);
    if (skip) {
      s += skip;
      if (!comment)
        out->end = (size_t)(s - sql);
      continue;
    }
    if (isspace((unsigned char)c) || c == ';') {
      s++;
      continue;
    }
    if (c == '(') {
      depth++;
    } else if (c == ')' && depth > 0) {
      depth--;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      const char *w = s;
      size_t len = lace_sql_word_length(s, driver);
      if (len == 0)
        len = 1;
      s += len;
      if (!out->first_keyword[0] && len < sizeof(out->first_keyword)) {
        for (size_t i = 0; i < len; i++)
          out->first_keyword[i] = (char)toupper((unsigned char)w[i]);
        out->first_keyword[len] = '\0';
      }
      if (depth == 0 && !out->verb[0]) {
        for (int v = 0; verbs[v]; v++) {
          if (word_is(w, len, verbs[v])) {
            memcpy(out->verb, verbs[v], len + 1);
            break;
          }
        }
      }
      if (depth == 0 && word_is(w, len, "INTO"))
        out->top_level_into = true;
      if (depth == 0 && (word_is(w, len, "LIMIT") || word_is(w, len, "OFFSET") ||
                         word_is(w, len, "FETCH")))
        out->top_level_limit = true;
      out->end = (size_t)(s - sql);
      continue;
    }
    s++;
    out->end = (size_t)(s - sql);
  }
  out->nested_write = !lace_sql_read_only(sql, driver);
}

bool lace_sql_shape_pageable(const LaceSqlShape *s) {
  const char *kw = s->first_keyword;
  const char *v = s->verb;
  bool starts_query = strcmp(kw, "SELECT") == 0 || strcmp(kw, "WITH") == 0 ||
                      strcmp(kw, "VALUES") == 0 || strcmp(kw, "TABLE") == 0;
  bool reads = strcmp(v, "SELECT") == 0 || strcmp(v, "VALUES") == 0 ||
               strcmp(v, "TABLE") == 0;
  return starts_query && reads && !s->top_level_into && !s->nested_write &&
         s->end > 0;
}

bool lace_sql_shape_needs_wrap(const LaceSqlShape *s) {
  return s->top_level_limit || strcmp(s->verb, "VALUES") == 0;
}

char *lace_sql_page_of_statement(const char *base, bool wrap, size_t limit,
                            size_t offset) {
  if (wrap)
    return str_printf("SELECT * FROM (\n%s\n) AS lace_page LIMIT %zu OFFSET %zu",
                      base, limit, offset);
  return str_printf("%s\nLIMIT %zu OFFSET %zu", base, limit, offset);
}

char *lace_sql_count_of_statement(const char *base) {
  return str_printf("SELECT COUNT(*) FROM (\n%s\n) AS lace_count", base);
}

/* ==========================================================================
 * Statement boundaries
 * ========================================================================== */

/* [start, end) without surrounding whitespace */
static void trim_range(const char *text, size_t *start, size_t *end) {
  while (*start < *end && isspace((unsigned char)text[*start]))
    (*start)++;
  while (*end > *start && isspace((unsigned char)text[*end - 1]))
    (*end)--;
}

/* True if [start, end) holds nothing but whitespace and comments */
static bool range_is_blank(const char *text, size_t start, size_t end,
                           LaceDriver driver) {
  size_t p = start;
  while (p < end) {
    if (isspace((unsigned char)text[p])) {
      p++;
      continue;
    }
    bool comment;
    size_t skip = lace_sql_skip(text + p, driver, &comment);
    if (skip && comment) {
      p += skip;
      continue;
    }
    return false;
  }
  return true;
}

/* Walk statements. Each call finds the raw statement at pos: [pos, raw_end)
 * where raw_end is the ';' or the end, and sets *after to the position past
 * the ';'. */
static size_t raw_statement(const char *text, size_t pos, LaceDriver driver,
                            size_t *after) {
  size_t len = lace_sql_statement_length(text + pos, driver);
  size_t raw_end = pos + len;
  *after = raw_end + (text[raw_end] == ';' ? 1 : 0);
  return raw_end;
}

bool lace_sql_statement_at(const char *text, size_t cursor, LaceDriver driver,
                      size_t *start, size_t *end) {
  *start = *end = 0;
  if (!text || !*text)
    return false;
  size_t len = strlen(text);
  if (cursor > len)
    cursor = len;

  bool have_prev = false;
  size_t prev_s = 0, prev_e = 0;
  size_t pos = 0;
  while (pos < len) {
    size_t after;
    size_t raw_end = raw_statement(text, pos, driver, &after);
    size_t s = pos, e = raw_end;
    trim_range(text, &s, &e);
    bool blank = s >= e || range_is_blank(text, s, e, driver);
    if (!blank) {
      if (cursor < s) {
        /* In the gap before this statement: the previous one, if any */
        if (have_prev) {
          *start = prev_s;
          *end = prev_e;
        } else {
          *start = s;
          *end = e;
        }
        return true;
      }
      if (cursor <= raw_end) {
        *start = s;
        *end = e;
        return true;
      }
      have_prev = true;
      prev_s = s;
      prev_e = e;
    }
    if (after <= pos)
      break;
    pos = after;
  }
  if (have_prev) {
    *start = prev_s;
    *end = prev_e;
    return true;
  }
  return false;
}

bool lace_sql_next_statement(const char *text, size_t from, LaceDriver driver,
                        size_t *start, size_t *end, size_t *next) {
  if (!text)
    return false;
  size_t len = strlen(text);
  size_t pos = from;
  while (pos < len) {
    size_t after;
    size_t raw_end = raw_statement(text, pos, driver, &after);
    size_t s = pos, e = raw_end;
    trim_range(text, &s, &e);
    bool blank = s >= e || range_is_blank(text, s, e, driver);
    if (after <= pos)
      after = len;
    if (!blank) {
      *start = s;
      *end = e;
      *next = after;
      return true;
    }
    pos = after;
  }
  *next = len;
  return false;
}

/* ==========================================================================
 * Filters and sorts
 * ========================================================================== */

static const struct {
  const char *key;
  const char *label;
  bool needs_value;
  bool two_values;
} g_ops[LACE_FILTER_OP_COUNT] = {
    [LACE_FILTER_EQ] = {"eq", "=", true, false},
    [LACE_FILTER_NE] = {"ne", "≠", true, false},
    [LACE_FILTER_GT] = {"gt", ">", true, false},
    [LACE_FILTER_GE] = {"ge", "≥", true, false},
    [LACE_FILTER_LT] = {"lt", "<", true, false},
    [LACE_FILTER_LE] = {"le", "≤", true, false},
    [LACE_FILTER_IN] = {"in", "in list", true, false},
    [LACE_FILTER_CONTAINS] = {"contains", "contains", true, false},
    [LACE_FILTER_STARTS] = {"starts", "starts with", true, false},
    [LACE_FILTER_ENDS] = {"ends", "ends with", true, false},
    [LACE_FILTER_REGEX] = {"regex", "matches regex", true, false},
    [LACE_FILTER_BETWEEN] = {"between", "between", true, true},
    [LACE_FILTER_EMPTY] = {"empty", "is empty", false, false},
    [LACE_FILTER_NOT_EMPTY] = {"not_empty", "is not empty", false, false},
    [LACE_FILTER_NULL] = {"null", "is NULL", false, false},
    [LACE_FILTER_NOT_NULL] = {"not_null", "is not NULL", false, false},
    [LACE_FILTER_RAW] = {"raw", "raw SQL", true, false},
};

const char *lace_filter_op_label(LaceFilterOp op) {
  return op < LACE_FILTER_OP_COUNT ? g_ops[op].label : "?";
}

const char *lace_filter_op_key(LaceFilterOp op) {
  return op < LACE_FILTER_OP_COUNT ? g_ops[op].key : "eq";
}

LaceFilterOp lace_filter_op_from_key(const char *key) {
  for (int i = 0; key && i < LACE_FILTER_OP_COUNT; i++) {
    if (strcmp(g_ops[i].key, key) == 0)
      return (LaceFilterOp)i;
  }
  return LACE_FILTER_EQ;
}

bool lace_filter_op_needs_value(LaceFilterOp op) {
  return op < LACE_FILTER_OP_COUNT && g_ops[op].needs_value;
}

bool lace_filter_op_two_values(LaceFilterOp op) {
  return op < LACE_FILTER_OP_COUNT && g_ops[op].two_values;
}

void lace_filter_clear(LaceFilter *f) {
  if (!f)
    return;
  free(f->column);
  free(f->value);
  free(f->value2);
  memset(f, 0, sizeof(*f));
}

static bool is_plain_number(const char *start, const char *end) {
  const char *c = start;
  if (c < end && (*c == '-' || *c == '+'))
    c++;
  bool digits = false, dot = false;
  for (; c < end; c++) {
    if (isdigit((unsigned char)*c)) {
      digits = true;
    } else if (*c == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  return digits;
}

/* A literal for a comparison: a plain number unquoted, anything else a
 * string literal the database converts */
static char *compare_literal(LaceDriver driver, const char *value) {
  if (!value)
    value = "";
  size_t n = strlen(value);
  if (n > 0 && n < 40 && is_plain_number(value, value + n))
    return str_dup(value);
  return lace_sql_string_literal(driver, value);
}

static char *pattern_literal(LaceDriver driver, const char *prefix,
                             const char *value, const char *suffix) {
  char *pattern = str_printf("%s%s%s", prefix, value ? value : "", suffix);
  char *lit = lace_sql_string_literal(driver, pattern);
  free(pattern);
  return lit;
}

/* "a, 'b c', 3" -> "'a', 'b c', 3" (numbers inlined); NULL if empty */
static char *in_list(LaceDriver driver, const char *input) {
  if (!input || !*input)
    return NULL;
  StringBuilder *sb = sb_new(strlen(input) * 2);
  const char *p = input;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '(')
    p++;
  size_t count = 0;
  bool first = true;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p || *p == ')')
      break;
    if (count++ >= SQL_MAX_IN_VALUES)
      break;
    if (!first)
      sb_append(sb, ", ");
    first = false;
    StringBuilder *val = sb_new(32);
    bool number = false;
    if (*p == '\'' || *p == '"') {
      char quote = *p++;
      while (*p) {
        if (*p == quote && p[1] != quote)
          break;
        if ((*p == quote || *p == '\\') && p[1])
          p++;
        sb_append_char(val, *p++);
      }
      if (*p == quote)
        p++;
    } else {
      const char *start = p;
      while (*p && *p != ',' && *p != ')')
        p++;
      const char *end = p;
      while (end > start && isspace((unsigned char)*(end - 1)))
        end--;
      number = end > start && is_plain_number(start, end);
      sb_append_len(val, start, (size_t)(end - start));
    }
    char *text = sb_finish(val);
    char *lit = number ? text : lace_sql_string_literal(driver, text);
    sb_append(sb, lit);
    if (lit != text)
      free(lit);
    free(text);
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == ',')
      p++;
  }
  if (first) {
    sb_free(sb);
    return NULL;
  }
  return sb_finish(sb);
}

static bool schema_has_column(const LaceSchema *schema, const char *name) {
  for (size_t i = 0; schema && name && i < schema->num_columns; i++) {
    if (schema->columns[i].name && strcmp(schema->columns[i].name, name) == 0)
      return true;
  }
  return false;
}

/* One filter's condition, or NULL when it does not apply */
static char *filter_condition(const LaceFilter *f, const LaceSchema *schema,
                              LaceDriver driver) {
  bool raw = f->op == LACE_FILTER_RAW || !f->column;
  const char *value = f->value ? f->value : "";
  if (raw) {
    size_t s = 0, e = strlen(value);
    trim_range(value, &s, &e);
    return e > s ? str_printf("(%.*s)", (int)(e - s), value + s) : NULL;
  }
  if (lace_filter_op_needs_value(f->op) && !*value)
    return NULL;
  if (f->op == LACE_FILTER_BETWEEN && (!f->value2 || !*f->value2))
    return NULL;
  if (!schema_has_column(schema, f->column))
    return NULL;

  char *col = lace_quote_identifier(driver, f->column);
  char *out = NULL;
  switch (f->op) {
  case LACE_FILTER_EQ:
  case LACE_FILTER_NE:
  case LACE_FILTER_GT:
  case LACE_FILTER_GE:
  case LACE_FILTER_LT:
  case LACE_FILTER_LE: {
    static const char *const sym[] = {"=", "<>", ">", ">=", "<", "<="};
    char *lit = compare_literal(driver, value);
    out = str_printf("%s %s %s", col, sym[f->op], lit);
    free(lit);
    break;
  }
  case LACE_FILTER_IN: {
    char *list = in_list(driver, value);
    out = str_printf("%s IN (%s)", col, list ? list : "NULL");
    free(list);
    break;
  }
  case LACE_FILTER_CONTAINS:
  case LACE_FILTER_STARTS:
  case LACE_FILTER_ENDS: {
    const char *pre = f->op == LACE_FILTER_STARTS ? "" : "%";
    const char *suf = f->op == LACE_FILTER_ENDS ? "" : "%";
    char *lit = pattern_literal(driver, pre, value, suf);
    out = str_printf("%s LIKE %s", col, lit);
    free(lit);
    break;
  }
  case LACE_FILTER_REGEX: {
    char *lit;
    if (driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB) {
      lit = lace_sql_string_literal(driver, value);
      out = str_printf("%s REGEXP %s", col, lit);
    } else if (driver == LACE_DRIVER_POSTGRES) {
      lit = lace_sql_string_literal(driver, value);
      out = str_printf("%s ~ %s", col, lit);
    } else {
      lit = pattern_literal(driver, "*", value, "*");
      out = str_printf("%s GLOB %s", col, lit);
    }
    free(lit);
    break;
  }
  case LACE_FILTER_BETWEEN: {
    char *a = compare_literal(driver, value);
    char *b = compare_literal(driver, f->value2);
    out = str_printf("%s BETWEEN %s AND %s", col, a, b);
    free(a);
    free(b);
    break;
  }
  case LACE_FILTER_EMPTY:
    out = str_printf("%s = ''", col);
    break;
  case LACE_FILTER_NOT_EMPTY:
    out = str_printf("%s <> ''", col);
    break;
  case LACE_FILTER_NULL:
    out = str_printf("%s IS NULL", col);
    break;
  case LACE_FILTER_NOT_NULL:
    out = str_printf("%s IS NOT NULL", col);
    break;
  default:
    break;
  }
  free(col);
  return out;
}

/* (col1 LIKE '%x%' OR col2 LIKE '%x%') over the text columns */
static char *search_condition(const char *search, const LaceSchema *schema,
                              LaceDriver driver) {
  if (!search || !*search || !schema)
    return NULL;
  const char *like = driver == LACE_DRIVER_POSTGRES ? "ILIKE" : "LIKE";
  char *lit = pattern_literal(driver, "%", search, "%");
  StringBuilder *sb = sb_new(128);
  bool first = true;
  for (size_t i = 0; i < schema->num_columns; i++) {
    const LaceColumn *c = &schema->columns[i];
    if (!c->name || c->type != LACE_TYPE_TEXT)
      continue;
    char *col = lace_quote_identifier(driver, c->name);
    sb_printf(sb, "%s%s %s %s", first ? "(" : " OR ", col, like, lit);
    free(col);
    first = false;
  }
  free(lit);
  if (first) {
    sb_free(sb);
    return NULL;
  }
  sb_append(sb, ")");
  return sb_finish(sb);
}

char *lace_filter_build_where(const LaceFilter *filters, size_t num_filters,
                      const char *search, const LaceSchema *schema,
                      LaceDriver driver) {
  StringBuilder *sb = sb_new(256);
  bool first = true;
  for (size_t i = 0; filters && i < num_filters; i++) {
    char *cond = filter_condition(&filters[i], schema, driver);
    if (!cond)
      continue;
    sb_printf(sb, "%s%s", first ? "" : " AND ", cond);
    free(cond);
    first = false;
  }
  char *s = search_condition(search, schema, driver);
  if (s) {
    sb_printf(sb, "%s%s", first ? "" : " AND ", s);
    free(s);
    first = false;
  }
  if (first) {
    sb_free(sb);
    return NULL;
  }
  return sb_finish(sb);
}

char *lace_sort_build_order_by(const LaceSortSpec *sorts, size_t num_sorts,
                         LaceDriver driver) {
  StringBuilder *sb = sb_new(128);
  bool first = true;
  for (size_t i = 0; sorts && i < num_sorts; i++) {
    if (!sorts[i].column)
      continue;
    char *col = lace_quote_identifier(driver, sorts[i].column);
    sb_printf(sb, "%s%s %s", first ? "" : ", ", col,
              sorts[i].desc ? "DESC" : "ASC");
    free(col);
    first = false;
  }
  if (first) {
    sb_free(sb);
    return NULL;
  }
  return sb_finish(sb);
}

char *lace_sql_table_page(LaceDriver driver, const char *table, const char *where,
                     const char *order_by, size_t limit, size_t offset) {
  char *t = lace_quote_table(driver, table);
  if (!t)
    return NULL;
  StringBuilder *sb = sb_new(256);
  sb_printf(sb, "SELECT * FROM %s", t);
  if (where && *where)
    sb_printf(sb, " WHERE %s", where);
  if (order_by && *order_by)
    sb_printf(sb, " ORDER BY %s", order_by);
  sb_printf(sb, " LIMIT %zu OFFSET %zu", limit, offset);
  free(t);
  return sb_finish(sb);
}

char *lace_sql_table_count(LaceDriver driver, const char *table, const char *where) {
  char *t = lace_quote_table(driver, table);
  if (!t)
    return NULL;
  char *sql = (where && *where)
                  ? str_printf("SELECT COUNT(*) FROM %s WHERE %s", t, where)
                  : str_printf("SELECT COUNT(*) FROM %s", t);
  free(t);
  return sql;
}

/* ==========================================================================
 * Editable query results
 * ========================================================================== */

/* True for a plain SELECT reading one table; *from_out points just past the
 * top-level FROM */
static bool is_simple_select(const char *sql, LaceDriver driver,
                             const char **from_out) {
  static const char *const banned[] = {
      "JOIN",  "UNION",    "INTERSECT", "EXCEPT", "MINUS", "GROUP", "HAVING",
      "DISTINCT", "WINDOW", "OVER",     "LATERAL", "WITH",  NULL};
  static const char *const aggregates[] = {
      "COUNT",    "SUM",       "AVG",      "MIN",      "MAX",   "GROUP_CONCAT",
      "STRING_AGG", "ARRAY_AGG", "JSON_AGG", "JSONB_AGG", "JSON_GROUP_ARRAY",
      "TOTAL",    NULL};
  const char *p = sql;
  bool first_word = true;
  bool in_from = false;
  int depth = 0;
  *from_out = NULL;
  while (*p) {
    char c = *p;
    bool comment;
    size_t skip = lace_sql_skip(p, driver, &comment);
    if (skip) {
      p += skip;
      continue;
    }
    if (c == '(') {
      depth++;
      const char *n = p + 1;
      while (*n && isspace((unsigned char)*n))
        n++;
      if (strncasecmp(n, "SELECT", 6) == 0 && !isalnum((unsigned char)n[6]))
        return false;
      p++;
      continue;
    }
    if (c == ')') {
      depth--;
      p++;
      continue;
    }
    if (c == ',' && in_from && depth == 0)
      return false;
    if (c == ';')
      break;
    if (isalpha((unsigned char)c) || c == '_') {
      const char *w = p;
      while (*p && (isalnum((unsigned char)*p) || *p == '_'))
        p++;
      size_t len = (size_t)(p - w);
      if (first_word) {
        if (!word_is(w, len, "SELECT"))
          return false;
        first_word = false;
        continue;
      }
      for (int i = 0; banned[i]; i++) {
        if (word_is(w, len, banned[i]))
          return false;
      }
      const char *n = p;
      while (*n && isspace((unsigned char)*n))
        n++;
      if (*n == '(') {
        for (int i = 0; aggregates[i]; i++) {
          if (word_is(w, len, aggregates[i]))
            return false;
        }
      }
      if (depth == 0) {
        if (word_is(w, len, "FROM")) {
          in_from = true;
          if (!*from_out)
            *from_out = p;
        } else if (word_is(w, len, "WHERE") || word_is(w, len, "ORDER") ||
                   word_is(w, len, "LIMIT") || word_is(w, len, "OFFSET")) {
          in_from = false;
        }
      }
      continue;
    }
    p++;
  }
  return !first_word;
}

/* One identifier at *p (quoted "", `` or [], or bare); bare PostgreSQL
 * identifiers fold to lower case like the server does */
static char *parse_identifier(const char **p, LaceDriver driver) {
  const char *s = *p;
  StringBuilder *sb = sb_new(32);
  if (*s == '"' || *s == '`' || *s == '[') {
    char close = *s == '[' ? ']' : *s;
    s++;
    for (;;) {
      if (!*s) {
        sb_free(sb);
        return NULL;
      }
      if (*s == close) {
        if (close == ']' || s[1] != close)
          break;
        s++;
      }
      sb_append_char(sb, *s++);
    }
    s++;
  } else {
    while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '$' ||
                  (unsigned char)*s >= 0x80)) {
      char c = *s++;
      if (driver == LACE_DRIVER_POSTGRES && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
      sb_append_char(sb, c);
    }
  }
  if (sb->len == 0) {
    sb_free(sb);
    return NULL;
  }
  *p = s;
  return sb_finish(sb);
}

char *lace_sql_simple_select_table(const char *sql, LaceDriver driver) {
  if (!sql)
    return NULL;
  const char *from = NULL;
  if (!is_simple_select(sql, driver, &from) || !from)
    return NULL;
  while (*from && isspace((unsigned char)*from))
    from++;
  if (!*from)
    return NULL;

  char *parts[2] = {NULL, NULL};
  size_t num_parts = 0;
  const char *q = from;
  for (;;) {
    if (num_parts == 2) {
      free(parts[0]);
      free(parts[1]);
      return NULL;
    }
    char *part = parse_identifier(&q, driver);
    if (!part) {
      free(parts[0]);
      return NULL;
    }
    parts[num_parts++] = part;
    const char *after = q;
    while (*after && isspace((unsigned char)*after))
      after++;
    if (*after != '.')
      break;
    q = after + 1;
    while (*q && isspace((unsigned char)*q))
      q++;
  }
  while (*q && isspace((unsigned char)*q))
    q++;
  if (*q == ',') {
    free(parts[0]);
    free(parts[1]);
    return NULL;
  }
  char *table = NULL;
  if (driver == LACE_DRIVER_POSTGRES) {
    table = num_parts == 2 ? lace_table_ref(parts[0], parts[1])
                           : lace_table_ref(NULL, parts[0]);
  } else if (num_parts == 1) {
    table = str_dup(parts[0]);
  } else if (driver == LACE_DRIVER_SQLITE && str_eq_nocase(parts[0], "main")) {
    table = str_dup(parts[1]);
  }
  free(parts[0]);
  free(parts[1]);
  return table;
}
