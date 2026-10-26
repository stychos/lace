/*
 * Lace
 * The Indexes and Foreign keys sub-views' editors: + an index (name,
 * unique, columns in order, method, partial condition), c a check
 * constraint, x drop, r/Enter rename; + a foreign key, Enter edit (drop and
 * add again), x drop, r rename. Every change goes through tui_ddl_run.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ddl_edit.h"
#include "util/mem.h"
#include "views/form_dialog.h"
#include "views/prompt.h"
#include <stdlib.h>
#include <string.h>

static const char *or_null(const char *s) { return s && *s ? s : NULL; }

/* ==========================================================================
 * Indexes
 * ========================================================================== */

enum { F_NAME = 1, F_UNIQUE, F_COLS, F_METHOD, F_WHERE, F_COND };

static const LaceIndex *tag_index(const Tab *tab, int tag) {
  if (tag < CATALOG_TAG_INDEX || !tab->schema)
    return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_INDEX);
  return i < tab->schema->num_indexes ? &tab->schema->indexes[i] : NULL;
}

static const char *kind_word(const LaceIndex *ix) {
  switch (ix->kind) {
  case LACE_INDEX_CHECK: return "check constraint";
  case LACE_INDEX_EXCLUSION: return "exclusion constraint";
  case LACE_INDEX_UNIQUE: return "unique index";
  default: return "index";
  }
}

