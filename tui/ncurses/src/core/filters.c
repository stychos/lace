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

#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
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
    [FILTER_OP_BETWEEN] = {"between", NULL, true},
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
  FREE_NULL(f->filters);
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
    f->filters = safe_reallocarray(f->filters, new_cap, sizeof(ColumnFilter));
    f->filters_cap = new_cap;
  }

  ColumnFilter *cf = &f->filters[f->num_filters];
  cf->column_index = col_idx;
  cf->op = op;
  if (value) {
    size_t len = strlen(value);
    if (len >= sizeof(cf->value)) {
      /* Value too long - reject instead of silent truncation */
      return false;
    }
    memcpy(cf->value, value, len + 1);
  } else {
    cf->value[0] = '\0';
  }
  cf->value2[0] = '\0'; /* Initialize second value for BETWEEN */

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

/* Map a driver name to the dialect liblace builds literals for */
static LaceDriver filter_driver(const char *driver_name) {
  if (str_eq(driver_name, "mysql"))
    return LACE_DRIVER_MYSQL;
  if (str_eq(driver_name, "mariadb"))
    return LACE_DRIVER_MARIADB;
  if (str_eq(driver_name, "postgres") || str_eq(driver_name, "postgresql") ||
      str_eq(driver_name, "pg"))
    return LACE_DRIVER_POSTGRES;
  return LACE_DRIVER_SQLITE;
}

/* A string literal for the value, quotes included (caller frees). Quoting is
 * left to liblace: MySQL needs more than doubled quotes (a backslash escapes
 * unless NO_BACKSLASH_ESCAPES is set). */
static char *sql_literal(LaceDriver driver, const char *value) {
  return lace_sql_string_literal(driver, value ? value : "");
}

/* A literal for prefix + value + suffix, e.g. a LIKE or GLOB pattern */
static char *sql_pattern_literal(LaceDriver driver, const char *prefix,
                                 const char *value, const char *suffix) {
  char *pattern = str_printf("%s%s%s", prefix, value ? value : "", suffix);
  if (!pattern)
    return NULL;
  char *lit = sql_literal(driver, pattern);
  free(pattern);
  return lit;
}

/* True when [start, end) is a plain decimal number that can be inlined */
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

