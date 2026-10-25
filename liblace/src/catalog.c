/*
 * Lace
 * Catalog reads for the table sub-views: indexes, constraints and foreign
 * keys of a table (into LaceSchema), the database's views, routines and
 * triggers, a table's information, an object's definition and a table's
 * auto-increment counter. Every read is driver-specific SQL sent through
 * lace_exec(); the daemon knows nothing of it.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "catalog_internal.h"
#include "client_internal.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

/* ---- cell helpers ---------------------------------------------------------- */

static const LaceValue *cell(const LaceRow *r, size_t i) {
  return r && r->cells && i < r->num_cells ? &r->cells[i] : NULL;
}

/* A copy of a cell as edit text; NULL for NULL, missing or empty */
static char *cell_text(const LaceRow *r, size_t i) {
  const LaceValue *v = cell(r, i);
  if (!v || v->is_null) return NULL;
  char *t = lace_value_to_edit_text(v);
  if (t && !*t) {
    free(t);
    return NULL;
  }
  return t;
}

static int64_t cell_int(const LaceRow *r, size_t i, int64_t dflt) {
  const LaceValue *v = cell(r, i);
  if (!v || v->is_null) return dflt;
  if (v->type == LACE_TYPE_INT) return v->int_val;
  if (v->type == LACE_TYPE_FLOAT) return (int64_t)v->float_val;
  if (v->type == LACE_TYPE_BOOL) return v->bool_val ? 1 : 0;
  if ((v->type == LACE_TYPE_TEXT || v->type == LACE_TYPE_DECIMAL) && v->text.data) {
    int64_t n;
    if (str_to_int64(v->text.data, &n)) return n;
    if (strcmp(v->text.data, "t") == 0 || strcasecmp(v->text.data, "true") == 0) return 1;
    if (strcmp(v->text.data, "f") == 0 || strcasecmp(v->text.data, "false") == 0) return 0;
  }
  return dflt;
}

static bool cell_text_is(const LaceRow *r, size_t i, const char *s) {
  const LaceValue *v = cell(r, i);
  return v && !v->is_null && v->type == LACE_TYPE_TEXT && v->text.data && strcasecmp(v->text.data, s) == 0;
}

/* The index of the result column called name, or -1 */
static long column_index(const LaceResult *res, const char *name) {
  for (size_t i = 0; res && res->columns && i < res->num_columns; i++)
    if (res->columns[i].name && strcasecmp(res->columns[i].name, name) == 0) return (long)i;
  return -1;
}

size_t lace_catalog_split_lines(const char *text, char ***out) {
  *out = NULL;
  if (!text || !*text) return 0;
  size_t n = 0, cap = 0;
  const char *p = text;
  for (;;) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (n == cap) {
      cap = cap ? cap * 2 : 4;
      *out = safe_realloc(*out, cap * sizeof **out);
    }
    (*out)[n++] = str_ndup(p, len);
    if (!e) break;
    p = e + 1;
  }
  return n;
}

static char *lit(LaceDriver driver, const char *text) { return lace_sql_string_literal(driver, text); }

/* The table reference as a string literal, quoted as SQL first for
 * PostgreSQL (to_regclass takes the SQL spelling) */
static char *table_lit(LaceDriver driver, const char *table) {
  if (driver != LACE_DRIVER_POSTGRES) return lit(driver, table);
  char *ident = lace_quote_table(driver, table);
  char *l = ident ? lit(driver, ident) : NULL;
  free(ident);
  return l;
}

/* Run sql (freed) and return the result; on failure NULL with *err set */
static LaceResult *run(lace_client_t *client, int conn_id, char *sql, int *err) {
  LaceResult *res = NULL;
  if (!sql) {
    *err = lace_fail(LACE_ERR_OUT_OF_MEMORY, NULL);
    return NULL;
  }
  *err = lace_exec(client, conn_id, sql, &res);
  free(sql);
  if (*err != LACE_OK) {
    lace_result_free(res);
    return NULL;
  }
  return res;
}

/* ---- schema: indexes, constraints, foreign keys ------------------------------ */

static LaceIndex *add_index(LaceSchema *sch) {
  sch->indexes = safe_realloc(sch->indexes, (sch->num_indexes + 1) * sizeof *sch->indexes);
  LaceIndex *ix = &sch->indexes[sch->num_indexes++];
  memset(ix, 0, sizeof *ix);
  return ix;
}

static LaceForeignKey *add_fk(LaceSchema *sch) {
  sch->foreign_keys = safe_realloc(sch->foreign_keys, (sch->num_foreign_keys + 1) * sizeof *sch->foreign_keys);
  LaceForeignKey *fk = &sch->foreign_keys[sch->num_foreign_keys++];
  memset(fk, 0, sizeof *fk);
  return fk;
}

static void push_str(char ***arr, size_t *n, char *s) {
  *arr = safe_realloc(*arr, (*n + 1) * sizeof **arr);
  (*arr)[(*n)++] = s ? s : str_dup("");
}

static LaceColumn *find_column(LaceSchema *sch, const char *name) {
  for (size_t i = 0; name && i < sch->num_columns; i++)
    if (sch->columns[i].name && strcasecmp(sch->columns[i].name, name) == 0) return &sch->columns[i];
  return NULL;
}

/* The primary key as an index entry when the catalog lists no index for
 * it (SQLite's INTEGER PRIMARY KEY has none) */
static void synthesize_primary(LaceSchema *sch) {
  for (size_t i = 0; i < sch->num_indexes; i++)
    if (sch->indexes[i].kind == LACE_INDEX_PRIMARY) return;
  int max = 0;
  for (size_t i = 0; i < sch->num_columns; i++)
    if (sch->columns[i].key_position > max) max = sch->columns[i].key_position;
  if (!max) return;
  LaceIndex *ix = add_index(sch);
  ix->name = str_dup("PRIMARY KEY");
  ix->kind = LACE_INDEX_PRIMARY;
  ix->primary = ix->unique = true;
  for (int pos = 1; pos <= max; pos++)
    for (size_t i = 0; i < sch->num_columns; i++)
      if (sch->columns[i].key_position == pos) push_str(&ix->columns, &ix->num_columns, str_dup(sch->columns[i].name));
}

