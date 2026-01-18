/*
 * liblace - Lace Client Library
 * Filter collection and WHERE clause building implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/filter.h"
#include "../include/util/mem.h"
#include "../include/util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Filter Collection Management
 * ========================================================================== */

void lace_filters_init(LaceFilters *f) {
  if (!f)
    return;
  memset(f, 0, sizeof(LaceFilters));
}

void lace_filters_free(LaceFilters *f) {
  if (!f)
    return;

  if (f->filters) {
    for (size_t i = 0; i < f->num_filters; i++) {
      lace_filter_free(&f->filters[i]);
    }
    free(f->filters);
  }

  f->filters = NULL;
  f->num_filters = 0;
  f->capacity = 0;
}

void lace_filters_clear(LaceFilters *f) {
  if (!f)
    return;

  /* Free individual filter data but keep the array */
  for (size_t i = 0; i < f->num_filters; i++) {
    lace_filter_free(&f->filters[i]);
  }
  f->num_filters = 0;
}

bool lace_filters_add(LaceFilters *f, size_t column, LaceFilterOp op,
                      const char *value, const char *value2) {
  if (!f)
    return false;

  /* Grow array if needed */
  if (f->num_filters >= f->capacity) {
    size_t new_cap = f->capacity == 0 ? 4 : f->capacity * 2;
    LaceFilter *new_filters =
        safe_reallocarray(f->filters, new_cap, sizeof(LaceFilter));
    if (!new_filters)
      return false;
    f->filters = new_filters;
    f->capacity = new_cap;
  }

  LaceFilter *filter = &f->filters[f->num_filters];
  memset(filter, 0, sizeof(LaceFilter));

  filter->column = column;
  filter->op = op;

  if (value) {
    filter->value = str_dup(value);
    if (!filter->value)
      return false;
  }

  if (value2) {
    filter->value2 = str_dup(value2);
    if (!filter->value2) {
      free(filter->value);
      filter->value = NULL;
      return false;
    }
  }

  f->num_filters++;
  return true;
}

void lace_filters_remove(LaceFilters *f, size_t index) {
  if (!f || index >= f->num_filters)
    return;

  /* Free the removed filter's data */
  lace_filter_free(&f->filters[index]);

  /* Shift remaining filters down */
  for (size_t i = index; i < f->num_filters - 1; i++) {
    f->filters[i] = f->filters[i + 1];
  }

  /* Clear the last slot */
  memset(&f->filters[f->num_filters - 1], 0, sizeof(LaceFilter));
  f->num_filters--;
}

size_t lace_filters_count_active(const LaceFilters *f) {
  if (!f || !f->filters)
    return 0;

  size_t count = 0;
  for (size_t i = 0; i < f->num_filters; i++) {
    const LaceFilter *filter = &f->filters[i];
    bool is_raw = (filter->column == LACE_FILTER_COL_RAW);

    /* Skip filters without values if they need one */
    if ((!filter->value || filter->value[0] == '\0') &&
        (is_raw || lace_filter_op_needs_value(filter->op))) {
      continue;
    }

    /* BETWEEN requires both values */
    if (filter->op == LACE_FILTER_BETWEEN &&
        (!filter->value2 || filter->value2[0] == '\0')) {
      continue;
    }

    count++;
  }

  return count;
}

/* ==========================================================================
 * SQL Generation Helpers
 * ========================================================================== */

LaceSqlDriver lace_sql_driver_from_name(const char *name) {
  if (!name)
    return LACE_SQL_SQLITE;

  if (str_eq(name, "mysql"))
    return LACE_SQL_MYSQL;
  if (str_eq(name, "mariadb"))
    return LACE_SQL_MARIADB;
  if (str_eq(name, "postgres") || str_eq(name, "postgresql") ||
      str_eq(name, "pg"))
    return LACE_SQL_POSTGRES;

  return LACE_SQL_SQLITE;
}

/* Escape a value for SQL */
static char *escape_sql_value(const char *value) {
  if (!value)
    return str_dup("");

  size_t len = strlen(value);
  StringBuilder *sb = sb_new(len * 2 + 2);
  if (!sb)
    return NULL;

  bool failed = false;
  for (const char *p = value; *p; p++) {
    if (*p == '\'') {
      if (!sb_append(sb, "''")) {
        failed = true;
        break;
      }
    } else {
      if (!sb_append_char(sb, *p)) {
        failed = true;
        break;
      }
    }
  }

  if (failed) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}

char *lace_filters_parse_in_values(const char *input, char **err) {
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

  /* Limit number of values to prevent DoS */
  size_t value_count = 0;

  bool first = true;
  bool ok = true;
  while (*p && ok) {
    /* Skip whitespace */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p || *p == ')')
      break;

    /* Check value limit */
    if (value_count >= LACE_MAX_IN_VALUES) {
      sb_free(sb);
      if (err)
        *err = str_dup("Too many values in IN clause (max 1000)");
      return NULL;
    }
    value_count++;

    if (!first)
      ok = sb_append(sb, ", ");
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
      ok = ok && sb_append_char(sb, '\'');
      /* Copy and escape single quotes */
      for (const char *c = start; c < p && ok; c++) {
        if (*c == '\'')
          ok = sb_append(sb, "''");
        else
          ok = sb_append_char(sb, *c);
      }
      ok = ok && sb_append_char(sb, '\'');
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
          ok = sb_append_len(sb, start, (size_t)(end - start));
        } else {
          ok = sb_append_char(sb, '\'');
          /* Escape single quotes in value */
          for (const char *c = start; c < end && ok; c++) {
            if (*c == '\'')
              ok = sb_append(sb, "''");
            else
              ok = sb_append_char(sb, *c);
          }
          ok = ok && sb_append_char(sb, '\'');
        }
      }
    }

    /* Skip comma */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == ',')
      p++;
  }

  if (!ok) {
    sb_free(sb);
    if (err)
      *err = str_dup("Out of memory");
    return NULL;
  }

  return sb_to_string(sb);
}