static void index_editor(TuiState *state, Tab *tab) {
  LaceDriver d = tui_tab_driver(state);
  static const char *const pg_methods[] = {"btree", "hash", "gin", "gist", "brin", "spgist"};
  static const char *const my_methods[] = {"BTREE", "HASH", "FULLTEXT", "SPATIAL"};
  const char **names;
  size_t nn = tui_schema_column_names(tab, &names);
  char *title = str_printf("New index on %s", tab->table_name);
  FormDialog f;
  fd_init(&f, state->app->config, title);
  fd_add_text(&f, F_NAME, "Name", NULL, NULL);
  fd_add_toggle(&f, F_UNIQUE, "Unique", false);
  fd_add_list(&f, F_COLS, "Columns", NULL, names, nn);
  if (d == LACE_DRIVER_POSTGRES)
    fd_add_pick(&f, F_METHOD, "Method", NULL, pg_methods, 6, true);
  else if (tui_driver_mysql(d))
    fd_add_pick(&f, F_METHOD, "Method", NULL, my_methods, 4, true);
  else
    fd_add_pick(&f, F_METHOD, "Method", NULL, NULL, 0, true);
  if (d == LACE_DRIVER_SQLITE)
    fd_disable(&f, F_METHOD, "SQLite indexes have no method");
  fd_add_text(&f, F_WHERE, "Partial (WHERE)", NULL, "a condition: status = 'open'");
  if (tui_driver_mysql(d))
    fd_disable(&f, F_WHERE, "MySQL has no partial indexes");
  while (form_dialog_run(&f)) {
    const char *name = fd_text(&f, F_NAME);
    if (!*name) {
      fd_set_message(&f, "A name is needed");
      continue;
    }
    char **cols;
    size_t n = fd_names(&f, F_COLS, &cols);
    if (!n) {
      fd_set_message(&f, "At least one column is needed");
      continue;
    }
    char *name_s = str_dup(name), *method = str_dup(fd_text(&f, F_METHOD)), *where = str_dup(fd_text(&f, F_WHERE));
    bool unique = fd_on(&f, F_UNIQUE);
    char *err = NULL;
    char *sql = lace_ddl_create_index(d, tab->table_name, name_s, unique, (const char *const *)cols, n, or_null(method),
                                      or_null(where), &err);
    bool done = false;
    if (!sql) {
      fd_set_message(&f, err ? err : "Not possible here");
      free(err);
    } else {
      char *joined = catalog_join(cols, n), *rows = tui_ddl_rows_text(tab);
      char *summary = str_printf("Create %sindex %s on %s (%s).\nLocks the table while it builds; Esc cancels.",
                                 unique ? "unique " : "", name_s, rows, joined);
      tui_ddl_run(state, "Create index", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
      free(summary);
      free(joined);
      free(rows);
      done = true;
    }
    free(name_s);
    free(method);
    free(where);
    lace_tables_free(cols, n);
    if (done)
      break;
  }
  form_dialog_free(&f);
  free(title);
  free(names);
}

static void check_editor(TuiState *state, Tab *tab) {
  LaceDriver d = tui_tab_driver(state);
  if (d == LACE_DRIVER_SQLITE) {
    tui_set_error(state, "SQLite takes check constraints only at CREATE TABLE; create a new table and copy the rows");
    return;
  }
  char *title = str_printf("New check constraint on %s", tab->table_name);
  FormDialog f;
  fd_init(&f, state->app->config, title);
  fd_add_text(&f, F_NAME, "Name", NULL, "optional");
  fd_add_text(&f, F_COND, "Condition", NULL, "total >= 0");
  while (form_dialog_run(&f)) {
    const char *cond = fd_text(&f, F_COND);
    if (!*cond) {
      fd_set_message(&f, "A condition is needed");
      continue;
    }
    char *name = str_dup(fd_text(&f, F_NAME)), *cond_s = str_dup(cond);
    char *err = NULL, *sql = lace_ddl_add_check(d, tab->table_name, or_null(name), cond_s, &err);
    bool done = false;
    if (!sql) {
      fd_set_message(&f, err ? err : "Not possible here");
      free(err);
    } else {
      char *summary = str_printf("Add the check constraint %s(%s) to %s.\nEvery row must satisfy it already.",
                                 *name ? name : "", cond_s, tab->table_name);
      tui_ddl_run(state, "Add check constraint", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
      free(summary);
      done = true;
    }
    free(name);
    free(cond_s);
    if (done)
      break;
  }
  form_dialog_free(&f);
  free(title);
}

static void drop_index(TuiState *state, Tab *tab, const LaceIndex *ix) {
  if (ix->kind == LACE_INDEX_PRIMARY) {
    tui_set_status(state, "The primary key is changed from Structure (p)");
    return;
  }
  LaceDriver d = tui_tab_driver(state);
  char *err = NULL, *sql;
  if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION)
    sql = lace_ddl_drop_constraint(d, tab->table_name, ix->name, &err);
  else
    sql = lace_ddl_drop_index(d, tab->table_name, ix->name, ix->constraint, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
    return;
  }
  char *summary = str_printf("Drop the %s %s of %s.", kind_word(ix), ix->name ? ix->name : "", tab->table_name);
  tui_ddl_run(state, "Drop index", summary, sql, true, DDL_AFTER_SCHEMA, NULL);
  free(summary);
}

static void rename_index(TuiState *state, Tab *tab, const LaceIndex *ix) {
  if (ix->kind == LACE_INDEX_PRIMARY) {
    tui_set_status(state, "The primary key is changed from Structure (p)");
    return;
  }
  char *title = str_printf("Rename %s", ix->name ? ix->name : "index");
  PromptOptions o = {.title = title, .label = "New name", .initial = ix->name};
  char *name = tui_prompt(&o);
  free(title);
  if (!name || !*name || (ix->name && !strcmp(name, ix->name))) {
    free(name);
    return;
  }
  LaceDriver d = tui_tab_driver(state);
  char *err = NULL, *sql;
  if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION)
    sql = lace_ddl_rename_constraint(d, tab->table_name, ix->name, name, &err);
  else
    sql = lace_ddl_rename_index(d, tab->table_name, ix, name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = str_printf("Rename the %s %s of %s to %s%s", kind_word(ix), ix->name ? ix->name : "",
                               tab->table_name, name, strchr(sql, ';') ? " (dropped and created again)" : "");
    tui_ddl_run(state, "Rename index", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
    free(summary);
  }
  free(name);
}

bool tui_index_action(TuiState *state, Tab *tab, HotkeyAction a, int tag) {
  if (!tab->schema) {
    tui_set_status(state, "The structure is still loading");
    return true;
  }
  const LaceIndex *ix = tag_index(tab, tag);
  switch (a) {
  case HOTKEY_STRUCT_ADD: index_editor(state, tab); return true;
  case HOTKEY_STRUCT_CHECK: check_editor(state, tab); return true;
  case HOTKEY_STRUCT_DROP:
    if (ix) drop_index(state, tab, ix);
    else tui_set_status(state, "Pick an index first");
    return true;
  case HOTKEY_STRUCT_RENAME:
  case HOTKEY_STRUCT_EDIT:
    if (ix) rename_index(state, tab, ix);
    else tui_set_status(state, "Pick an index first");
    return true;
  case HOTKEY_STRUCT_PRIMARY_KEY: tui_set_status(state, "The primary key is changed from Structure (s, then p)"); return true;
  case HOTKEY_STRUCT_MOVE: return true;
  default: return false;
  }
}

/* ==========================================================================
 * Foreign keys
 * ========================================================================== */

enum { K_NAME = 1, K_COLS, K_REF_TABLE, K_REF_COLS, K_ON_DELETE, K_ON_UPDATE, K_DEFERRABLE, K_INIT_DEFERRED };
static const char *const ACTIONS[] = {"NO ACTION", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"};
#define SQLITE_FK_WHY "SQLite cannot add or drop a foreign key on an existing table; create a new table and copy the rows"

static const LaceForeignKey *tag_fk(const Tab *tab, int tag) {
  if (tag < CATALOG_TAG_FK || tag >= CATALOG_TAG_DEP || !tab->schema)
    return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_FK);
  return i < tab->schema->num_foreign_keys ? &tab->schema->foreign_keys[i] : NULL;
}

/* MySQL needs an index starting with the key's columns */
static bool indexed(const Tab *tab, const char *const *cols, size_t n) {
  for (size_t i = 0; tab->schema && i < tab->schema->num_indexes; i++) {
    const LaceIndex *ix = &tab->schema->indexes[i];
    if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION || ix->num_columns < n)
      continue;
    bool same = true;
    for (size_t k = 0; k < n && same; k++)
      same = ix->columns[k] && !strcmp(ix->columns[k], cols[k]);
    if (same)
      return true;
  }
  return false;
}