static int sqlite_fill(lace_client_t *client, int conn_id, const char *table, LaceSchema *sch) {
  LaceDriver d = LACE_DRIVER_SQLITE;
  int err;
  char *tl = lit(d, table);
  /* The CREATE TABLE text: collations and check constraints live only there */
  LaceResult *res = run(client, conn_id, str_printf("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = %s", tl), &err);
  if (!res) {
    free(tl);
    return err;
  }
  char *create = res->num_rows && res->rows ? cell_text(&res->rows[0], 0) : NULL;
  lace_result_free(res);
  for (size_t i = 0; i < sch->num_columns; i++)
    if (!sch->columns[i].collation) sch->columns[i].collation = lace_sqlite_column_collation(create, sch->columns[i].name);
  char **names = NULL, **conds = NULL;
  size_t nchecks = lace_sqlite_check_constraints(create, &names, &conds);
  for (size_t i = 0; i < nchecks; i++) {
    LaceIndex *ix = add_index(sch);
    ix->kind = LACE_INDEX_CHECK;
    ix->name = names[i] ? names[i] : str_printf("check %zu", i + 1);
    ix->check = conds[i];
  }
  free(names);
  free(conds);
  free(create);

  /* Indexes with their key columns; an expression column has no name in
   * index_xinfo (cid -2) and is read from the CREATE INDEX text */
  res = run(client, conn_id,
            str_printf("SELECT il.seq, il.name, il.\"unique\", il.origin, il.partial, ii.seqno, ii.name, ii.cid, m.sql "
                       "FROM pragma_index_list(%s) il "
                       "JOIN pragma_index_xinfo(il.name) ii ON ii.key = 1 "
                       "LEFT JOIN sqlite_master m ON m.type = 'index' AND m.name = il.name "
                       "ORDER BY il.seq, ii.seqno",
                       tl),
            &err);
  if (!res) {
    free(tl);
    return err;
  }
  LaceIndex *cur = NULL;
  int64_t cur_seq = -1;
  char **parsed = NULL;
  size_t nparsed = 0;
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    int64_t seq = cell_int(r, 0, -1);
    if (!cur || seq != cur_seq) {
      for (size_t k = 0; k < nparsed; k++) free(parsed[k]);
      free(parsed);
      parsed = NULL;
      nparsed = 0;
      cur = add_index(sch);
      cur_seq = seq;
      cur->name = cell_text(r, 1);
      cur->unique = cell_int(r, 2, 0) != 0;
      bool pk = cell_text_is(r, 3, "pk");
      cur->primary = pk;
      cur->kind = pk ? LACE_INDEX_PRIMARY : cur->unique ? LACE_INDEX_UNIQUE : LACE_INDEX_PLAIN;
      if (cell_text_is(r, 3, "u")) cur->constraint = str_dup("UNIQUE");
      char *isql = cell_text(r, 8);
      if (cell_int(r, 4, 0) != 0) cur->where = lace_sqlite_index_where(isql);
      nparsed = lace_sqlite_index_columns(isql, &parsed);
      free(isql);
    }
    char *name = cell_text(r, 6);
    int64_t cid = cell_int(r, 7, 0);
    size_t seqno = (size_t)cell_int(r, 5, 0);
    if (!name && cid == -1) name = str_dup("rowid");
    if (!name && seqno < nparsed) name = str_dup(parsed[seqno]);
    push_str(&cur->columns, &cur->num_columns, name);
  }
  for (size_t k = 0; k < nparsed; k++) free(parsed[k]);
  free(parsed);
  lace_result_free(res);
  synthesize_primary(sch);

  /* Foreign keys: one row per column, grouped by id; no names on SQLite */
  res = run(client, conn_id,
            str_printf("SELECT id, seq, \"table\", \"from\", \"to\", on_update, on_delete "
                       "FROM pragma_foreign_key_list(%s) ORDER BY id, seq",
                       tl),
            &err);
  free(tl);
  if (!res) return err;
  LaceForeignKey *fk = NULL;
  int64_t fk_id = -1;
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    int64_t id = cell_int(r, 0, -1);
    if (!fk || id != fk_id) {
      fk = add_fk(sch);
      fk_id = id;
      fk->ref_table = cell_text(r, 2);
      fk->on_update = cell_text(r, 5);
      fk->on_delete = cell_text(r, 6);
    }
    push_str(&fk->columns, &fk->num_columns, cell_text(r, 3));
    push_str(&fk->ref_columns, &fk->num_ref_columns, cell_text(r, 4));
  }
  lace_result_free(res);
  return LACE_OK;
}

/* PostgreSQL's one-letter foreign-key actions in words */
static const char *pg_action(const char *c) {
  if (!c || !*c) return "NO ACTION";
  switch (c[0]) {
  case 'c': return "CASCADE";
  case 'n': return "SET NULL";
  case 'd': return "SET DEFAULT";
  case 'r': return "RESTRICT";
  default: return "NO ACTION";
  }
}

