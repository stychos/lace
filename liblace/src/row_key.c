/*
 * liblace - Lace Client Library
 * Row keys for UPDATE / DELETE (see row_key.h)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "row_key.h"
#include "lace.h"
#include <util/mem.h>
#include <util/str.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *key_value_problem(const LaceValue *v) {
  if (v->is_null || v->type == LACE_TYPE_NULL)
    return "its primary key contains NULL";
  if (v->type == LACE_TYPE_FLOAT && !isfinite(v->float_val))
    return "its primary key is NaN or infinite";
  if (v->omitted)
    return "its primary key is too large to load";
  return NULL;
}

static size_t find_column(const LaceResult *data, const char *name,
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

LaceRowKeyStatus lace_row_key_build(LaceRowKey *key, const LaceResult *data, size_t row,
                           const LaceSchema *schema, bool unique_names,
                           const char **why) {
  memset(key, 0, sizeof(*key));
  *why = "the table has no primary key";
  if (!data || !schema || !data->rows || !data->columns ||
      row >= data->num_rows || !data->rows[row].cells)
    return LACE_ROW_KEY_NONE;
  const LaceRow *r = &data->rows[row];

  size_t idx[LACE_MAX_PK_COLUMNS];
  size_t n = 0;
  for (size_t i = 0; i < schema->num_columns; i++) {
    if (!schema->columns[i].primary_key)
      continue;
    const char *name = schema->columns[i].name;
    if (!name || n >= LACE_MAX_PK_COLUMNS)
      return LACE_ROW_KEY_NONE;
    size_t j = find_column(data, name, unique_names);
    if (j == SIZE_MAX || j >= r->num_cells) {
      *why = "a primary key column is not (once) in the loaded data";
      return LACE_ROW_KEY_NONE;
    }
    const char *problem = key_value_problem(&r->cells[j]);
    if (problem) {
      *why = problem;
      return LACE_ROW_KEY_BAD;
    }
    idx[n++] = j;
  }
  if (n == 0)
    return LACE_ROW_KEY_NONE;

  key->names = safe_calloc(n, sizeof(char *));
  key->values = safe_calloc(n, sizeof(LaceValue));
  for (size_t i = 0; i < n; i++) {
    key->names[i] = str_dup(data->columns[idx[i]].name);
    key->values[i] = lace_value_copy(&r->cells[idx[i]]);
  }
  key->count = n;
  *why = NULL;
  return LACE_ROW_KEY_OK;
}

void lace_row_key_copy(LaceRowKey *dst, const LaceRowKey *src) {
  memset(dst, 0, sizeof(*dst));
  if (!src || src->count == 0)
    return;
  dst->names = safe_calloc(src->count, sizeof(char *));
  dst->values = safe_calloc(src->count, sizeof(LaceValue));
  for (size_t i = 0; i < src->count; i++) {
    dst->names[i] = str_dup(src->names[i]);
    dst->values[i] = lace_value_copy(&src->values[i]);
  }
  dst->count = src->count;
}

void lace_row_key_free(LaceRowKey *key) {
  if (!key)
    return;
  for (size_t i = 0; i < key->count; i++) {
    free(key->names ? key->names[i] : NULL);
    if (key->values)
      lace_value_free(&key->values[i]);
  }
  free(key->names);
  free(key->values);
  memset(key, 0, sizeof(*key));
}

LacePkValue *lace_row_key_to_pk(const LaceRowKey *key) {
  LacePkValue *pk = safe_calloc(key->count ? key->count : 1, sizeof(LacePkValue));
  for (size_t i = 0; i < key->count; i++) {
    pk[i].column = key->names[i];
    pk[i].value = key->values[i];
  }
  return pk;
}

static bool is_schema_column(const LaceSchema *schema, const char *name) {
  for (size_t k = 0; schema && k < schema->num_columns; k++) {
    if (schema->columns[k].name && strcmp(schema->columns[k].name, name) == 0)
      return true;
  }
  return false;
}

void lace_row_snapshot_build(LaceRowSnapshot *snap, const LaceResult *data, size_t row,
                        size_t col, const LaceSchema *schema) {
  memset(snap, 0, sizeof(*snap));
  if (!data || !data->rows || row >= data->num_rows)
    return;
  const LaceRow *r = &data->rows[row];
  snap->names = safe_calloc(data->num_columns + 1, sizeof(char *));
  snap->values = safe_calloc(data->num_columns + 1, sizeof(LaceValue));
  for (size_t c = 0; c < data->num_columns && c < r->num_cells; c++) {
    if (col != SIZE_MAX && c != col)
      continue;
    const char *name = data->columns[c].name;
    if (!name || r->cells[c].omitted)
      continue;
    if (col == SIZE_MAX && !is_schema_column(schema, name))
      continue;
    snap->names[snap->count] = str_dup(name);
    snap->values[snap->count] = lace_value_copy(&r->cells[c]);
    snap->count++;
  }
}

void lace_row_snapshot_free(LaceRowSnapshot *snap) {
  if (!snap)
    return;
  for (size_t i = 0; i < snap->count; i++) {
    free(snap->names[i]);
    lace_value_free(&snap->values[i]);
  }
  free(snap->names);
  free(snap->values);
  memset(snap, 0, sizeof(*snap));
}

/* Two values hold the same data: both NULL, or the same exact edit text */
static bool values_equal(const LaceValue *a, const LaceValue *b) {
  bool an = !a || a->is_null || a->type == LACE_TYPE_NULL;
  bool bn = !b || b->is_null || b->type == LACE_TYPE_NULL;
  if (an || bn)
    return an && bn;
  char *ta = lace_value_to_edit_text(a);
  char *tb = lace_value_to_edit_text(b);
  bool same = ta && tb && strcmp(ta, tb) == 0;
  free(ta);
  free(tb);
  return same;
}

bool lace_row_key_verify(lace_client_t *client, int conn_id, const char *table,
                    const LaceRowKey *key, const LaceRowSnapshot *snap, char **why) {
  *why = NULL;
  /* Nothing comparable: still require the key to match exactly one row */
  const char **cols;
  const LaceValue *vals;
  size_t n;
  if (snap && snap->count > 0) {
    cols = (const char **)snap->names;
    vals = snap->values;
    n = snap->count;
  } else {
    cols = (const char **)key->names;
    vals = key->values;
    n = key->count;
  }

  LacePkValue *pk = lace_row_key_to_pk(key);
  LaceResult *res = NULL;
  int rc = lace_select_by_pk(client, conn_id, table, pk, key->count, cols, n,
                             &res);
  free(pk);
  if (rc != LACE_OK) {
    *why = str_printf("could not re-read the row: %s", lace_client_error(client));
    return false;
  }
  bool ok = false;
  if (!res || res->num_rows == 0) {
    *why = str_dup("the row no longer exists");
  } else if (res->num_rows > 1) {
    *why = str_dup("the key matches more than one row");
  } else if (!res->rows || res->num_columns != n ||
             res->rows[0].num_cells != n) {
    *why = str_dup("the row could not be compared");
  } else {
    ok = true;
    for (size_t i = 0; i < n; i++) {
      if (!values_equal(&res->rows[0].cells[i], &vals[i])) {
        *why = str_printf("column %s changed since it was loaded", cols[i]);
        ok = false;
        break;
      }
    }
  }
  lace_result_free(res);
  return ok;
}