char *filters_parse_in_values(const char *input, const char *driver_name,
                              char **err) {
  if (!input || !*input) {
    if (err)
      *err = str_dup("Empty value list");
    return NULL;
  }

  LaceDriver driver = filter_driver(driver_name);
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

  /* Limit number of values to prevent DoS (MAX_IN_VALUES from constants.h) */
  size_t value_count = 0;

  bool first = true;
  bool ok = true; /* Track StringBuilder operation success */
  while (*p && ok) {
    /* Skip whitespace */
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p || *p == ')')
      break;

    /* Check value limit */
    if (value_count >= MAX_IN_VALUES) {
      sb_free(sb);
      if (err)
        *err = str_dup("Too many values in IN clause (max 1000)");
      return NULL;
    }
    value_count++;

    if (!first)
      ok = sb_append(sb, ", ");
    first = false;

    /* Numbers are inlined; everything else becomes a string literal */
    StringBuilder *val = sb_new(64);
    if (!val) {
      ok = false;
      break;
    }
    bool number = false;
    if (*p == '\'' || *p == '"') {
      /* Quoted value: a doubled quote or a backslash takes the next
       * character literally */
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
      /* Unquoted value - read until comma or end, trailing space trimmed */
      const char *start = p;
      while (*p && *p != ',' && *p != ')')
        p++;
      const char *end = p;
      while (end > start && isspace((unsigned char)*(end - 1)))
        end--;
      number = end > start && is_plain_number(start, end);
      sb_append_len(val, start, (size_t)(end - start));
    }
    if (!sb_ok(val)) {
      sb_free(val);
      ok = false;
      break;
    }
    char *text = sb_finish(val);
    char *lit = number ? text : sql_literal(driver, text);
    ok = ok && lit && sb_append(sb, lit);
    if (lit != text)
      free(lit);
    free(text);

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
  bool ok = true; /* Track StringBuilder operation success */
  LaceDriver driver = filter_driver(driver_name);

  /* Process each column filter */
  if (!f->filters && f->num_filters > 0) {
    sb_free(sb);
    if (err)
      *err = str_dup("Invalid filter state");
    return NULL;
  }
  for (size_t i = 0; i < f->num_filters && ok; i++) {
    ColumnFilter *cf = &f->filters[i];

    /* Skip filters with empty values if the operator requires a value.
     * Operators like IS NULL, IS NOT NULL, IS EMPTY, IS NOT EMPTY don't need
     * values. RAW filters also need a value (the SQL expression).
     * BETWEEN requires both values to be non-empty. */
    bool is_raw = (cf->column_index == SIZE_MAX);
    if (cf->value[0] == '\0' && (is_raw || filter_op_needs_value(cf->op))) {
      continue;
    }
    if (cf->op == FILTER_OP_BETWEEN && cf->value2[0] == '\0') {
      continue;
    }

    /* Validate column index */
    if (!is_raw && cf->column_index >= schema->num_columns)
      continue;

    if (!first)
      ok = sb_append(sb, " AND ");
    first = false;

    /* Handle RAW filters (virtual column) - advanced feature for SQL-savvy
     * users */
    if (is_raw) {
      ok = ok && sb_printf(sb, "(%s)", cf->value);
      continue;
    }

    const char *col_name = schema->columns[cf->column_index].name;

    char *escaped_col = lace_quote_identifier(driver, col_name);
    if (!escaped_col) {
      ok = false;
      continue;
    }

    switch (cf->op) {
    case FILTER_OP_EQ:
    case FILTER_OP_NE:
    case FILTER_OP_GT:
    case FILTER_OP_GE:
    case FILTER_OP_LT:
    case FILTER_OP_LE: {
      char *lit = sql_literal(driver, cf->value);
      ok = lit && sb_printf(sb, "%s %s %s", escaped_col, filter_op_sql(cf->op),
                            lit);
      free(lit);
      break;
    }

    case FILTER_OP_IN: {
      char *in_err = NULL;
      char *in_list = filters_parse_in_values(cf->value, driver_name, &in_err);
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

    case FILTER_OP_CONTAINS: {
      /* % and _ in the value stay wildcards */
      char *lit = sql_pattern_literal(driver, "%", cf->value, "%");
      ok = lit && sb_printf(sb, "%s LIKE %s", escaped_col, lit);
      free(lit);
      break;
    }

    case FILTER_OP_REGEX: {
      char *lit = NULL;
      if (driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB) {
        lit = sql_literal(driver, cf->value);
        ok = lit && sb_printf(sb, "%s REGEXP %s", escaped_col, lit);
      } else if (driver == LACE_DRIVER_POSTGRES) {
        lit = sql_literal(driver, cf->value);
        ok = lit && sb_printf(sb, "%s ~ %s", escaped_col, lit);
      } else {
        /* SQLite - use GLOB as fallback (not true regex) */
        lit = sql_pattern_literal(driver, "*", cf->value, "*");
        ok = lit && sb_printf(sb, "%s GLOB %s", escaped_col, lit);
      }
      free(lit);
      break;
    }

    case FILTER_OP_BETWEEN: {
      char *lit1 = sql_literal(driver, cf->value);
      char *lit2 = sql_literal(driver, cf->value2);
      ok = lit1 && lit2 &&
           sb_printf(sb, "%s BETWEEN %s AND %s", escaped_col, lit1, lit2);
      free(lit1);
      free(lit2);
      break;
    }

    case FILTER_OP_IS_EMPTY:
      ok = sb_printf(sb, "%s = ''", escaped_col);
      break;

    case FILTER_OP_IS_NOT_EMPTY:
      ok = sb_printf(sb, "%s <> ''", escaped_col);
      break;

    case FILTER_OP_IS_NULL:
      ok = sb_printf(sb, "%s IS NULL", escaped_col);
      break;

    case FILTER_OP_IS_NOT_NULL:
      ok = sb_printf(sb, "%s IS NOT NULL", escaped_col);
      break;

    case FILTER_OP_RAW:
      /* This case shouldn't occur - RAW is now a virtual column */
      ok = sb_printf(sb, "(%s)", cf->value);
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

/* ============================================================================
 * ORDER BY Building
 * ============================================================================
 */

char *filters_build_order_by(const SortEntry *entries, size_t num_entries,
                             const TableSchema *schema,
                             const char *driver_name) {
  if (!entries || num_entries == 0 || !schema)
    return NULL;

  LaceDriver driver = filter_driver(driver_name);
  StringBuilder *sb = sb_new(128);
  bool first = true;
  for (size_t i = 0; i < num_entries; i++) {
    /* Sort entries refer to columns by index: skip any the schema lacks */
    if (entries[i].column >= schema->num_columns ||
        !schema->columns[entries[i].column].name)
      continue;
    char *col = lace_quote_identifier(driver,
                                      schema->columns[entries[i].column].name);
    if (!col) {
      sb_free(sb);
      return NULL;
    }
    sb_printf(sb, "%s%s %s", first ? "" : ", ", col,
              entries[i].direction == SORT_ASC ? "ASC" : "DESC");
    free(col);
    first = false;
  }
  if (first) {
    sb_free(sb);
    return NULL;
  }
  return sb_finish(sb);
}

LaceKeysetState *tab_page_keyset(const Tab *tab, const char *driver_name,
                                 bool keyset_paging) {
  if (!tab || !tab->schema || !keyset_paging)
    return NULL;
  LaceSort sorts[MAX_SORT_COLUMNS];
  size_t n = 0;
  for (size_t i = 0; i < tab->num_sort_entries && n < MAX_SORT_COLUMNS; i++) {
    sorts[n].column = tab->sort_entries[i].column;
    sorts[n].dir = tab->sort_entries[i].direction == SORT_DESC ? LACE_SORT_DESC
                                                               : LACE_SORT_ASC;
    n++;
  }
  return lace_keyset_create(tab->schema, sorts, n, filter_driver(driver_name));
}

char *tab_page_order_by(const Tab *tab, const char *driver_name,
                        bool keyset_paging) {
  if (!tab)
    return NULL;
  LaceKeysetState *ks = tab_page_keyset(tab, driver_name, keyset_paging);
  if (ks) {
    char *order = lace_keyset_build_order(ks, true);
    lace_keyset_free(ks);
    return order;
  }
  return filters_build_order_by(tab->sort_entries, tab->num_sort_entries,
                                tab->schema, driver_name);
}