static int pg_fill(lace_client_t *client, int conn_id, const char *table, LaceSchema *sch) {
  LaceDriver d = LACE_DRIVER_POSTGRES;
  int err;
  char *tl = table_lit(d, table);
  LaceResult *res = run(client, conn_id,
                        str_printf("SELECT c.relname, i.indisunique::int, i.indisprimary::int, am.amname, "
                                   "pg_catalog.pg_get_expr(i.indpred, i.indrelid), con.conname, con.contype::text, "
                                   "(SELECT string_agg(COALESCE(a.attname, "
                                   "pg_catalog.pg_get_indexdef(i.indexrelid, k.n::int, true)), E'\\n' ORDER BY k.n) "
                                   "FROM unnest(i.indkey) WITH ORDINALITY k(attnum, n) "
                                   "LEFT JOIN pg_catalog.pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum) "
                                   "FROM pg_catalog.pg_index i "
                                   "JOIN pg_catalog.pg_class c ON c.oid = i.indexrelid "
                                   "JOIN pg_catalog.pg_am am ON am.oid = c.relam "
                                   "LEFT JOIN pg_catalog.pg_constraint con ON con.conindid = i.indexrelid "
                                   "AND con.contype IN ('p', 'u', 'x') "
                                   "WHERE i.indrelid = to_regclass(%s) "
                                   "ORDER BY i.indisprimary DESC, c.relname",
                                   tl),
                        &err);
  if (!res) {
    free(tl);
    return err;
  }
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    LaceIndex *ix = add_index(sch);
    ix->name = cell_text(r, 0);
    ix->unique = cell_int(r, 1, 0) != 0;
    ix->primary = cell_int(r, 2, 0) != 0;
    ix->type = cell_text(r, 3);
    ix->where = cell_text(r, 4);
    ix->constraint = cell_text(r, 5);
    ix->kind = ix->primary ? LACE_INDEX_PRIMARY : ix->unique ? LACE_INDEX_UNIQUE : LACE_INDEX_PLAIN;
    if (cell_text_is(r, 6, "x")) ix->kind = LACE_INDEX_EXCLUSION;
    char *cols = cell_text(r, 7);
    ix->num_columns = lace_catalog_split_lines(cols, &ix->columns);
    free(cols);
  }
  lace_result_free(res);

  /* Check and exclusion constraints */
  res = run(client, conn_id,
            str_printf("SELECT con.conname, con.contype::text, pg_catalog.pg_get_constraintdef(con.oid, true) "
                       "FROM pg_catalog.pg_constraint con "
                       "WHERE con.conrelid = to_regclass(%s) AND con.contype IN ('c', 'x') "
                       "ORDER BY con.conname",
                       tl),
            &err);
  if (!res) {
    free(tl);
    return err;
  }
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    char *name = cell_text(r, 0), *def = cell_text(r, 2);
    bool excl = cell_text_is(r, 1, "x");
    LaceIndex *ix = NULL;
    if (excl) { /* its index is listed already: attach the definition */
      for (size_t k = 0; k < sch->num_indexes; k++)
        if (sch->indexes[k].constraint && name && strcmp(sch->indexes[k].constraint, name) == 0) ix = &sch->indexes[k];
    }
    if (!ix) {
      ix = add_index(sch);
      ix->name = name;
      name = NULL;
      ix->kind = excl ? LACE_INDEX_EXCLUSION : LACE_INDEX_CHECK;
    }
    if (def && !excl && strncasecmp(def, "CHECK ", 6) == 0) { /* keep the condition alone */
      char *cond = str_dup(def + 6);
      free(def);
      def = cond;
    }
    free(ix->check);
    ix->check = def;
    free(name);
  }
  lace_result_free(res);

  res = run(client, conn_id,
            str_printf("SELECT con.conname, con.confrelid::regclass::text, con.confdeltype::text, "
                       "con.confupdtype::text, con.condeferrable::int, con.condeferred::int, "
                       "(SELECT string_agg(a.attname, E'\\n' ORDER BY k.n) "
                       "FROM unnest(con.conkey) WITH ORDINALITY k(attnum, n) "
                       "JOIN pg_catalog.pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = k.attnum), "
                       "(SELECT string_agg(a.attname, E'\\n' ORDER BY k.n) "
                       "FROM unnest(con.confkey) WITH ORDINALITY k(attnum, n) "
                       "JOIN pg_catalog.pg_attribute a ON a.attrelid = con.confrelid AND a.attnum = k.attnum) "
                       "FROM pg_catalog.pg_constraint con "
                       "WHERE con.conrelid = to_regclass(%s) AND con.contype = 'f' "
                       "ORDER BY con.conname",
                       tl),
            &err);
  free(tl);
  if (!res) return err;
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    LaceForeignKey *fk = add_fk(sch);
    fk->name = cell_text(r, 0);
    fk->ref_table = cell_text(r, 1);
    char *del = cell_text(r, 2), *upd = cell_text(r, 3);
    fk->on_delete = str_dup(pg_action(del));
    fk->on_update = str_dup(pg_action(upd));
    free(del);
    free(upd);
    fk->deferrable = cell_int(r, 4, 0) != 0;
    fk->initially_deferred = cell_int(r, 5, 0) != 0;
    char *cols = cell_text(r, 6), *refs = cell_text(r, 7);
    fk->num_columns = lace_catalog_split_lines(cols, &fk->columns);
    fk->num_ref_columns = lace_catalog_split_lines(refs, &fk->ref_columns);
    free(cols);
    free(refs);
  }
  lace_result_free(res);
  return LACE_OK;
}