/* The ADD statement (and MySQL's index before it when the columns have none); *note says so */
static char *add_sql(TuiState *state, Tab *tab, const LaceDdlForeignKey *fk, char **note, char **err) {
  LaceDriver d = tui_tab_driver(state);
  *note = NULL;
  char *add = lace_ddl_add_foreign_key(d, tab->table_name, fk, err);
  if (!add)
    return NULL;
  if (tui_driver_mysql(d) && !indexed(tab, fk->columns, fk->num_columns)) {
    char *ixname = str_printf("fk_%s_%s", tab->table_name, fk->columns[0]);
    char *ix = lace_ddl_create_index(d, tab->table_name, ixname, false, fk->columns, fk->num_columns, NULL, NULL, err);
    if (!ix) {
      free(add);
      free(ixname);
      return NULL;
    }
    char *joined = catalog_join((char *const *)fk->columns, fk->num_columns);
    *note = str_printf("\nAnd an index %s on (%s), which MySQL requires.", ixname, joined);
    char *both = str_printf("%s;\n%s", ix, add);
    free(ix);
    free(add);
    free(ixname);
    free(joined);
    return both;
  }
  return add;
}

/* cur NULL: a new key; else dropped and added again */
static void fk_editor(TuiState *state, Tab *tab, const LaceForeignKey *cur) {
  LaceDriver d = tui_tab_driver(state);
  if (d == LACE_DRIVER_SQLITE) {
    tui_set_error(state, SQLITE_FK_WHY);
    return;
  }
  Connection *c = TUI_TAB_CONNECTION(state);
  const char **names;
  size_t nn = tui_schema_column_names(tab, &names);
  char *cols = cur ? catalog_join(cur->columns, cur->num_columns) : NULL;
  char *rcols = cur ? catalog_join(cur->ref_columns, cur->num_ref_columns) : NULL;
  char *old_name = cur && cur->name ? str_dup(cur->name) : NULL;
  char *title = cur ? str_printf("Foreign key %s", cur->name ? cur->name : "")
                    : str_printf("New foreign key on %s", tab->table_name);
  FormDialog f;
  fd_init(&f, state->app->config, title);
  fd_add_text(&f, K_NAME, "Name", cur ? cur->name : NULL, "optional");
  fd_add_list(&f, K_COLS, "Columns", cols, names, nn);
  fd_add_pick(&f, K_REF_TABLE, "References", cur ? cur->ref_table : NULL,
              c ? (const char *const *)c->tables : NULL, c ? c->num_tables : 0, true);
  fd_add_text(&f, K_REF_COLS, "Referenced columns", rcols, "comma-separated, in order");
  fd_add_pick(&f, K_ON_DELETE, "ON DELETE", cur ? cur->on_delete : NULL, ACTIONS, 5, true);
  fd_add_pick(&f, K_ON_UPDATE, "ON UPDATE", cur ? cur->on_update : NULL, ACTIONS, 5, true);
  fd_add_toggle(&f, K_DEFERRABLE, "Deferrable", cur && cur->deferrable);
  fd_add_toggle(&f, K_INIT_DEFERRED, "Initially deferred", cur && cur->initially_deferred);
  if (d != LACE_DRIVER_POSTGRES) {
    fd_disable(&f, K_DEFERRABLE, "Only PostgreSQL has deferrable constraints");
    fd_disable(&f, K_INIT_DEFERRED, "Only PostgreSQL has deferrable constraints");
  }
  while (form_dialog_run(&f)) {
    char **kcols, **ref_cols;
    size_t n = fd_names(&f, K_COLS, &kcols), rn = fd_names(&f, K_REF_COLS, &ref_cols);
    const char *ref_table = fd_text(&f, K_REF_TABLE);
    if (!n || !*ref_table || rn != n) {
      fd_set_message(&f, !n ? "At least one column is needed"
                            : !*ref_table ? "The referenced table is needed"
                                          : "As many referenced columns as columns are needed");
      lace_tables_free(kcols, n);
      lace_tables_free(ref_cols, rn);
      continue;
    }
    char *name = str_dup(fd_text(&f, K_NAME)), *rt = str_dup(ref_table);
    char *od = str_dup(fd_text(&f, K_ON_DELETE)), *ou = str_dup(fd_text(&f, K_ON_UPDATE));
    LaceDdlForeignKey fk = {.name = or_null(name), .columns = (const char *const *)kcols, .num_columns = n,
                            .ref_table = rt, .ref_columns = (const char *const *)ref_cols, .num_ref_columns = rn,
                            .on_delete = or_null(od), .on_update = or_null(ou),
                            .deferrable = fd_on(&f, K_DEFERRABLE), .initially_deferred = fd_on(&f, K_INIT_DEFERRED)};
    char *err = NULL, *note = NULL, *sql = add_sql(state, tab, &fk, &note, &err);
    if (sql && old_name) { /* a change: the old key goes first */
      char *drop = lace_ddl_drop_foreign_key(d, tab->table_name, old_name, &err);
      if (drop) {
        char *both = str_printf("%s;\n%s", drop, sql);
        free(sql);
        sql = both;
        free(drop);
      } else {
        free(sql);
        sql = NULL;
      }
    }
    bool done = false;
    if (!sql) {
      fd_set_message(&f, err ? err : "Not possible here");
      free(err);
    } else {
      char *joined = catalog_join(kcols, n), *rjoined = catalog_join(ref_cols, rn);
      char *summary = old_name
                          ? str_printf("Change the foreign key %s of %s: (%s) references %s (%s), by dropping it and adding it again.%s",
                                       old_name, tab->table_name, joined, rt, rjoined, note ? note : "")
                          : str_printf("Add a foreign key to %s: (%s) references %s (%s).\nEvery row must satisfy it already.%s",
                                       tab->table_name, joined, rt, rjoined, note ? note : "");
      tui_ddl_run(state, old_name ? "Change foreign key" : "Add foreign key", summary, sql, old_name != NULL,
                  DDL_AFTER_SCHEMA, NULL);
      free(summary);
      free(joined);
      free(rjoined);
      done = true;
    }
    free(note);
    free(name);
    free(rt);
    free(od);
    free(ou);
    lace_tables_free(kcols, n);
    lace_tables_free(ref_cols, rn);
    if (done)
      break;
  }
  form_dialog_free(&f);
  free(title);
  free(old_name);
  free(cols);
  free(rcols);
  free(names);
}

