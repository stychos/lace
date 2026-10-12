/*
 * liblace - Lace Client Library
 * Keyset (cursor-based) pagination
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/keyset.h"
#include "../include/lace.h"
#include "../include/util/mem.h"
#include "../include/util/str.h"
#include "client_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct LaceKeysetState {
  char **col_names; /* Sort columns, then the key columns not among them */
  bool *desc;       /* Direction of each */
  bool *nullable;   /* Can hold NULL (else the NULL branches are left out,
                       which keeps the condition an index range) */
  size_t num_columns;
  LaceDriver driver;
};

static bool is_mysql(LaceDriver d) {
  return d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB;
}

/* Can values of this column be compared exactly with a literal? */
static bool column_comparable(const LaceColumn *col, LaceDriver driver) {
  return !(col->type == LACE_TYPE_FLOAT &&
           (driver == LACE_DRIVER_POSTGRES || is_mysql(driver)));
}

/* A key column holds no NULL, except in a SQLite non-INTEGER primary key
 * declared without NOT NULL */
static bool column_nullable(const LaceColumn *col, LaceDriver driver) {
  if (col->primary_key && (driver != LACE_DRIVER_SQLITE || col->auto_increment))
    return false;
  return col->nullable;
}

static bool add_column(LaceKeysetState *ks, const LaceColumn *col, bool desc) {
  if (!col->name || !column_comparable(col, ks->driver))
    return false;
  for (size_t i = 0; i < ks->num_columns; i++) {
    if (strcmp(ks->col_names[i], col->name) == 0)
      return true; /* Already ordered by it: a repeat changes nothing */
  }
  ks->col_names[ks->num_columns] = str_dup(col->name);
  if (!ks->col_names[ks->num_columns])
    return false;
  ks->desc[ks->num_columns] = desc;
  ks->nullable[ks->num_columns] = column_nullable(col, ks->driver);
  ks->num_columns++;
  return true;
}

LaceKeysetState *lace_keyset_create(const LaceSchema *schema,
                                    const LaceSort *sorts, size_t num_sorts,
                                    LaceDriver driver) {
  if (!schema || !schema->columns || (num_sorts > 0 && !sorts))
    return NULL;
  bool has_pk = false;
  for (size_t i = 0; i < schema->num_columns; i++)
    has_pk = has_pk || schema->columns[i].primary_key;
  if (!has_pk)
    return NULL;

  size_t cap = num_sorts + schema->num_columns;
  LaceKeysetState *ks = safe_calloc(1, sizeof(*ks));
  ks->col_names = safe_calloc(cap, sizeof(char *));
  ks->desc = safe_calloc(cap, sizeof(bool));
  ks->nullable = safe_calloc(cap, sizeof(bool));
  ks->driver = driver;

  bool ok = true;
  for (size_t i = 0; ok && i < num_sorts; i++) {
    ok = sorts[i].column < schema->num_columns &&
         add_column(ks, &schema->columns[sorts[i].column],
                    sorts[i].dir == LACE_SORT_DESC);
  }
  for (size_t i = 0; ok && i < schema->num_columns; i++) {
    if (schema->columns[i].primary_key)
      ok = add_column(ks, &schema->columns[i], false);
  }
  if (!ok) {
    lace_keyset_free(ks);
    return NULL;
  }
  return ks;
}

void lace_keyset_free(LaceKeysetState *ks) {
  if (!ks)
    return;
  for (size_t i = 0; i < ks->num_columns; i++)
    free(ks->col_names[i]);
  free(ks->col_names);
  free(ks->desc);
  free(ks->nullable);
  free(ks);
}

size_t lace_keyset_num_columns(const LaceKeysetState *ks) {
  return ks ? ks->num_columns : 0;
}

char *lace_keyset_build_order(const LaceKeysetState *ks, bool forward) {
  if (!ks || ks->num_columns == 0)
    return NULL;
  StringBuilder *sb = sb_new(128);
  for (size_t i = 0; i < ks->num_columns; i++) {
    char *col = lace_quote_identifier(ks->driver, ks->col_names[i]);
    if (!col) {
      sb_free(sb);
      return NULL;
    }
    /* Backward is the exact reverse, NULL placement included: each
     * driver's default NULL position flips with the direction */
    bool desc = forward ? ks->desc[i] : !ks->desc[i];
    sb_printf(sb, "%s%s %s", i ? ", " : "", col, desc ? "DESC" : "ASC");
    free(col);
  }
  return sb_finish(sb);
}

static bool value_is_null(const LaceValue *v) {
  return v->is_null || v->type == LACE_TYPE_NULL;
}