/* ==========================================================================
 * WHERE Clause Building
 * ========================================================================== */

char *lace_filters_build_where(const LaceFilters *f, const LaceSchema *schema,
                               LaceSqlDriver driver, char **err) {
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
  bool ok = true;
  bool use_backticks = (driver == LACE_SQL_MYSQL || driver == LACE_SQL_MARIADB);

  /* Process each filter */
  for (size_t i = 0; i < f->num_filters && ok; i++) {
    const LaceFilter *filter = &f->filters[i];

    /* Skip filters with empty values if the operator requires a value.
     * Operators like IS NULL don't need values.
     * RAW filters also need a value (the SQL expression).
     * BETWEEN requires both values to be non-empty. */
    bool is_raw = (filter->column == LACE_FILTER_COL_RAW);
    bool has_value = filter->value && filter->value[0] != '\0';
    bool has_value2 = filter->value2 && filter->value2[0] != '\0';

    if (!has_value && (is_raw || lace_filter_op_needs_value(filter->op))) {
      continue;
    }
    if (filter->op == LACE_FILTER_BETWEEN && !has_value2) {
      continue;
    }

    if (!first)
      ok = sb_append(sb, " AND ");
    first = false;

    /* Handle RAW filters (virtual column) */
    if (is_raw) {
      ok = ok && sb_printf(sb, "(%s)", filter->value);
      continue;
    }

    /* Validate column index */
    if (filter->column >= schema->num_columns)
      continue;

    const char *col_name = schema->columns[filter->column].name;

    /* Escape column name */
    char *escaped_col;
    if (use_backticks) {
      escaped_col = str_escape_identifier_backtick(col_name);
    } else {
      escaped_col = str_escape_identifier_dquote(col_name);
    }
    if (!escaped_col)
      continue;

    switch (filter->op) {
    case LACE_FILTER_EQ:
    case LACE_FILTER_NE:
    case LACE_FILTER_GT:
    case LACE_FILTER_GE:
    case LACE_FILTER_LT:
    case LACE_FILTER_LE: {
      char *escaped_val = escape_sql_value(filter->value);
      ok = sb_printf(sb, "%s %s '%s'", escaped_col,
                     lace_filter_op_sql(filter->op),
                     escaped_val ? escaped_val : "");
      free(escaped_val);
      break;
    }

    case LACE_FILTER_IN: {
      char *in_err = NULL;
      char *in_list = lace_filters_parse_in_values(filter->value, &in_err);
      if (in_list) {
        ok = sb_printf(sb, "%s IN (%s)", escaped_col, in_list);
        free(in_list);
      } else {
        /* Fall back to empty IN */
        ok = sb_printf(sb, "%s IN (NULL)", escaped_col);
        free(in_err);
      }
      break;
    }

    case LACE_FILTER_CONTAINS: {
      char *escaped_val = escape_sql_value(filter->value);
      ok = sb_printf(sb, "%s LIKE '%%%s%%'", escaped_col,
                     escaped_val ? escaped_val : "");
      free(escaped_val);
      break;
    }

    case LACE_FILTER_REGEX: {
      char *escaped_val = escape_sql_value(filter->value);
      /* Driver-specific regex */
      if (driver == LACE_SQL_MYSQL || driver == LACE_SQL_MARIADB) {
        ok = sb_printf(sb, "%s REGEXP '%s'", escaped_col,
                       escaped_val ? escaped_val : "");
      } else if (driver == LACE_SQL_POSTGRES) {
        ok = sb_printf(sb, "%s ~ '%s'", escaped_col,
                       escaped_val ? escaped_val : "");
      } else {
        /* SQLite - use GLOB as fallback (not true regex) */
        ok = sb_printf(sb, "%s GLOB '*%s*'", escaped_col,
                       escaped_val ? escaped_val : "");
      }
      free(escaped_val);
      break;
    }

    case LACE_FILTER_BETWEEN: {
      char *escaped_val1 = escape_sql_value(filter->value);
      char *escaped_val2 = escape_sql_value(filter->value2);
      ok = sb_printf(sb, "%s BETWEEN '%s' AND '%s'", escaped_col,
                     escaped_val1 ? escaped_val1 : "",
                     escaped_val2 ? escaped_val2 : "");
      free(escaped_val1);
      free(escaped_val2);
      break;
    }

    case LACE_FILTER_IS_EMPTY:
      ok = sb_printf(sb, "%s = ''", escaped_col);
      break;

    case LACE_FILTER_IS_NOT_EMPTY:
      ok = sb_printf(sb, "%s <> ''", escaped_col);
      break;

    case LACE_FILTER_IS_NULL:
      ok = sb_printf(sb, "%s IS NULL", escaped_col);
      break;

    case LACE_FILTER_IS_NOT_NULL:
      ok = sb_printf(sb, "%s IS NOT NULL", escaped_col);
      break;

    case LACE_FILTER_RAW:
      /* This case shouldn't occur - RAW is now a virtual column */
      ok = sb_printf(sb, "(%s)", filter->value);
      break;

    default:
      break;
    }

    free(escaped_col);
  }

  /* Check for allocation failure */
  if (!ok) {
    sb_free(sb);
    if (err)
      *err = str_dup("Out of memory");
    return NULL;
  }

  /* If all filters were skipped, return NULL */
  if (first) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}