static void drop_fk(TuiState *state, Tab *tab, const LaceForeignKey *fk) {
  LaceDriver d = tui_tab_driver(state);
  if (d == LACE_DRIVER_SQLITE) {
    tui_set_error(state, SQLITE_FK_WHY);
    return;
  }
  if (!fk->name) {
    tui_set_error(state, "The key has no name to drop it by");
    return;
  }
  char *err = NULL, *sql = lace_ddl_drop_foreign_key(d, tab->table_name, fk->name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
    return;
  }
  char *summary = str_printf("Drop the foreign key %s of %s (references %s).", fk->name, tab->table_name,
                             fk->ref_table ? fk->ref_table : "");
  tui_ddl_run(state, "Drop foreign key", summary, sql, true, DDL_AFTER_SCHEMA, NULL);
  free(summary);
}

static void rename_fk(TuiState *state, Tab *tab, const LaceForeignKey *fk) {
  LaceDriver d = tui_tab_driver(state);
  if (d == LACE_DRIVER_SQLITE) {
    tui_set_error(state, SQLITE_FK_WHY);
    return;
  }
  if (!fk->name) {
    tui_set_error(state, "The key has no name");
    return;
  }
  char *title = str_printf("Rename %s", fk->name);
  PromptOptions o = {.title = title, .label = "New name", .initial = fk->name};
  char *name = tui_prompt(&o);
  free(title);
  if (!name || !*name || !strcmp(name, fk->name)) {
    free(name);
    return;
  }
  char *err = NULL, *sql = NULL, *note = NULL;
  if (d == LACE_DRIVER_POSTGRES) {
    sql = lace_ddl_rename_constraint(d, tab->table_name, fk->name, name, &err);
  } else {
    LaceDdlForeignKey nk = {.name = name, .columns = (const char *const *)fk->columns, .num_columns = fk->num_columns,
                            .ref_table = fk->ref_table, .ref_columns = (const char *const *)fk->ref_columns,
                            .num_ref_columns = fk->num_ref_columns, .on_delete = fk->on_delete, .on_update = fk->on_update};
    char *add = add_sql(state, tab, &nk, &note, &err);
    char *drop = add ? lace_ddl_drop_foreign_key(d, tab->table_name, fk->name, &err) : NULL;
    if (add && drop)
      sql = str_printf("%s;\n%s", drop, add);
    free(add);
    free(drop);
  }
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = str_printf("Rename the foreign key %s of %s to %s%s", fk->name, tab->table_name, name,
                               d == LACE_DRIVER_POSTGRES ? "" : " (dropped and added again)");
    tui_ddl_run(state, "Rename foreign key", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
    free(summary);
  }
  free(note);
  free(name);
}

bool tui_fk_action(TuiState *state, Tab *tab, HotkeyAction a, int tag) {
  if (!tab->schema) {
    tui_set_status(state, "The structure is still loading");
    return true;
  }
  const LaceForeignKey *fk = tag_fk(tab, tag);
  switch (a) {
  case HOTKEY_STRUCT_ADD: fk_editor(state, tab, NULL); return true;
  case HOTKEY_STRUCT_EDIT:
    if (fk) fk_editor(state, tab, fk);
    else tui_set_status(state, "Pick a foreign key first");
    return true;
  case HOTKEY_STRUCT_DROP:
    if (fk) drop_fk(state, tab, fk);
    else tui_set_status(state, "Pick a foreign key first");
    return true;
  case HOTKEY_STRUCT_RENAME:
    if (fk) rename_fk(state, tab, fk);
    else tui_set_status(state, "Pick a foreign key first");
    return true;
  case HOTKEY_STRUCT_PRIMARY_KEY: case HOTKEY_STRUCT_MOVE: case HOTKEY_STRUCT_CHECK: return true;
  default: return false;
  }
}