static int mysql_fill(lace_client_t *client, int conn_id, LaceDriver d, const char *table, LaceSchema *sch) {
  int err;
  char *tl = lit(d, table);
  /* Column extras DESCRIBE does not show */
  LaceResult *res = run(client, conn_id,
                        str_printf("SELECT COLUMN_NAME, COLLATION_NAME, COLUMN_COMMENT, GENERATION_EXPRESSION "
                                   "FROM information_schema.COLUMNS "
                                   "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = %s",
                                   tl),
                        &err);
  if (!res) {
    free(tl);
    return err;
  }
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    char *name = cell_text(r, 0);
    LaceColumn *col = find_column(sch, name);
    free(name);
    if (!col) continue;
    free(col->collation);
    col->collation = cell_text(r, 1);
    free(col->comment);
    col->comment = cell_text(r, 2);
    free(col->generated_expr);
    col->generated_expr = col->generated ? cell_text(r, 3) : NULL;
  }
  lace_result_free(res);

  /* SHOW INDEX: one row per key column, grouped by Key_name; Expression
   * exists from MySQL 8.0.13 (not on MariaDB), so columns go by name */
  char *ident = lace_quote_table(d, table);
  res = run(client, conn_id, str_printf("SHOW INDEX FROM %s", ident), &err);
  free(ident);
  if (!res) {
    free(tl);
    return err;
  }
  long c_key = column_index(res, "Key_name"), c_nonu = column_index(res, "Non_unique"),
       c_seq = column_index(res, "Seq_in_index"), c_col = column_index(res, "Column_name"),
       c_type = column_index(res, "Index_type"), c_expr = column_index(res, "Expression");
  LaceIndex *cur = NULL;
  for (size_t i = 0; res->rows && c_key >= 0 && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    char *key = cell_text(r, (size_t)c_key);
    if (!cur || !cur->name || !key || strcmp(cur->name, key) != 0) {
      cur = add_index(sch);
      cur->name = key;
      key = NULL;
      cur->unique = c_nonu >= 0 && cell_int(r, (size_t)c_nonu, 1) == 0;
      cur->primary = cur->name && strcmp(cur->name, "PRIMARY") == 0;
      cur->kind = cur->primary ? LACE_INDEX_PRIMARY : cur->unique ? LACE_INDEX_UNIQUE : LACE_INDEX_PLAIN;
      cur->type = c_type >= 0 ? cell_text(r, (size_t)c_type) : NULL;
      if (cur->unique && !cur->primary) cur->constraint = str_dup(cur->name);
    }
    free(key);
    char *col = c_col >= 0 ? cell_text(r, (size_t)c_col) : NULL;
    if (!col && c_expr >= 0) col = cell_text(r, (size_t)c_expr);
    if (cur->primary && col) {
      LaceColumn *lc = find_column(sch, col);
      if (lc) lc->key_position = c_seq >= 0 ? (int)cell_int(r, (size_t)c_seq, 0) : (int)cur->num_columns + 1;
    }
    push_str(&cur->columns, &cur->num_columns, col);
  }
  lace_result_free(res);

  /* Check constraints: MySQL 8.0.16+ and MariaDB 10.2+; older servers have
   * no CHECK_CONSTRAINTS and the read is just skipped */
  res = run(client, conn_id,
            str_printf("SELECT tc.CONSTRAINT_NAME, cc.CHECK_CLAUSE "
                       "FROM information_schema.TABLE_CONSTRAINTS tc "
                       "JOIN information_schema.CHECK_CONSTRAINTS cc "
                       "ON cc.CONSTRAINT_SCHEMA = tc.CONSTRAINT_SCHEMA AND cc.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                       "WHERE tc.TABLE_SCHEMA = DATABASE() AND tc.TABLE_NAME = %s "
                       "AND tc.CONSTRAINT_TYPE = 'CHECK' ORDER BY tc.CONSTRAINT_NAME",
                       tl),
            &err);
  if (res) {
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      const LaceRow *r = &res->rows[i];
      LaceIndex *ix = add_index(sch);
      ix->kind = LACE_INDEX_CHECK;
      ix->name = cell_text(r, 0);
      ix->check = cell_text(r, 1);
    }
    lace_result_free(res);
  }

  res = run(client, conn_id,
            str_printf("SELECT k.CONSTRAINT_NAME, k.COLUMN_NAME, k.REFERENCED_TABLE_NAME, "
                       "k.REFERENCED_COLUMN_NAME, r.DELETE_RULE, r.UPDATE_RULE "
                       "FROM information_schema.KEY_COLUMN_USAGE k "
                       "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
                       "ON r.CONSTRAINT_SCHEMA = k.CONSTRAINT_SCHEMA AND r.CONSTRAINT_NAME = k.CONSTRAINT_NAME "
                       "AND r.TABLE_NAME = k.TABLE_NAME "
                       "WHERE k.TABLE_SCHEMA = DATABASE() AND k.TABLE_NAME = %s "
                       "AND k.REFERENCED_TABLE_NAME IS NOT NULL "
                       "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION",
                       tl),
            &err);
  free(tl);
  if (!res) return err;
  LaceForeignKey *fk = NULL;
  for (size_t i = 0; res->rows && i < res->num_rows; i++) {
    const LaceRow *r = &res->rows[i];
    char *name = cell_text(r, 0);
    if (!fk || !fk->name || !name || strcmp(fk->name, name) != 0) {
      fk = add_fk(sch);
      fk->name = name;
      name = NULL;
      fk->ref_table = cell_text(r, 2);
      fk->on_delete = cell_text(r, 4);
      fk->on_update = cell_text(r, 5);
    }
    free(name);
    push_str(&fk->columns, &fk->num_columns, cell_text(r, 1));
    push_str(&fk->ref_columns, &fk->num_ref_columns, cell_text(r, 3));
  }
  lace_result_free(res);
  return LACE_OK;
}

/* The columns' foreign_key ("table.column") from the keys read: the first
 * key naming a column wins */
static void mark_fk_columns(LaceSchema *sch) {
  for (size_t k = 0; k < sch->num_foreign_keys; k++) {
    const LaceForeignKey *fk = &sch->foreign_keys[k];
    for (size_t i = 0; i < fk->num_columns && i < fk->num_ref_columns; i++) {
      for (size_t c = 0; c < sch->num_columns; c++) {
        LaceColumn *col = &sch->columns[c];
        if (col->foreign_key || !col->name || !fk->columns[i] || strcmp(col->name, fk->columns[i]) != 0) continue;
        col->foreign_key = str_printf("%s.%s", fk->ref_table ? fk->ref_table : "", fk->ref_columns[i] ? fk->ref_columns[i] : "");
      }
    }
  }
}

int lace_catalog_fill_schema(lace_client_t *client, int conn_id, LaceDriver driver,
                             const char *table, LaceSchema *sch) {
  int rc;
  switch (driver) {
  case LACE_DRIVER_SQLITE: rc = sqlite_fill(client, conn_id, table, sch); break;
  case LACE_DRIVER_POSTGRES: rc = pg_fill(client, conn_id, table, sch); break;
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: rc = mysql_fill(client, conn_id, driver, table, sch); break;
  default: return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }
  mark_fk_columns(sch);
  return rc;
}

/* ---- objects ------------------------------------------------------------------ */

static LaceObject *add_object(LaceObject **arr, size_t *n, LaceObjectKind kind) {
  *arr = safe_realloc(*arr, (*n + 1) * sizeof **arr);
  LaceObject *o = &(*arr)[(*n)++];
  memset(o, 0, sizeof *o);
  o->kind = kind;
  return o;
}

/* PostgreSQL: public and visible bare, else schema.name, as lace_list_tables */
static char *pg_ref(const LaceRow *r, size_t schema_col, size_t name_col, size_t bare_col) {
  char *schema = cell_text(r, schema_col), *name = cell_text(r, name_col);
  bool bare = cell_int(r, bare_col, 0) != 0;
  char *ref = name ? lace_table_ref(bare ? NULL : schema, name) : NULL;
  free(schema);
  free(name);
  return ref;
}

