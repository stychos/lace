/*
 * liblace - Lace Client Library
 * Keyset (cursor-based) pagination implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/keyset.h"
#include "../include/lace.h"
#include "../include/util/mem.h"
#include "../include/util/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Internal Structures
 * ========================================================================== */

struct LaceKeysetState {
  /* Keyset column indices (sort cols + PK cols in order) */
  size_t *columns;
  size_t num_columns;

  /* Sort directions for keyset columns (true = DESC, false = ASC) */
  bool *desc;

  /* Column names (copied from schema for SQL generation) */
  char **col_names;

  /* Primary key column indices within keyset columns */
  size_t pk_start;  /* Index where PK columns begin */
  size_t num_pk;    /* Number of PK columns */

  /* Boundary values for pagination */
  LaceValue *first_boundary;  /* Values from first row of current data */
  LaceValue *last_boundary;   /* Values from last row of current data */
  bool has_first;             /* first_boundary is valid */
  bool has_last;              /* last_boundary is valid */

  /* End-of-data flags */
  bool at_start;  /* No more rows before first_boundary */
  bool at_end;    /* No more rows after last_boundary */

  /* Database driver for SQL syntax */
  LaceDriver driver;
};

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Check if driver uses backticks for identifiers (MySQL/MariaDB) */
static bool use_backticks(LaceDriver driver) {
  return driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB;
}

/* Escape an identifier for SQL */
static char *escape_id(const char *name, LaceDriver driver) {
  return use_backticks(driver)
      ? str_escape_identifier_backtick(name)
      : str_escape_identifier_dquote(name);
}

/* Convert a value to SQL literal string */
static char *value_to_sql_literal(const LaceValue *val) {
  if (!val || val->is_null || val->type == LACE_TYPE_NULL) {
    return str_dup("NULL");
  }

  char buf[64];
  switch (val->type) {
  case LACE_TYPE_INT:
    snprintf(buf, sizeof(buf), "%lld", (long long)val->int_val);
    return str_dup(buf);

  case LACE_TYPE_FLOAT:
    snprintf(buf, sizeof(buf), "%g", val->float_val);
    return str_dup(buf);

  case LACE_TYPE_TEXT: {
    if (!val->text.data) return str_dup("NULL");
    /* Escape single quotes */
    size_t len = val->text.len;
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
      if (val->text.data[i] == '\'') extra++;
    }
    char *esc = safe_malloc(len + extra + 3);
    char *p = esc;
    *p++ = '\'';
    for (size_t i = 0; i < len; i++) {
      if (val->text.data[i] == '\'') *p++ = '\'';
      *p++ = val->text.data[i];
    }
    *p++ = '\'';
    *p = '\0';
    return esc;
  }

  case LACE_TYPE_BOOL:
    return str_dup(val->bool_val ? "TRUE" : "FALSE");

  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    /* Treat as text */
    if (val->text.data) {
      return str_printf("'%s'", val->text.data);
    }
    return str_dup("NULL");

  case LACE_TYPE_BLOB:
    /* Can't reliably represent blobs in keyset comparison */
    return str_dup("NULL");

  default:
    return str_dup("NULL");
  }
}

/* Free boundary values array */
static void free_boundaries(LaceValue *boundaries, size_t num) {
  if (!boundaries) return;
  for (size_t i = 0; i < num; i++) {
    lace_value_free(&boundaries[i]);
  }
  free(boundaries);
}

/* Copy a value */
static LaceValue copy_value(const LaceValue *src) {
  return lace_value_copy(src);
}

/* ==========================================================================
 * Capability Checking
 * ========================================================================== */

bool lace_keyset_can_use(const LaceSchema *schema) {
  if (!schema || !schema->columns || schema->num_columns == 0) {
    return false;
  }

  /* Check for at least one primary key column */
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (schema->columns[i].primary_key) {
      return true;
    }
  }

  return false;
}

