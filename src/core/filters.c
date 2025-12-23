/*
 * Lace
 * Core Filter Logic (platform-independent)
 *
 * This module implements filter operations and SQL WHERE clause building.
 * All functions here are UI-agnostic.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../util/str.h"
#include "app_state.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Operator Definitions
 * ============================================================================
 */

typedef struct {
  const char *display_name; /* Name shown in UI */
  const char *sql_symbol;   /* SQL operator (NULL if complex/special) */
  bool needs_value;         /* Whether operator requires a value */
} FilterOpDef;

static const FilterOpDef FILTER_OPS[] = {
    [FILTER_OP_EQ] = {"=", "=", true},
    [FILTER_OP_NE] = {"<>", "<>", true},
    [FILTER_OP_GT] = {">", ">", true},
    [FILTER_OP_GE] = {">=", ">=", true},
    [FILTER_OP_LT] = {"<", "<", true},
    [FILTER_OP_LE] = {"<=", "<=", true},
    [FILTER_OP_IN] = {"in", NULL, true},
    [FILTER_OP_CONTAINS] = {"contains", NULL, true},
    [FILTER_OP_REGEX] = {"regex", NULL, true},
    [FILTER_OP_IS_EMPTY] = {"is empty", NULL, false},
    [FILTER_OP_IS_NOT_EMPTY] = {"is not empty", NULL, false},
    [FILTER_OP_IS_NULL] = {"is null", NULL, false},
    [FILTER_OP_IS_NOT_NULL] = {"is not null", NULL, false},
    [FILTER_OP_RAW] = {"RAW", NULL, true},
};

/* ============================================================================
 * Filter Structure Operations
 * ============================================================================
 */

void filters_init(TableFilters *f) {
  if (!f)
    return;
  memset(f, 0, sizeof(TableFilters));
}

void filters_free(TableFilters *f) {
  if (!f)
    return;
  free(f->filters);
  f->filters = NULL;
  f->num_filters = 0;
  f->filters_cap = 0;
}

void filters_clear(TableFilters *f) {
  if (!f)
    return;
  f->num_filters = 0;
}

bool filters_add(TableFilters *f, size_t col_idx, FilterOperator op,
                 const char *value) {
  if (!f)
    return false;

  /* Grow array if needed */
  if (f->num_filters >= f->filters_cap) {
    size_t new_cap = f->filters_cap == 0 ? 4 : f->filters_cap * 2;
    ColumnFilter *new_filters =
        realloc(f->filters, new_cap * sizeof(ColumnFilter));
    if (!new_filters)
      return false;
    f->filters = new_filters;
    f->filters_cap = new_cap;
  }

  ColumnFilter *cf = &f->filters[f->num_filters];
  cf->column_index = col_idx;
  cf->op = op;
  if (value) {
    strncpy(cf->value, value, sizeof(cf->value) - 1);
    cf->value[sizeof(cf->value) - 1] = '\0';
  } else {
    cf->value[0] = '\0';
  }

  f->num_filters++;
  return true;
}

void filters_remove(TableFilters *f, size_t index) {
  if (!f || index >= f->num_filters)
    return;

  /* Shift remaining filters down */
  for (size_t i = index; i < f->num_filters - 1; i++) {
    f->filters[i] = f->filters[i + 1];
  }
  f->num_filters--;
}

/* ============================================================================
 * Operator Info Functions
 * ============================================================================
 */

const char *filter_op_name(FilterOperator op) {
  if (op >= FILTER_OP_COUNT)
    return "?";
  return FILTER_OPS[op].display_name;
}

const char *filter_op_sql(FilterOperator op) {
  if (op >= FILTER_OP_COUNT || !FILTER_OPS[op].sql_symbol)
    return "=";
  return FILTER_OPS[op].sql_symbol;
}

bool filter_op_needs_value(FilterOperator op) {
  if (op >= FILTER_OP_COUNT)
    return true;
  return FILTER_OPS[op].needs_value;
}

/* ============================================================================
 * SQL Building Helpers
 * ============================================================================
 */

/* Escape a value for SQL */
static char *escape_sql_value(const char *value) {
  if (!value)
    return str_dup("");

  size_t len = strlen(value);
  StringBuilder *sb = sb_new(len * 2 + 2);
  if (!sb)
    return NULL;

  for (const char *p = value; *p; p++) {
    if (*p == '\'')
      sb_append(sb, "''");
    else
      sb_append_char(sb, *p);
  }

  return sb_to_string(sb);
}

char *filters_parse_in_values(const char *input, char **err) {
  if (!input || !*input) {
    if (err)
      *err = str_dup("Empty value list");
    return NULL;
  }

  StringBuilder *sb = sb_new(strlen(input) * 2);
  if (!sb) {
    if (err)
      *err = str_dup("Out of memory");
    return NULL;
  }

  const char *p = input;

  /* Skip leading whitespace and optional ( */
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '(')
    p++;

  bool first = true;
  while (*p) {
    /* Skip whitespace */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p || *p == ')')
      break;

    if (!first)
      sb_append(sb, ", ");
    first = false;

    if (*p == '\'' || *p == '"') {
      /* Quoted value - find closing quote */
      char quote = *p;
      p++;
      const char *start = p;
      while (*p && *p != quote) {
        if (*p == '\\' && *(p + 1))
          p++; /* Skip escaped chars */
        p++;
      }
      sb_append_char(sb, '\'');
      /* Copy and escape single quotes */
      for (const char *c = start; c < p; c++) {
        if (*c == '\'')
          sb_append(sb, "''");
        else
          sb_append_char(sb, *c);
      }
      sb_append_char(sb, '\'');
      if (*p == quote)
        p++;
    } else {
      /* Unquoted value - read until comma or end */
      const char *start = p;
      while (*p && *p != ',' && *p != ')')
        p++;
      /* Trim trailing whitespace */
      const char *end = p;
      while (end > start && isspace((unsigned char)*(end - 1)))
        end--;

      if (end > start) {
        /* Check if numeric */
        bool is_numeric = true;
        for (const char *c = start; c < end; c++) {
          if (!isdigit((unsigned char)*c) && *c != '.' && *c != '-' &&
              *c != '+') {
            is_numeric = false;
            break;
          }
        }

        if (is_numeric) {
          sb_append_len(sb, start, end - start);
        } else {
          sb_append_char(sb, '\'');
          /* Escape single quotes in value */
          for (const char *c = start; c < end; c++) {
            if (*c == '\'')
              sb_append(sb, "''");
            else
              sb_append_char(sb, *c);
          }
          sb_append_char(sb, '\'');
        }
      }
    }

    /* Skip comma */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == ',')
      p++;
  }

  return sb_to_string(sb);
}