int lace_list_objects(lace_client_t *client, int conn_id, LaceObject **objects, size_t *count) {
  if (objects) *objects = NULL;
  if (count) *count = 0;
  if (!client || !objects || !count) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  LaceObject *out = NULL;
  size_t n = 0;
  LaceResult *res;
  switch (d) {
  case LACE_DRIVER_SQLITE:
    res = run(client, conn_id,
              str_dup("SELECT type, name, tbl_name FROM sqlite_master "
                      "WHERE type IN ('view', 'trigger') ORDER BY type DESC, name"),
              &err);
    if (!res) return err;
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      const LaceRow *r = &res->rows[i];
      bool view = cell_text_is(r, 0, "view");
      LaceObject *o = add_object(&out, &n, view ? LACE_OBJ_VIEW : LACE_OBJ_TRIGGER);
      o->name = cell_text(r, 1);
      if (!view) o->table = cell_text(r, 2);
    }
    lace_result_free(res);
    break;
  case LACE_DRIVER_POSTGRES:
    res = run(client, conn_id,
              str_dup("SELECT n.nspname, c.relname, "
                      "(n.nspname = 'public' AND pg_catalog.pg_table_is_visible(c.oid))::int, "
                      "(c.relkind = 'm')::int "
                      "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                      "WHERE c.relkind IN ('v', 'm') "
                      "AND n.nspname NOT IN ('pg_catalog', 'information_schema') "
                      "AND n.nspname NOT LIKE 'pg\\_%' "
                      "AND pg_catalog.has_schema_privilege(n.oid, 'USAGE') "
                      "ORDER BY 3 DESC, n.nspname, c.relname"),
              &err);
    if (!res) return err;
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      LaceObject *o = add_object(&out, &n, LACE_OBJ_VIEW);
      o->name = pg_ref(&res->rows[i], 0, 1, 2);
      if (cell_int(&res->rows[i], 3, 0)) o->detail = str_dup("materialized");
    }
    lace_result_free(res);
    res = run(client, conn_id,
              str_dup("SELECT p.oid::regprocedure::text, "
                      "CASE WHEN COALESCE(row_to_json(p)->>'prokind', 'f') = 'p' THEN 'procedure' ELSE 'function' END "
                      "FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
                      "WHERE n.nspname NOT IN ('pg_catalog', 'information_schema') "
                      "AND n.nspname NOT LIKE 'pg\\_%' "
                      "AND pg_catalog.has_schema_privilege(n.oid, 'USAGE') "
                      "AND COALESCE(row_to_json(p)->>'prokind', 'f') IN ('f', 'p') "
                      "ORDER BY n.nspname, p.proname"),
              &err);
    if (!res) {
      lace_objects_free(out, n);
      return err;
    }
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      LaceObject *o = add_object(&out, &n, LACE_OBJ_ROUTINE);
      o->name = cell_text(&res->rows[i], 0);
      o->detail = cell_text(&res->rows[i], 1);
    }
    lace_result_free(res);
    res = run(client, conn_id,
              str_dup("SELECT t.tgname, t.tgrelid::regclass::text "
                      "FROM pg_catalog.pg_trigger t "
                      "JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid "
                      "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                      "WHERE NOT t.tgisinternal "
                      "AND n.nspname NOT IN ('pg_catalog', 'information_schema') "
                      "AND n.nspname NOT LIKE 'pg\\_%' "
                      "ORDER BY 2, t.tgname"),
              &err);
    if (!res) {
      lace_objects_free(out, n);
      return err;
    }
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      LaceObject *o = add_object(&out, &n, LACE_OBJ_TRIGGER);
      o->name = cell_text(&res->rows[i], 0);
      o->table = cell_text(&res->rows[i], 1);
    }
    lace_result_free(res);
    break;
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB:
    res = run(client, conn_id,
              str_dup("SELECT TABLE_NAME FROM information_schema.VIEWS "
                      "WHERE TABLE_SCHEMA = DATABASE() ORDER BY TABLE_NAME"),
              &err);
    if (!res) return err;
    for (size_t i = 0; res->rows && i < res->num_rows; i++)
      add_object(&out, &n, LACE_OBJ_VIEW)->name = cell_text(&res->rows[i], 0);
    lace_result_free(res);
    res = run(client, conn_id,
              str_dup("SELECT ROUTINE_NAME, LOWER(ROUTINE_TYPE) FROM information_schema.ROUTINES "
                      "WHERE ROUTINE_SCHEMA = DATABASE() ORDER BY ROUTINE_NAME"),
              &err);
    if (!res) {
      lace_objects_free(out, n);
      return err;
    }
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      LaceObject *o = add_object(&out, &n, LACE_OBJ_ROUTINE);
      o->name = cell_text(&res->rows[i], 0);
      o->detail = cell_text(&res->rows[i], 1);
    }
    lace_result_free(res);
    res = run(client, conn_id,
              str_dup("SELECT TRIGGER_NAME, EVENT_OBJECT_TABLE FROM information_schema.TRIGGERS "
                      "WHERE TRIGGER_SCHEMA = DATABASE() ORDER BY EVENT_OBJECT_TABLE, TRIGGER_NAME"),
              &err);
    if (!res) {
      lace_objects_free(out, n);
      return err;
    }
    for (size_t i = 0; res->rows && i < res->num_rows; i++) {
      LaceObject *o = add_object(&out, &n, LACE_OBJ_TRIGGER);
      o->name = cell_text(&res->rows[i], 0);
      o->table = cell_text(&res->rows[i], 1);
    }
    lace_result_free(res);
    break;
  default:
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }
  *objects = out;
  *count = n;
  return LACE_OK;
}

/* ---- table information -------------------------------------------------------- */