bool lace_keyset_has_suitable_index(const LaceSchema *schema,
                                    const LaceSort *sorts, size_t num_sorts) {
  if (!schema) return false;

  /* No sort columns - keyset on PK is always efficient */
  if (!sorts || num_sorts == 0) {
    return lace_keyset_can_use(schema);
  }

  /* Check if there's an index that matches the sort columns */
  if (!schema->indexes || schema->num_indexes == 0) {
    /* No indexes available - can't use keyset efficiently with custom sort */
    return false;
  }

  /* Build list of sort column names */
  const char **sort_cols = safe_calloc(num_sorts, sizeof(char *));
  for (size_t i = 0; i < num_sorts; i++) {
    if (sorts[i].column < schema->num_columns) {
      sort_cols[i] = schema->columns[sorts[i].column].name;
    }
  }

  /* Check each index */
  bool found = false;
  for (size_t idx = 0; idx < schema->num_indexes && !found; idx++) {
    const LaceIndex *index = &schema->indexes[idx];
    if (!index->columns || index->num_columns < num_sorts) {
      continue;
    }

    /* Check if index columns match sort columns in order (prefix match OK) */
    bool matches = true;
    for (size_t i = 0; i < num_sorts && matches; i++) {
      if (!sort_cols[i] || !index->columns[i] ||
          strcmp(sort_cols[i], index->columns[i]) != 0) {
        matches = false;
      }
    }

    if (matches) {
      found = true;
    }
  }

  free(sort_cols);
  return found;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

LaceKeysetState *lace_keyset_create(const LaceSchema *schema,
                                    const LaceSort *sorts, size_t num_sorts,
                                    LaceDriver driver) {
  if (!lace_keyset_can_use(schema)) {
    return NULL;
  }

  /* Count PK columns */
  size_t num_pk = 0;
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (schema->columns[i].primary_key) {
      num_pk++;
    }
  }

  if (num_pk == 0) {
    return NULL;
  }

  /* Total keyset columns = sort columns + PK columns */
  size_t total_cols = num_sorts + num_pk;

  LaceKeysetState *ks = safe_calloc(1, sizeof(LaceKeysetState));
  ks->columns = safe_calloc(total_cols, sizeof(size_t));
  ks->desc = safe_calloc(total_cols, sizeof(bool));
  ks->col_names = safe_calloc(total_cols, sizeof(char *));
  ks->num_columns = total_cols;
  ks->pk_start = num_sorts;
  ks->num_pk = num_pk;
  ks->driver = driver;

  /* Add sort columns first */
  for (size_t i = 0; i < num_sorts; i++) {
    size_t col_idx = sorts[i].column;
    if (col_idx < schema->num_columns) {
      ks->columns[i] = col_idx;
      ks->desc[i] = (sorts[i].dir == LACE_SORT_DESC);
      ks->col_names[i] = str_dup(schema->columns[col_idx].name);
    }
  }

  /* Add PK columns (skip if already in sort columns) */
  size_t pk_pos = num_sorts;
  for (size_t i = 0; i < schema->num_columns && pk_pos < total_cols; i++) {
    if (!schema->columns[i].primary_key) {
      continue;
    }

    /* Check if this PK column is already in sort columns */
    bool already_in_sort = false;
    for (size_t j = 0; j < num_sorts; j++) {
      if (ks->columns[j] == i) {
        already_in_sort = true;
        break;
      }
    }

    if (!already_in_sort) {
      ks->columns[pk_pos] = i;
      ks->desc[pk_pos] = false;  /* PK tiebreaker is ASC by default */
      ks->col_names[pk_pos] = str_dup(schema->columns[i].name);
      pk_pos++;
    }
  }

  /* Adjust actual column count if some PKs were already in sort */
  ks->num_columns = pk_pos;
  ks->num_pk = pk_pos - num_sorts;

  return ks;
}

void lace_keyset_free(LaceKeysetState *ks) {
  if (!ks) return;

  free(ks->columns);
  free(ks->desc);

  if (ks->col_names) {
    for (size_t i = 0; i < ks->num_columns; i++) {
      free(ks->col_names[i]);
    }
    free(ks->col_names);
  }

  free_boundaries(ks->first_boundary, ks->num_columns);
  free_boundaries(ks->last_boundary, ks->num_columns);

  free(ks);
}

/* ==========================================================================
 * Boundary Management
 * ========================================================================== */

void lace_keyset_extract_boundaries(LaceKeysetState *ks, const LaceResult *data) {
  if (!ks || !data || data->num_rows == 0 || !data->rows) {
    return;
  }

  /* Free old boundaries */
  free_boundaries(ks->first_boundary, ks->num_columns);
  free_boundaries(ks->last_boundary, ks->num_columns);
  ks->first_boundary = NULL;
  ks->last_boundary = NULL;
  ks->has_first = false;
  ks->has_last = false;

  /* Allocate new boundaries */
  ks->first_boundary = safe_calloc(ks->num_columns, sizeof(LaceValue));
  ks->last_boundary = safe_calloc(ks->num_columns, sizeof(LaceValue));

  /* Extract from first row */
  const LaceRow *first_row = &data->rows[0];
  if (first_row->cells && first_row->num_cells > 0) {
    for (size_t i = 0; i < ks->num_columns; i++) {
      size_t col_idx = ks->columns[i];
      if (col_idx < first_row->num_cells) {
        ks->first_boundary[i] = copy_value(&first_row->cells[col_idx]);
      } else {
        ks->first_boundary[i] = lace_value_null();
      }
    }
    ks->has_first = true;
  }

  /* Extract from last row */
  const LaceRow *last_row = &data->rows[data->num_rows - 1];
  if (last_row->cells && last_row->num_cells > 0) {
    for (size_t i = 0; i < ks->num_columns; i++) {
      size_t col_idx = ks->columns[i];
      if (col_idx < last_row->num_cells) {
        ks->last_boundary[i] = copy_value(&last_row->cells[col_idx]);
      } else {
        ks->last_boundary[i] = lace_value_null();
      }
    }
    ks->has_last = true;
  }
}