char *lace_keyset_build_where(const LaceKeysetState *ks, const LaceResult *data,
                              size_t row, bool forward) {
  if (!ks || ks->num_columns == 0 || !data || !data->rows || !data->columns ||
      row >= data->num_rows || !data->rows[row].cells)
    return NULL;
  const LaceRow *r = &data->rows[row];

  /* The boundary row's value and SQL literal of every keyset column */
  const LaceValue **vals = safe_calloc(ks->num_columns, sizeof(*vals));
  char **lits = safe_calloc(ks->num_columns, sizeof(char *));
  char **cols = safe_calloc(ks->num_columns, sizeof(char *));
  bool ok = true;
  for (size_t i = 0; ok && i < ks->num_columns; i++) {
    size_t c = SIZE_MAX;
    for (size_t j = 0; j < data->num_columns && j < r->num_cells; j++) {
      if (data->columns[j].name &&
          strcmp(data->columns[j].name, ks->col_names[i]) == 0) {
        c = j;
        break;
      }
    }
    if (c == SIZE_MAX) {
      ok = false;
      break;
    }
    vals[i] = &r->cells[c];
    if (vals[i]->omitted ||
        (vals[i]->type == LACE_TYPE_FLOAT && !value_is_null(vals[i]) &&
         !isfinite(vals[i]->float_val))) {
      ok = false;
      break;
    }
    cols[i] = lace_quote_identifier(ks->driver, ks->col_names[i]);
    lits[i] = value_is_null(vals[i])
                  ? NULL
                  : lace_value_to_sql_literal(vals[i], ks->driver);
    ok = cols[i] && (value_is_null(vals[i]) || lits[i]);
  }

  /* A row comparison, (c1, c2) > (v1, v2), when every column goes the same
   * way and none can be NULL: SQLite and PostgreSQL answer it with one
   * range scan of a matching index. MySQL plans the expanded form better. */
  bool row_form = ok && ks->num_columns > 1 && !is_mysql(ks->driver);
  for (size_t i = 0; row_form && i < ks->num_columns; i++)
    row_form = !ks->nullable[i] && ks->desc[i] == ks->desc[0] &&
               !value_is_null(vals[i]);

  char *where = NULL;
  if (row_form) {
    StringBuilder *sb = sb_new(256);
    sb_append(sb, "(");
    for (size_t i = 0; i < ks->num_columns; i++)
      sb_printf(sb, "%s%s", i ? ", " : "", cols[i]);
    sb_printf(sb, ") %s (", (forward != ks->desc[0]) ? ">" : "<");
    for (size_t i = 0; i < ks->num_columns; i++)
      sb_printf(sb, "%s%s", i ? ", " : "", lits[i]);
    sb_append(sb, ")");
    where = sb_finish(sb);
  } else if (ok) {
    /* Expanded form, valid on every driver:
     *   c1 > v1 OR (c1 = v1 AND c2 > v2) OR (c1 = v1 AND c2 = v2 AND ...) */
    StringBuilder *sb = sb_new(256);
    sb_append(sb, "(");
    for (size_t i = 0; i < ks->num_columns; i++) {
      sb_append(sb, i ? " OR (" : "(");
      for (size_t j = 0; j < i; j++) {
        if (value_is_null(vals[j]))
          sb_printf(sb, "%s IS NULL AND ", cols[j]);
        else
          sb_printf(sb, "%s = %s AND ", cols[j], lits[j]);
      }
      /* Forward: > for ASC, < for DESC; backward: the reverse */
      bool desc = ks->desc[i];
      const char *op = (forward != desc) ? ">" : "<";
      /* NULLs sort last for ASC on PostgreSQL and first on MySQL/SQLite
       * (the reverse for DESC); walking backward flips which side they
       * are on. Rows "after" the boundary in this column follow that. */
      bool nulls_last = ks->driver == LACE_DRIVER_POSTGRES ? !desc : desc;
      bool nulls_after = (forward ? nulls_last : !nulls_last) && ks->nullable[i];
      if (value_is_null(vals[i])) {
        /* After a NULL: the non-NULL values, or nothing if NULLs come last */
        if (nulls_after)
          sb_append(sb, "1 = 0");
        else
          sb_printf(sb, "%s IS NOT NULL", cols[i]);
      } else if (nulls_after) {
        sb_printf(sb, "(%s %s %s OR %s IS NULL)", cols[i], op, lits[i], cols[i]);
      } else {
        sb_printf(sb, "%s %s %s", cols[i], op, lits[i]);
      }
      sb_append(sb, ")");
    }
    sb_append(sb, ")");
    where = sb_finish(sb);
  }

  for (size_t i = 0; i < ks->num_columns; i++) {
    free(cols[i]);
    free(lits[i]);
  }
  free(cols);
  free(lits);
  free(vals);
  return where;
}