int lace_table_info(lace_client_t *client, int conn_id, const char *table, LaceTableInfo **info) {
  if (info) *info = NULL;
  if (!client || !table || !info) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  LaceTableInfo *ti = safe_calloc(1, sizeof *ti);
  ti->row_estimate = ti->data_bytes = ti->index_bytes = ti->total_bytes = -1;
  LaceResult *res;
  char *tl = table_lit(d, table);
  switch (d) {
  case LACE_DRIVER_SQLITE: {
    ti->schema = str_dup("main");
    /* dbstat exists only when SQLite was built with it: try, else the file */
    res = run(client, conn_id,
              str_printf("SELECT (SELECT SUM(pgsize) FROM dbstat WHERE name = %s), "
                         "(SELECT COALESCE(SUM(s.pgsize), 0) FROM dbstat s "
                         "JOIN sqlite_master m ON m.name = s.name "
                         "WHERE m.type = 'index' AND m.tbl_name = %s)",
                         tl, tl),
              &err);
    if (res) {
      if (res->rows && res->num_rows) {
        ti->data_bytes = cell_int(&res->rows[0], 0, -1);
        ti->index_bytes = cell_int(&res->rows[0], 1, -1);
        if (ti->data_bytes >= 0) ti->total_bytes = ti->data_bytes + (ti->index_bytes > 0 ? ti->index_bytes : 0);
      }
      lace_result_free(res);
    }
    if (ti->total_bytes < 0) {
      res = run(client, conn_id, str_dup("SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()"), &err);
      if (res) {
        if (res->rows && res->num_rows) ti->total_bytes = cell_int(&res->rows[0], 0, -1);
        ti->size_is_file = ti->total_bytes >= 0;
        lace_result_free(res);
      }
    }
    /* ANALYZE's row count, when it ran */
    res = run(client, conn_id, str_printf("SELECT stat FROM sqlite_stat1 WHERE tbl = %s AND idx IS NULL", tl), &err);
    if (res) {
      if (res->rows && res->num_rows) {
        char *stat = cell_text(&res->rows[0], 0);
        if (stat) ti->row_estimate = strtoll(stat, NULL, 10);
        free(stat);
      }
      lace_result_free(res);
    }
    break;
  }
  case LACE_DRIVER_POSTGRES:
    res = run(client, conn_id,
              str_printf("SELECT pg_catalog.pg_table_size(c.oid), pg_catalog.pg_indexes_size(c.oid), "
                         "pg_catalog.pg_total_relation_size(c.oid), c.reltuples::bigint, "
                         "pg_catalog.pg_get_userbyid(c.relowner), n.nspname, "
                         "pg_catalog.obj_description(c.oid, 'pg_class'), (c.relkind = 'p')::int, "
                         "ts.spcname, GREATEST(s.last_analyze, s.last_autoanalyze)::text, "
                         "pg_catalog.pg_encoding_to_char(db.encoding), db.datcollate "
                         "FROM pg_catalog.pg_class c "
                         "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                         "LEFT JOIN pg_catalog.pg_tablespace ts ON ts.oid = c.reltablespace "
                         "LEFT JOIN pg_catalog.pg_stat_user_tables s ON s.relid = c.oid "
                         "JOIN pg_catalog.pg_database db ON db.datname = current_database() "
                         "WHERE c.oid = to_regclass(%s)",
                         tl),
              &err);
    if (!res) {
      free(tl);
      lace_table_info_free(ti);
      return err;
    }
    if (res->rows && res->num_rows) {
      const LaceRow *r = &res->rows[0];
      ti->data_bytes = cell_int(r, 0, -1);
      ti->index_bytes = cell_int(r, 1, -1);
      ti->total_bytes = cell_int(r, 2, -1);
      ti->row_estimate = cell_int(r, 3, -1);
      ti->owner = cell_text(r, 4);
      ti->schema = cell_text(r, 5);
      ti->comment = cell_text(r, 6);
      ti->partitioned = cell_int(r, 7, 0) != 0;
      ti->tablespace = cell_text(r, 8);
      ti->stats_updated = cell_text(r, 9);
      ti->charset = cell_text(r, 10);
      ti->collation = cell_text(r, 11);
    }
    lace_result_free(res);
    break;
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB:
    res = run(client, conn_id,
              str_printf("SELECT t.ENGINE, t.TABLE_ROWS, t.DATA_LENGTH, t.INDEX_LENGTH, t.TABLE_COLLATION, "
                         "t.TABLE_COMMENT, t.CREATE_TIME, t.UPDATE_TIME, t.CREATE_OPTIONS, DATABASE(), "
                         "(SELECT CHARACTER_SET_NAME FROM information_schema.COLLATIONS "
                         "WHERE COLLATION_NAME = t.TABLE_COLLATION) "
                         "FROM information_schema.TABLES t "
                         "WHERE t.TABLE_SCHEMA = DATABASE() AND t.TABLE_NAME = %s",
                         tl),
              &err);
    if (!res) {
      free(tl);
      lace_table_info_free(ti);
      return err;
    }
    if (res->rows && res->num_rows) {
      const LaceRow *r = &res->rows[0];
      ti->engine = cell_text(r, 0);
      ti->row_estimate = cell_int(r, 1, -1);
      ti->data_bytes = cell_int(r, 2, -1);
      ti->index_bytes = cell_int(r, 3, -1);
      if (ti->data_bytes >= 0) ti->total_bytes = ti->data_bytes + (ti->index_bytes > 0 ? ti->index_bytes : 0);
      ti->collation = cell_text(r, 4);
      ti->comment = cell_text(r, 5);
      ti->created = cell_text(r, 6);
      ti->updated = cell_text(r, 7);
      char *opts = cell_text(r, 8);
      ti->partitioned = opts && strstr(opts, "partitioned") != NULL;
      free(opts);
      ti->schema = cell_text(r, 9);
      ti->charset = cell_text(r, 10);
      if (!ti->charset && ti->collation) { /* MariaDB's uca1400 collations are not in COLLATIONS: the name says */
        const char *u = strchr(ti->collation, '_');
        ti->charset = u ? str_ndup(ti->collation, (size_t)(u - ti->collation)) : str_dup(ti->collation);
      }
    }
    lace_result_free(res);
    break;
  default:
    free(tl);
    lace_table_info_free(ti);
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }
  free(tl);
  *info = ti;
  return LACE_OK;
}

/* ---- definitions ---------------------------------------------------------------- */

int lace_object_definition(lace_client_t *client, int conn_id, const LaceObject *obj, char **sql) {
  if (sql) *sql = NULL;
  if (!client || !obj || !obj->name || !sql) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  char *q = NULL;
  size_t col = 0; /* the result column holding the text */
  char *nl = NULL;
  switch (d) {
  case LACE_DRIVER_SQLITE: {
    nl = lit(d, obj->name);
    const char *type = obj->kind == LACE_OBJ_VIEW ? "view" : obj->kind == LACE_OBJ_TRIGGER ? "trigger" : "table";
    q = str_printf("SELECT sql FROM sqlite_master WHERE type = '%s' AND name = %s", type, nl);
    break;
  }
  case LACE_DRIVER_POSTGRES:
    if (obj->kind == LACE_OBJ_VIEW) {
      nl = table_lit(d, obj->name);
      char *ident = lace_quote_table(d, obj->name);
      char *il = ident ? lit(d, ident) : NULL;
      q = str_printf("SELECT CASE WHEN c.relkind = 'm' THEN 'CREATE MATERIALIZED VIEW ' ELSE 'CREATE OR REPLACE VIEW ' END "
                     "|| %s || E' AS\\n' || pg_catalog.pg_get_viewdef(c.oid, true) "
                     "FROM pg_catalog.pg_class c WHERE c.oid = to_regclass(%s)",
                     il ? il : "''", nl);
      free(ident);
      free(il);
    } else if (obj->kind == LACE_OBJ_ROUTINE) {
      nl = lit(d, obj->name);
      q = str_printf("SELECT pg_catalog.pg_get_functiondef(%s::regprocedure)", nl);
    } else if (obj->kind == LACE_OBJ_TRIGGER && obj->table) {
      nl = lit(d, obj->name);
      char *tl = table_lit(d, obj->table);
      q = str_printf("SELECT pg_catalog.pg_get_triggerdef(t.oid, true) FROM pg_catalog.pg_trigger t "
                     "WHERE t.tgname = %s AND t.tgrelid = to_regclass(%s)",
                     nl, tl ? tl : "''");
      free(tl);
    } else {
      nl = table_lit(d, obj->name);
      q = str_printf("SELECT 'CREATE TABLE ' || %s", nl);
    }
    break;
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: {
    char *ident = lace_quote_table(d, obj->name);
    if (obj->kind == LACE_OBJ_VIEW) {
      q = str_printf("SHOW CREATE VIEW %s", ident);
      col = 1;
    } else if (obj->kind == LACE_OBJ_ROUTINE) {
      bool proc = obj->detail && strcasecmp(obj->detail, "procedure") == 0;
      q = str_printf("SHOW CREATE %s %s", proc ? "PROCEDURE" : "FUNCTION", ident);
      col = 2;
    } else if (obj->kind == LACE_OBJ_TRIGGER) {
      q = str_printf("SHOW CREATE TRIGGER %s", ident);
      col = 2;
    } else {
      q = str_printf("SHOW CREATE TABLE %s", ident);
      col = 1;
    }
    free(ident);
    break;
  }
  default:
    return lace_fail(LACE_ERR_INTERNAL_ERROR, NULL);
  }
  free(nl);
  LaceResult *res = run(client, conn_id, q, &err);
  if (!res) return err;
  if (!res->rows || !res->num_rows) {
    lace_result_free(res);
    return lace_fail(LACE_ERR_TABLE_NOT_FOUND, NULL);
  }
  *sql = cell_text(&res->rows[0], col);
  lace_result_free(res);
  if (!*sql) *sql = str_dup("");
  return LACE_OK;
}