void lace_keyset_clear_boundaries(LaceKeysetState *ks) {
  if (!ks) return;

  free_boundaries(ks->first_boundary, ks->num_columns);
  free_boundaries(ks->last_boundary, ks->num_columns);
  ks->first_boundary = NULL;
  ks->last_boundary = NULL;
  ks->has_first = false;
  ks->has_last = false;
  ks->at_start = false;
  ks->at_end = false;
}

bool lace_keyset_at_start(const LaceKeysetState *ks) {
  return ks ? ks->at_start : true;
}

bool lace_keyset_at_end(const LaceKeysetState *ks) {
  return ks ? ks->at_end : true;
}

void lace_keyset_set_at_start(LaceKeysetState *ks, bool at_start) {
  if (ks) ks->at_start = at_start;
}

void lace_keyset_set_at_end(LaceKeysetState *ks, bool at_end) {
  if (ks) ks->at_end = at_end;
}

bool lace_keyset_has_last_boundary(const LaceKeysetState *ks) {
  return ks && ks->has_last;
}

bool lace_keyset_has_first_boundary(const LaceKeysetState *ks) {
  return ks && ks->has_first;
}

/* ==========================================================================
 * SQL Generation
 * ========================================================================== */

/*
 * Build WHERE clause for keyset pagination.
 *
 * For a single column PK with forward pagination:
 *   WHERE pk > last_pk
 *
 * For composite key (col1, col2, pk) with forward pagination:
 *   For databases supporting row comparison (PostgreSQL):
 *     WHERE (col1, col2, pk) > (v1, v2, v3)
 *
 *   For MySQL/SQLite (expanded form):
 *     WHERE col1 > v1
 *        OR (col1 = v1 AND col2 > v2)
 *        OR (col1 = v1 AND col2 = v2 AND pk > v3)
 *
 * With sort directions (e.g., col1 DESC, col2 ASC):
 *   Forward: col1 < v1 OR (col1 = v1 AND col2 > v2) OR ...
 *   Backward: Flip all comparisons
 */
char *lace_keyset_build_where(const LaceKeysetState *ks, bool forward) {
  if (!ks || ks->num_columns == 0) return NULL;

  const LaceValue *boundary = forward ? ks->last_boundary : ks->first_boundary;
  bool has_boundary = forward ? ks->has_last : ks->has_first;

  if (!has_boundary || !boundary) {
    return NULL;  /* No boundary = first page, no keyset WHERE needed */
  }

  StringBuilder *sb = sb_new(256);
  if (!sb) return NULL;

  /* For simplicity, always use expanded form (works with all databases) */
  sb_append(sb, "(");

  for (size_t i = 0; i < ks->num_columns; i++) {
    if (i > 0) {
      sb_append(sb, " OR (");
    }

    /* Build equality conditions for all previous columns */
    for (size_t j = 0; j < i; j++) {
      char *col_esc = escape_id(ks->col_names[j], ks->driver);
      char *val_sql = value_to_sql_literal(&boundary[j]);

      /* Handle NULL comparisons */
      if (boundary[j].is_null || boundary[j].type == LACE_TYPE_NULL) {
        sb_printf(sb, "%s IS NULL AND ", col_esc);
      } else {
        sb_printf(sb, "%s = %s AND ", col_esc, val_sql);
      }

      free(col_esc);
      free(val_sql);
    }

    /* Build comparison for current column */
    char *col_esc = escape_id(ks->col_names[i], ks->driver);
    char *val_sql = value_to_sql_literal(&boundary[i]);

    /* Determine comparison operator based on direction and forward/backward */
    bool col_desc = ks->desc[i];
    const char *op;
    if (forward) {
      /* Forward: > for ASC columns, < for DESC columns */
      op = col_desc ? "<" : ">";
    } else {
      /* Backward: < for ASC columns, > for DESC columns */
      op = col_desc ? ">" : "<";
    }

    /* Skip NULL values in comparison (NULL doesn't compare) */
    if (boundary[i].is_null || boundary[i].type == LACE_TYPE_NULL) {
      /* For NULL, we need IS NOT NULL to get next values */
      sb_printf(sb, "%s IS NOT NULL", col_esc);
    } else {
      sb_printf(sb, "%s %s %s", col_esc, op, val_sql);
    }

    free(col_esc);
    free(val_sql);

    if (i > 0) {
      sb_append(sb, ")");
    }
  }

  sb_append(sb, ")");

  return sb_finish(sb);
}

