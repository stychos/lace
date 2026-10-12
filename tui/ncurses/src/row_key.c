/*
 * Lace
 * Row keys for UPDATE / DELETE
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "row_key.h"
#include "core/constants.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Why a key value cannot address its row (NULL if it can): a NULL part
 * matches no row with "=" and every NULL row with "IS NULL" (SQLite allows
 * several NULLs in a non-INTEGER primary key); NaN/Inf have no exact
 * literal on most databases. */
static const char *key_value_problem(const DbValue *v) {
  if (v->is_null || v->type == DB_TYPE_NULL)
    return "its primary key contains NULL";
  if (v->type == DB_TYPE_FLOAT && !isfinite(v->float_val))
    return "its primary key is NaN or infinite";
  return NULL;
}

/* Index of the column named name in data; SIZE_MAX if missing, or (with
 * unique) if it appears more than once */
static size_t find_column(const ResultSet *data, const char *name,
                          bool unique) {
  size_t found = SIZE_MAX, hits = 0;
  for (size_t j = 0; j < data->num_columns; j++) {
    if (data->columns[j].name && strcmp(data->columns[j].name, name) == 0) {
      if (found == SIZE_MAX)
        found = j;
      hits++;
    }
  }
  return unique && hits > 1 ? SIZE_MAX : found;
}

RowKeyStatus row_key_build(RowKey *key, const ResultSet *data, size_t row,
                           const TableSchema *schema, bool unique_names,
                           const char **why) {
  memset(key, 0, sizeof(*key));
  *why = "no primary key found";
  if (!data || !schema || !data->rows || !data->columns ||
      row >= data->num_rows || !data->rows[row].cells)
    return ROW_KEY_NONE;
  const Row *r = &data->rows[row];

  size_t idx[LACE_MAX_PK_COLUMNS];
  size_t n = 0;
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (!schema->columns[i].primary_key)
      continue;
    const char *name = schema->columns[i].name;
    if (!name || n >= LACE_MAX_PK_COLUMNS)
      return ROW_KEY_NONE;
    size_t j = find_column(data, name, unique_names);
    if (j == SIZE_MAX || j >= r->num_cells) {
      *why = "a primary key column is not (once) in the loaded data";
      return ROW_KEY_NONE;
    }
    const char *problem = key_value_problem(&r->cells[j]);
    if (problem) {
      *why = problem;
      return ROW_KEY_BAD;
    }
    idx[n++] = j;
  }
  if (n == 0)
    return ROW_KEY_NONE;

  key->col_names = safe_malloc(sizeof(char *) * n);
  key->values = safe_malloc(sizeof(DbValue) * n);
  for (size_t i = 0; i < n; i++) {
    key->col_names[i] = data->columns[idx[i]].name;
    key->values[i] = db_value_copy(&r->cells[idx[i]]);
  }
  key->count = n;
  *why = NULL;
  return ROW_KEY_OK;
}

void row_key_free(RowKey *key) {
  free(key->col_names);
  if (key->values) {
    for (size_t i = 0; i < key->count; i++)
      db_value_free(&key->values[i]);
    free(key->values);
  }
  memset(key, 0, sizeof(*key));
}

static bool is_schema_column(const TableSchema *schema, const char *name) {
  for (size_t k = 0; schema && k < schema->num_columns; k++) {
    if (schema->columns[k].name && strcmp(schema->columns[k].name, name) == 0)
      return true;
  }
  return false;
}

bool row_key_verify(DbConnection *conn, const char *table, const RowKey *key,
                    const ResultSet *data, size_t row, size_t col,
                    const TableSchema *schema, char **why) {
  *why = NULL;
  if (!data || !data->rows || row >= data->num_rows) {
    *why = str_dup("the loaded rows changed");
    return false;
  }
  const Row *r = &data->rows[row];

  /* The columns to compare: the one being edited, or every loaded column
   * that belongs to the table (not computed or aliased) */
  const char **cols = safe_calloc(data->num_columns + 1, sizeof(char *));
  DbValue *vals = safe_calloc(data->num_columns + 1, sizeof(DbValue));
  size_t n = 0;
  for (size_t c = 0; c < data->num_columns && c < r->num_cells; c++) {
    if (col != SIZE_MAX && c != col)
      continue;
    const char *name = data->columns[c].name;
    if (!name || r->cells[c].omitted)
      continue;
    if (col == SIZE_MAX && !is_schema_column(schema, name))
      continue;
    cols[n] = name;
    vals[n] = r->cells[c]; /* Shallow: borrowed */
    n++;
  }
  /* Nothing comparable: still require the key to match exactly one row */
  bool ok = n > 0
                ? db_verify_row(conn, table, key->col_names, key->values,
                                key->count, cols, vals, n, why)
                : db_verify_row(conn, table, key->col_names, key->values,
                                key->count, key->col_names, key->values,
                                key->count, why);
  free(cols);
  free(vals);
  return ok;
}