/* ---- auto-increment -------------------------------------------------------------- */

int lace_autoincrement_get(lace_client_t *client, int conn_id, const char *table, LaceAutoInc **out) {
  if (out) *out = NULL;
  if (!client || !table || !out) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  LaceSchema *sch = NULL;
  err = lace_get_schema(client, conn_id, table, &sch);
  if (err != LACE_OK) return err;
  const LaceColumn *ai = NULL;
  for (size_t i = 0; i < sch->num_columns && !ai; i++)
    if (sch->columns[i].auto_increment || sch->columns[i].identity) ai = &sch->columns[i];
  LaceAutoInc *a = safe_calloc(1, sizeof *a);
  a->next = a->max = -1;
  if (!ai) {
    lace_schema_free(sch);
    *out = a;
    return LACE_OK;
  }
  a->column = str_dup(ai->name);
  a->has_counter = true;
  char *ident = lace_quote_table(d, table), *cident = lace_quote_identifier(d, ai->name);
  LaceResult *res = run(client, conn_id, str_printf("SELECT MAX(%s) FROM %s", cident, ident), &err);
  if (res) {
    if (res->rows && res->num_rows) a->max = cell_int(&res->rows[0], 0, -1);
    lace_result_free(res);
  }
  char *tl = table_lit(d, table);
  switch (d) {
  case LACE_DRIVER_SQLITE: {
    /* A counter exists only with the AUTOINCREMENT keyword */
    res = run(client, conn_id, str_printf("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = %s", tl), &err);
    char *create = res && res->rows && res->num_rows ? cell_text(&res->rows[0], 0) : NULL;
    lace_result_free(res);
    a->has_counter = lace_sqlite_has_autoincrement(create);
    free(create);
    if (a->has_counter) {
      res = run(client, conn_id, str_printf("SELECT seq FROM sqlite_sequence WHERE name = %s", tl), &err);
      a->next = 1;
      if (res) {
        if (res->rows && res->num_rows) a->next = cell_int(&res->rows[0], 0, 0) + 1;
        lace_result_free(res);
      }
    } else {
      a->next = a->max >= 0 ? a->max + 1 : 1;
    }
    break;
  }
  case LACE_DRIVER_POSTGRES: {
    char *cl = lit(d, ai->name);
    res = run(client, conn_id, str_printf("SELECT pg_catalog.pg_get_serial_sequence(%s, %s)", tl, cl), &err);
    free(cl);
    if (res) {
      if (res->rows && res->num_rows) a->sequence = cell_text(&res->rows[0], 0);
      lace_result_free(res);
    }
    if (a->sequence) {
      /* The sequence relation itself: pg_sequences.last_value is only what
       * reached the disk, and is NULL before the first nextval */
      res = run(client, conn_id,
                str_printf("SELECT CASE WHEN is_called THEN last_value + 1 ELSE last_value END FROM %s",
                           a->sequence),
                &err);
      if (res) {
        if (res->rows && res->num_rows) a->next = cell_int(&res->rows[0], 0, -1);
        lace_result_free(res);
      }
    }
    break;
  }
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB:
    res = run(client, conn_id,
              str_printf("SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                         "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = %s",
                         tl),
              &err);
    if (res) {
      if (res->rows && res->num_rows) a->next = cell_int(&res->rows[0], 0, -1);
      lace_result_free(res);
    }
    if (d == LACE_DRIVER_MYSQL) {
      /* MySQL 8 serves information_schema.TABLES from a cache up to a day
       * old (information_schema_stats_expiry); SHOW CREATE TABLE's table
       * options carry the live counter */
      res = run(client, conn_id, str_printf("SHOW CREATE TABLE %s", ident), &err);
      char *create = res && res->rows && res->num_rows ? cell_text(&res->rows[0], 1) : NULL;
      lace_result_free(res);
      const char *opts = create ? strrchr(create, ')') : NULL; /* the table options follow the column list */
      const char *at = opts ? strstr(opts, " AUTO_INCREMENT=") : NULL;
      if (at) {
        char *end = NULL;
        long long n = strtoll(at + 16, &end, 10);
        if (end && end != at + 16 && n > 0) a->next = n;
      }
      free(create);
    }
    break;
  default: break;
  }
  free(tl);
  free(ident);
  free(cident);
  lace_schema_free(sch);
  *out = a;
  return LACE_OK;
}

/* ---- pickers --------------------------------------------------------------- */

/* Append a copy of s to a growing name array */
static void push_name(char ***arr, size_t *n, size_t *cap, const char *s) {
  if (!s || !*s) return;
  for (size_t i = 0; i < *n; i++)
    if (strcmp((*arr)[i], s) == 0) return;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *arr = safe_realloc(*arr, *cap * sizeof **arr);
  }
  (*arr)[(*n)++] = str_dup(s);
}

static void push_fixed(char ***arr, size_t *n, size_t *cap, const char *const *list) {
  for (size_t i = 0; list[i]; i++) push_name(arr, n, cap, list[i]);
}