char *lace_keyset_build_order(const LaceKeysetState *ks, bool forward) {
  if (!ks || ks->num_columns == 0) return NULL;

  StringBuilder *sb = sb_new(128);
  if (!sb) return NULL;

  for (size_t i = 0; i < ks->num_columns; i++) {
    if (i > 0) {
      sb_append(sb, ", ");
    }

    char *col_esc = escape_id(ks->col_names[i], ks->driver);

    /* Determine direction */
    bool col_desc = ks->desc[i];
    const char *dir;
    if (forward) {
      dir = col_desc ? "DESC" : "ASC";
    } else {
      /* Backward: flip directions */
      dir = col_desc ? "ASC" : "DESC";
    }

    sb_printf(sb, "%s %s", col_esc, dir);
    free(col_esc);
  }

  return sb_finish(sb);
}

/* ==========================================================================
 * Query Execution
 * ========================================================================== */

int lace_query_keyset(lace_client_t *client, int conn_id,
                      const char *table,
                      LaceKeysetState *ks, bool forward,
                      const char *user_where,
                      size_t limit,
                      LaceResult **result) {
  if (!client || !table || !ks || !result) {
    return LACE_ERR_INVALID_PARAMS;
  }

  *result = NULL;

  /* Build keyset WHERE clause */
  char *keyset_where = lace_keyset_build_where(ks, forward);

  /* Build ORDER BY clause */
  char *order_by = lace_keyset_build_order(ks, forward);
  if (!order_by) {
    free(keyset_where);
    return LACE_ERR_INTERNAL_ERROR;
  }

  /* Escape table name */
  char *table_esc = escape_id(table, ks->driver);
  if (!table_esc) {
    free(keyset_where);
    free(order_by);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  /* Build full SQL query */
  StringBuilder *sb = sb_new(512);
  if (!sb) {
    free(table_esc);
    free(keyset_where);
    free(order_by);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  sb_printf(sb, "SELECT * FROM %s", table_esc);

  /* Combine WHERE clauses */
  if (keyset_where && user_where) {
    sb_printf(sb, " WHERE (%s) AND (%s)", keyset_where, user_where);
  } else if (keyset_where) {
    sb_printf(sb, " WHERE %s", keyset_where);
  } else if (user_where) {
    sb_printf(sb, " WHERE %s", user_where);
  }

  sb_printf(sb, " ORDER BY %s LIMIT %zu", order_by, limit);

  char *sql = sb_finish(sb);

  free(table_esc);
  free(keyset_where);
  free(order_by);

  if (!sql) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  /* Execute query */
  int err = lace_exec(client, conn_id, sql, result);
  free(sql);

  if (err != LACE_OK) {
    return err;
  }

  /* For backward queries, reverse the result rows */
  if (!forward && *result && (*result)->num_rows > 1) {
    LaceResult *res = *result;
    size_t n = res->num_rows;

    /* Swap rows in place */
    for (size_t i = 0; i < n / 2; i++) {
      LaceRow tmp = res->rows[i];
      res->rows[i] = res->rows[n - 1 - i];
      res->rows[n - 1 - i] = tmp;
    }
  }

  /* Update at_end/at_start flags based on result */
  if (*result) {
    LaceResult *res = *result;
    if (res->num_rows < limit) {
      if (forward) {
        ks->at_end = true;
      } else {
        ks->at_start = true;
      }
    }
  }

  return LACE_OK;
}

/* ==========================================================================
 * Utility
 * ========================================================================== */

size_t lace_keyset_num_columns(const LaceKeysetState *ks) {
  return ks ? ks->num_columns : 0;
}

size_t lace_keyset_column_index(const LaceKeysetState *ks, size_t idx) {
  if (!ks || idx >= ks->num_columns) {
    return SIZE_MAX;
  }
  return ks->columns[idx];
}