/* ============================================================================
 * WHERE Clause Building
 * ============================================================================
 */

char *filters_build_where(TableFilters *f, TableSchema *schema,
                          const char *driver_name, char **err) {
  if (!f || !schema)
    return NULL;

  /* No filters? Return NULL (no WHERE clause) */
  if (f->num_filters == 0)
    return NULL;

  StringBuilder *sb = sb_new(256);
  if (!sb) {
    if (err)
      *err = str_dup("Out of memory");
    return NULL;
  }

  bool first = true;
  bool use_backticks =
      str_eq(driver_name, "mysql") || str_eq(driver_name, "mariadb");

  /* Process each column filter */
  for (size_t i = 0; i < f->num_filters; i++) {
    ColumnFilter *cf = &f->filters[i];

    /* Skip filters with empty values if the operator requires a value.
     * Operators like IS NULL, IS NOT NULL, IS EMPTY, IS NOT EMPTY don't need
     * values. RAW filters also need a value (the SQL expression). */
    bool is_raw = (cf->column_index == SIZE_MAX);
    if (cf->value[0] == '\0' && (is_raw || filter_op_needs_value(cf->op))) {
      continue;
    }

    if (!first)
      sb_append(sb, " AND ");
    first = false;

    /* Handle RAW filters (virtual column) */
    if (cf->column_index == SIZE_MAX) {
      sb_printf(sb, "(%s)", cf->value);
      continue;
    }

    /* Validate column index */
    if (cf->column_index >= schema->num_columns)
      continue;

    const char *col_name = schema->columns[cf->column_index].name;

    /* Escape column name */
    char *escaped_col;
    if (use_backticks) {
      escaped_col = str_escape_identifier_backtick(col_name);
    } else {
      escaped_col = str_escape_identifier_dquote(col_name);
    }
    if (!escaped_col)
      continue;

    switch (cf->op) {
    case FILTER_OP_EQ:
    case FILTER_OP_NE:
    case FILTER_OP_GT:
    case FILTER_OP_GE:
    case FILTER_OP_LT:
    case FILTER_OP_LE: {
      char *escaped_val = escape_sql_value(cf->value);
      sb_printf(sb, "%s %s '%s'", escaped_col, filter_op_sql(cf->op),
                escaped_val ? escaped_val : "");
      free(escaped_val);
      break;
    }

    case FILTER_OP_IN: {
      char *in_err = NULL;
      char *in_list = filters_parse_in_values(cf->value, &in_err);
      if (in_list) {
        sb_printf(sb, "%s IN (%s)", escaped_col, in_list);
        free(in_list);
      } else {
        /* Fall back to empty IN */
        sb_printf(sb, "%s IN (NULL)", escaped_col);
        free(in_err);
      }
      break;
    }

    case FILTER_OP_CONTAINS: {
      char *escaped_val = escape_sql_value(cf->value);
      /* Escape LIKE wildcards */
      sb_printf(sb, "%s LIKE '%%%s%%'", escaped_col,
                escaped_val ? escaped_val : "");
      free(escaped_val);
      break;
    }

    case FILTER_OP_REGEX: {
      char *escaped_val = escape_sql_value(cf->value);
      /* Driver-specific regex */
      if (str_eq(driver_name, "mysql") || str_eq(driver_name, "mariadb")) {
        sb_printf(sb, "%s REGEXP '%s'", escaped_col,
                  escaped_val ? escaped_val : "");
      } else if (str_eq(driver_name, "postgres") ||
                 str_eq(driver_name, "postgresql") ||
                 str_eq(driver_name, "pg")) {
        sb_printf(sb, "%s ~ '%s'", escaped_col, escaped_val ? escaped_val : "");
      } else {
        /* SQLite - use GLOB as fallback (not true regex) */
        sb_printf(sb, "%s GLOB '*%s*'", escaped_col,
                  escaped_val ? escaped_val : "");
      }
      free(escaped_val);
      break;
    }

    case FILTER_OP_IS_EMPTY:
      sb_printf(sb, "%s = ''", escaped_col);
      break;

    case FILTER_OP_IS_NOT_EMPTY:
      sb_printf(sb, "%s <> ''", escaped_col);
      break;

    case FILTER_OP_IS_NULL:
      sb_printf(sb, "%s IS NULL", escaped_col);
      break;

    case FILTER_OP_IS_NOT_NULL:
      sb_printf(sb, "%s IS NOT NULL", escaped_col);
      break;

    case FILTER_OP_RAW:
      /* This case shouldn't occur - RAW is now a virtual column */
      sb_printf(sb, "(%s)", cf->value);
      break;

    default:
      break;
    }

    free(escaped_col);
  }

  /* If all filters were skipped, return NULL */
  if (first) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}