/* The first column of every row, in order */
static void push_rows(char ***arr, size_t *n, size_t *cap, LaceResult *res) {
  for (size_t i = 0; res && res->rows && i < res->num_rows; i++) {
    char *s = cell_text(&res->rows[i], 0);
    push_name(arr, n, cap, s);
    free(s);
  }
}

static int finish_names(char **arr, size_t n, char ***names, size_t *count) {
  *names = n ? arr : NULL;
  *count = n;
  if (!n) free(arr);
  return LACE_OK;
}

int lace_list_collations(lace_client_t *client, int conn_id, const char *charset, char ***names, size_t *count) {
  if (names) *names = NULL;
  if (count) *count = 0;
  if (!client || !names || !count) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  char **arr = NULL;
  size_t n = 0, cap = 0;
  LaceResult *res = NULL;
  switch (d) {
  case LACE_DRIVER_SQLITE: {
    static const char *const sqlite_collations[] = {"BINARY", "NOCASE", "RTRIM", NULL};
    push_fixed(&arr, &n, &cap, sqlite_collations);
    break;
  }
  case LACE_DRIVER_POSTGRES:
    push_name(&arr, &n, &cap, "default");
    res = run(client, conn_id,
              str_dup("SELECT c.collname FROM pg_catalog.pg_collation c "
                      "JOIN pg_catalog.pg_namespace n ON n.oid = c.collnamespace "
                      "WHERE c.collname <> 'default' "
                      "AND (c.collprovider <> 'c' OR c.collencoding IN (-1, pg_catalog.pg_char_to_encoding("
                      "pg_catalog.getdatabaseencoding()))) "
                      "ORDER BY (n.nspname = 'pg_catalog') DESC, c.collname"),
              &err);
    if (!res) {
      free(arr);
      return err;
    }
    push_rows(&arr, &n, &cap, res);
    lace_result_free(res);
    break;
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: {
    char *sql;
    if (charset && *charset) {
      char *l = lit(d, charset);
      sql = str_printf("SELECT COLLATION_NAME FROM information_schema.COLLATIONS "
                       "WHERE CHARACTER_SET_NAME = %s ORDER BY IS_DEFAULT = 'Yes' DESC, COLLATION_NAME",
                       l);
      free(l);
    } else {
      sql = str_dup("SELECT COLLATION_NAME FROM information_schema.COLLATIONS ORDER BY COLLATION_NAME");
    }
    res = run(client, conn_id, sql, &err);
    if (!res) {
      free(arr);
      return err;
    }
    push_rows(&arr, &n, &cap, res);
    lace_result_free(res);
    break;
  }
  default: break;
  }
  return finish_names(arr, n, names, count);
}

int lace_list_types(lace_client_t *client, int conn_id, char ***names, size_t *count) {
  if (names) *names = NULL;
  if (count) *count = 0;
  if (!client || !names || !count) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  static const char *const sqlite_types[] = {"INTEGER", "REAL", "TEXT", "BLOB", "NUMERIC", "BOOLEAN",
                                             "DATE", "DATETIME", NULL};
  static const char *const pg_types[] = {
      "smallint", "integer", "bigint", "numeric", "real", "double precision", "boolean", "text",
      "varchar", "char", "bytea", "date", "time", "timetz", "timestamp", "timestamptz",
      "interval", "uuid", "json", "jsonb", "xml", "inet", "cidr", "macaddr", "money",
      "point", "serial", "bigserial", "integer[]", "text[]", NULL};
  static const char *const mysql_types[] = {
      "TINYINT", "SMALLINT", "MEDIUMINT", "INT", "BIGINT", "DECIMAL", "FLOAT", "DOUBLE", "BIT",
      "BOOLEAN", "CHAR", "VARCHAR(255)", "TINYTEXT", "TEXT", "MEDIUMTEXT", "LONGTEXT", "BINARY",
      "VARBINARY(255)", "TINYBLOB", "BLOB", "MEDIUMBLOB", "LONGBLOB", "DATE", "TIME", "DATETIME",
      "TIMESTAMP", "YEAR", "JSON", "ENUM", "SET", "GEOMETRY", "POINT", NULL};
  char **arr = NULL;
  size_t n = 0, cap = 0;
  switch (d) {
  case LACE_DRIVER_SQLITE: push_fixed(&arr, &n, &cap, sqlite_types); break;
  case LACE_DRIVER_POSTGRES: {
    push_fixed(&arr, &n, &cap, pg_types);
    LaceResult *res = run(client, conn_id,
                          str_dup("SELECT CASE WHEN pg_catalog.pg_type_is_visible(t.oid) THEN t.typname "
                                  "ELSE n.nspname || '.' || t.typname END "
                                  "FROM pg_catalog.pg_type t JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace "
                                  "WHERE t.typtype IN ('e', 'd', 'c') "
                                  "AND n.nspname NOT IN ('pg_catalog', 'information_schema') "
                                  "AND n.nspname NOT LIKE 'pg\\_%' "
                                  "AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c "
                                  "WHERE c.oid = t.typrelid AND c.relkind <> 'c') "
                                  "ORDER BY 1"),
                          &err);
    if (res) { /* a failing read leaves the fixed list */
      push_rows(&arr, &n, &cap, res);
      lace_result_free(res);
    }
    break;
  }
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: push_fixed(&arr, &n, &cap, mysql_types); break;
  default: break;
  }
  return finish_names(arr, n, names, count);
}

int lace_list_engines(lace_client_t *client, int conn_id, char ***names, size_t *count) {
  if (names) *names = NULL;
  if (count) *count = 0;
  if (!client || !names || !count) return lace_fail(LACE_ERR_INVALID_PARAMS, NULL);
  LaceDriver d;
  int err = lace_client_driver_of(client, conn_id, &d);
  if (err != LACE_OK) return err;
  if (d != LACE_DRIVER_MYSQL && d != LACE_DRIVER_MARIADB) return LACE_OK;
  LaceResult *res = run(client, conn_id,
                        str_dup("SELECT ENGINE FROM information_schema.ENGINES "
                                "WHERE SUPPORT IN ('DEFAULT', 'YES') ORDER BY SUPPORT = 'DEFAULT' DESC, ENGINE"),
                        &err);
  if (!res) return err;
  char **arr = NULL;
  size_t n = 0, cap = 0;
  push_rows(&arr, &n, &cap, res);
  lace_result_free(res);
  return finish_names(arr, n, names, count);
}
