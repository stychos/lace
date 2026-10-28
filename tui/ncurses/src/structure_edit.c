/*
 * Lace
 * The Structure sub-view's editors: the column form (add, Enter/e edit), x
 * drop, r rename, p the primary-key editor, Space move (MySQL); a database
 * view's, routine's or trigger's Definition opens in a query tab. The keys
 * of the other lists are dispatched from here too (tui_structure_action).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ddl_edit.h"
#include "util/mem.h"
#include "views/form_dialog.h"
#include "views/list_picker.h"
#include "views/prompt.h"
#include <stdlib.h>
#include <string.h>

enum { F_NAME = 1, F_TYPE, F_NULL, F_DEFAULT, F_COLLATION, F_COMMENT, F_AUTOINC, F_AFTER, F_COLS };

static const char *or_null(const char *s) { return s && *s ? s : NULL; }

static const LaceColumn *tag_column(const Tab *tab, int tag) {
  if (tag < CATALOG_TAG_COLUMN || !tab->schema)
    return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_COLUMN);
  return i < tab->schema->num_columns ? &tab->schema->columns[i] : NULL;
}

/* ==========================================================================
 * The column editor
 * ========================================================================== */

/* The position the After field names: NULL (the end / in place), "" (first) or a column */
static const char *after_of(const FormDialog *f) {
  const char *a = fd_text(f, F_AFTER);
  if (!*a)
    return NULL;
  if (!strcmp(a, "(first)"))
    return "";
  return a;
}

/* cur NULL: a new column */
static void column_editor(TuiState *state, Tab *tab, const LaceColumn *cur) {
  LaceDriver d = tui_tab_driver(state);
  bool mysql = tui_driver_mysql(d), sqlite = d == LACE_DRIVER_SQLITE;
  size_t nt = 0, nc = 0;
  char **types = tui_ddl_names(state, "types", NULL, &nt);
  char **colls = tui_ddl_names(state, "collations", NULL, &nc);
  char *title = cur ? str_printf("Column %s", cur->name) : str_printf("Add column to %s", tab->table_name);
  char *column = cur ? str_dup(cur->name) : NULL; /* the schema may reload under us */
  /* MySQL's After options: (first) and the other columns */
  const char **names = NULL;
  size_t nn = tui_schema_column_names(tab, &names);
  const char **after_opts = safe_calloc(nn + 2, sizeof *after_opts);
  size_t na = 0;
  after_opts[na++] = "(first)";
  for (size_t i = 0; i < nn; i++)
    if (!cur || strcmp(names[i], cur->name))
      after_opts[na++] = names[i];

  FormDialog f;
  fd_init(&f, state->app->config, title);
  fd_add_text(&f, F_NAME, "Name", cur ? cur->name : NULL, NULL);
  fd_add_pick(&f, F_TYPE, "Type", cur ? cur->type_name : NULL, (const char *const *)types, nt, true);
  fd_add_toggle(&f, F_NULL, "Nullable", cur ? cur->nullable : true);
  fd_add_text(&f, F_DEFAULT, "Default", cur ? cur->default_val : NULL, "SQL: 'text', 0, CURRENT_TIMESTAMP");
  fd_add_pick(&f, F_COLLATION, "Collation", cur ? cur->collation : NULL, (const char *const *)colls, nc, true);
  fd_add_text(&f, F_COMMENT, "Comment", cur ? cur->comment : NULL, NULL);
  fd_add_toggle(&f, F_AUTOINC, "Auto-increment", cur && (cur->auto_increment || cur->identity));
  if (mysql)
    fd_add_pick(&f, F_AFTER, cur ? "Move after" : "After", NULL, after_opts, na, false);
  if (sqlite) {
    fd_disable(&f, F_COMMENT, "SQLite has no column comments");
    if (cur) {
      fd_disable(&f, F_TYPE, SQLITE_CANNOT_ALTER);
      fd_disable(&f, F_NULL, SQLITE_CANNOT_ALTER);
      fd_disable(&f, F_DEFAULT, SQLITE_CANNOT_ALTER);
      fd_disable(&f, F_COLLATION, SQLITE_CANNOT_ALTER);
      fd_disable(&f, F_AUTOINC, SQLITE_CANNOT_ALTER);
    } else {
      fd_disable(&f, F_AUTOINC, "SQLite: only INTEGER PRIMARY KEY AUTOINCREMENT at CREATE TABLE");
    }
  } else if (cur && cur->generated) {
    fd_disable(&f, F_TYPE, "A generated column: drop it and add it again");
    fd_disable(&f, F_DEFAULT, "A generated column: drop it and add it again");
  }

  while (form_dialog_run(&f)) {
    const char *name = fd_text(&f, F_NAME);
    if (!*name) {
      fd_set_message(&f, "A name is needed");
      continue;
    }
    char *name_s = str_dup(name), *type_s = str_dup(fd_text(&f, F_TYPE)), *dflt = str_dup(fd_text(&f, F_DEFAULT));
    char *coll = str_dup(fd_text(&f, F_COLLATION)), *comment = str_dup(fd_text(&f, F_COMMENT));
    const char *after = mysql ? after_of(&f) : NULL;
    char *after_s = after ? str_dup(after) : NULL;
    LaceDdlColumn w = {.name = name_s, .type = or_null(type_s), .nullable = fd_on(&f, F_NULL),
                       .default_sql = or_null(dflt), .collation = or_null(coll),
                       .comment = sqlite ? NULL : comment, .auto_increment = fd_on(&f, F_AUTOINC), .after = after_s};
    char *err = NULL, *sql = NULL, *summary = NULL;
    const char *rtitle = "Add column";
    bool done = false;
    if (!column) {
      if (!w.type) {
        fd_set_message(&f, "A type is needed");
      } else {
        if (!w.comment || !*w.comment)
          w.comment = NULL;
        sql = lace_ddl_add_column(d, tab->table_name, &w, &err);
        summary = str_printf("Add column %s to %s", name, tab->table_name);
      }
    } else {
      const LaceColumn *c = tui_schema_column(tab, column);
      if (!c) {
        fd_set_message(&f, "The column is gone (the structure was reloaded)");
      } else {
        w.name = c->name;
        if (w.comment && !strcmp(w.comment, c->comment ? c->comment : ""))
          w.comment = NULL; /* unchanged */
        char *alter = lace_ddl_alter_column(d, tab->table_name, c, &w, &err);
        bool no_change = !alter && err && !strcmp(err, "No change");
        if (no_change) {
          free(err);
          err = NULL;
        }
        char *rename = NULL;
        if (strcmp(name, c->name) != 0 && (alter || no_change))
          rename = lace_ddl_rename_column(d, tab->table_name, c->name, name, &err);
        if (alter && rename)
          sql = str_printf("%s;\n%s", alter, rename); /* the alter names the old column */
        else if (alter)
          sql = str_dup(alter);
        else if (rename)
          sql = str_dup(rename);
        if (!sql && !err)
          fd_set_message(&f, "No change");
        else
          summary = rename ? str_printf("Change column %s of %s and rename it to %s", c->name, tab->table_name, name)
                           : str_printf("Change column %s of %s", c->name, tab->table_name);
        free(alter);
        free(rename);
        rtitle = "Change column";
      }
    }
    if (err) {
      fd_set_message(&f, err);
      free(err);
    } else if (sql) {
      tui_ddl_run(state, rtitle, summary, sql, false, DDL_AFTER_SCHEMA, NULL); /* adopts sql */
      sql = NULL;
      done = true;
    }
    free(summary);
    free(sql);
    free(name_s);
    free(type_s);
    free(dflt);
    free(coll);
    free(comment);
    free(after_s);
    if (done)
      break;
  }
  form_dialog_free(&f);
  free(after_opts);
  free(names);
  free(column);
  free(title);
  lace_tables_free(types, nt);
  lace_tables_free(colls, nc);
}

/* ==========================================================================
 * Drop, rename, move, the primary key
 * ========================================================================== */

/* "index a, foreign key b": what the schema says uses the column */
static char *column_uses(const Tab *tab, const char *name) {
  char *out = str_dup("");
  for (size_t i = 0; tab->schema && i < tab->schema->num_indexes; i++) {
    const LaceIndex *ix = &tab->schema->indexes[i];
    for (size_t k = 0; k < ix->num_columns; k++)
      if (ix->columns[k] && !strcmp(ix->columns[k], name)) {
        char *n = str_printf("%s%s%s %s", out, *out ? ", " : "",
                             ix->kind == LACE_INDEX_PRIMARY ? "the primary key" : ix->kind == LACE_INDEX_CHECK ? "check" : "index",
                             ix->name ? ix->name : "");
        free(out);
        out = n;
        break;
      }
  }
  for (size_t i = 0; tab->schema && i < tab->schema->num_foreign_keys; i++) {
    const LaceForeignKey *fk = &tab->schema->foreign_keys[i];
    for (size_t k = 0; k < fk->num_columns; k++)
      if (fk->columns[k] && !strcmp(fk->columns[k], name)) {
        char *n = str_printf("%s%sforeign key %s", out, *out ? ", " : "", fk->name ? fk->name : "");
        free(out);
        out = n;
        break;
      }
  }
  return out;
}

static void drop_column(TuiState *state, Tab *tab, const LaceColumn *c) {
  char *err = NULL, *sql = lace_ddl_drop_column(tui_tab_driver(state), tab->table_name, c->name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
    return;
  }
  char *rows = tui_ddl_rows_text(tab), *uses = column_uses(tab, c->name);
  char *summary = *uses ? str_printf("Drop column %s from %s.\nIt is part of %s: PostgreSQL drops those with it, "
                                     "MySQL and SQLite may refuse.", c->name, rows, uses)
                        : str_printf("Drop column %s from %s.", c->name, rows);
  tui_ddl_run(state, "Drop column", summary, sql, true, DDL_AFTER_SCHEMA, NULL);
  free(summary);
  free(rows);
  free(uses);
}

static void rename_column(TuiState *state, Tab *tab, const LaceColumn *c) {
  char *title = str_printf("Rename column %s", c->name);
  PromptOptions o = {.title = title, .label = "New name", .initial = c->name};
  char *name = tui_prompt(&o);
  free(title);
  if (!name || !*name || !strcmp(name, c->name)) {
    free(name);
    return;
  }
  char *err = NULL, *sql = lace_ddl_rename_column(tui_tab_driver(state), tab->table_name, c->name, name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = str_printf("Rename column %s of %s to %s", c->name, tab->table_name, name);
    tui_ddl_run(state, "Rename column", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
    free(summary);
  }
  free(name);
}

static void move_column(TuiState *state, Tab *tab, const LaceColumn *c) {
  LaceDriver d = tui_tab_driver(state);
  if (!tui_driver_mysql(d)) {
    tui_set_status(state, "Only MySQL can reorder columns; the others keep the creation order");
    return;
  }
  size_t n = tab->schema->num_columns;
  char **items = safe_calloc(n + 1, sizeof *items);
  const char **targets = safe_calloc(n + 1, sizeof *targets);
  size_t k = 0;
  items[k] = str_dup("(first)");
  targets[k++] = "";
  for (size_t i = 0; i < n; i++)
    if (strcmp(tab->schema->columns[i].name, c->name)) {
      items[k] = str_printf("after %s", tab->schema->columns[i].name);
      targets[k++] = tab->schema->columns[i].name;
    }
  char *title = str_printf("Move column %s", c->name);
  int x = state->sidebar_visible ? tui_sidebar_width(state) + 2 : 2;
  int sel = tui_pick_from_list(state->app->config, title, (const char *const *)items, k, 0, 3, x, 30,
                               state->term_cols - 4, state->term_rows - 6);
  free(title);
  if (sel >= 0 && (size_t)sel < k) {
    LaceDdlColumn w = {.name = c->name, .type = c->type_name, .nullable = c->nullable, .default_sql = c->default_val,
                       .collation = c->collation, .auto_increment = c->auto_increment || c->identity != 0,
                       .after = targets[sel]};
    char *err = NULL, *sql = lace_ddl_alter_column(d, tab->table_name, c, &w, &err);
    if (!sql) {
      tui_ddl_refused(state, err);
    } else {
      char *summary = *targets[sel] ? str_printf("Move column %s of %s after %s", c->name, tab->table_name, targets[sel])
                                    : str_printf("Move column %s of %s first", c->name, tab->table_name);
      tui_ddl_run(state, "Move column", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
      free(summary);
    }
  }
  for (size_t i = 0; i < k; i++)
    free(items[i]);
  free(items);
  free(targets);
}

static void pk_editor(TuiState *state, Tab *tab) {
  LaceDriver d = tui_tab_driver(state);
  size_t n = tab->schema->num_columns;
  char *current = str_dup("");
  for (int pos = 1; pos <= (int)n; pos++)
    for (size_t i = 0; i < n; i++)
      if (tab->schema->columns[i].key_position == pos) {
        char *t = str_printf("%s%s%s", current, *current ? ", " : "", tab->schema->columns[i].name);
        free(current);
        current = t;
      }
  const char **names;
  size_t nn = tui_schema_column_names(tab, &names);
  char *title = str_printf("Primary key of %s", tab->table_name);
  FormDialog f;
  fd_init(&f, state->app->config, title);
  fd_add_list(&f, F_COLS, "Key columns", current, names, nn);
  if (d == LACE_DRIVER_SQLITE)
    fd_disable(&f, F_COLS, SQLITE_CANNOT_ALTER);
  while (form_dialog_run(&f)) {
    char **cols;
    size_t nc = fd_names(&f, F_COLS, &cols);
    bool bad = false;
    for (size_t i = 0; i < nc && !bad; i++)
      if (!tui_schema_column(tab, cols[i])) {
        char *m = str_printf("%s is not a column of %s", cols[i], tab->table_name);
        fd_set_message(&f, m);
        free(m);
        bad = true;
      }
    if (bad) {
      lace_tables_free(cols, nc);
      continue;
    }
    const char *existing = NULL;
    for (size_t i = 0; i < tab->schema->num_indexes; i++)
      if (tab->schema->indexes[i].kind == LACE_INDEX_PRIMARY) {
        const LaceIndex *ix = &tab->schema->indexes[i];
        existing = ix->constraint && *ix->constraint ? ix->constraint : ix->name;
      }
    char *err = NULL;
    char *sql = lace_ddl_set_primary_key(d, tab->table_name, (const char *const *)cols, nc, existing, &err);
    if (!sql) {
      fd_set_message(&f, err ? err : "Not possible here");
      free(err);
      lace_tables_free(cols, nc);
      continue;
    }
    char *joined = catalog_join(cols, nc);
    char *summary = nc ? str_printf("Set the primary key of %s to (%s)", tab->table_name, joined)
                       : str_printf("Drop the primary key of %s", tab->table_name);
    tui_ddl_run(state, nc ? "Set primary key" : "Drop primary key", summary, sql, nc == 0, DDL_AFTER_SCHEMA, NULL);
    free(summary);
    free(joined);
    lace_tables_free(cols, nc);
    break;
  }
  form_dialog_free(&f);
  free(title);
  free(names);
  free(current);
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static bool structure_action(TuiState *state, Tab *tab, HotkeyAction a, int tag) {
  if (!tab->schema) {
    tui_set_status(state, "The structure is still loading");
    return true;
  }
  const LaceColumn *c = tag_column(tab, tag);
  switch (a) {
  case HOTKEY_STRUCT_ADD: column_editor(state, tab, NULL); return true;
  case HOTKEY_STRUCT_EDIT:
    if (c) column_editor(state, tab, c);
    else tui_set_status(state, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_DROP:
    if (c) drop_column(state, tab, c);
    else tui_set_status(state, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_RENAME:
    if (c) rename_column(state, tab, c);
    else tui_set_status(state, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_MOVE:
    if (c) move_column(state, tab, c);
    else tui_set_status(state, "Pick a column first");
    return true;
  case HOTKEY_STRUCT_PRIMARY_KEY: pk_editor(state, tab); return true;
  case HOTKEY_STRUCT_CHECK: tui_set_status(state, "Check constraints are added from Indexes (y)"); return true;
  default: return false;
  }
}

/* A database view, routine or trigger: its SQL is changed in a query tab */
static void definition_edit(TuiState *state, Tab *tab) {
  if (!tab->definition) {
    tui_set_status(state, "The definition is still loading");
    return;
  }
  tui_ddl_open_query(state, tab->definition);
}

static bool definition_action(TuiState *state, Tab *tab, HotkeyAction a) {
  if (a == HOTKEY_STRUCT_EDIT) {
    definition_edit(state, tab);
    return true;
  }
  tui_set_status(state, "A %s is changed in a query tab (Enter); its actions are under m",
                 tab->object == LACE_OBJ_VIEW ? "view" : tab->object == LACE_OBJ_ROUTINE ? "routine" : "trigger");
  return true;
}

bool tui_structure_action(TuiState *state, Tab *tab, HotkeyAction a, int tag) {
  switch (tab->subview) {
  case SUBVIEW_STRUCTURE:
    return tab->object == LACE_OBJ_TABLE ? structure_action(state, tab, a, tag) : definition_action(state, tab, a);
  case SUBVIEW_INDEXES: return tui_index_action(state, tab, a, tag);
  case SUBVIEW_FOREIGN_KEYS: return tui_fk_action(state, tab, a, tag);
  case SUBVIEW_INFORMATION:
    if (a == HOTKEY_STRUCT_EDIT)
      return tui_info_activate(state, tab, tag);
    tui_set_status(state, "Enter runs the action under the cursor; the table's actions are under m");
    return true;
  default: return false;
  }
}

bool tui_structure_activate(TuiState *state, Tab *tab, int tag) {
  switch (tab->subview) {
  case SUBVIEW_STRUCTURE: {
    if (tab->object != LACE_OBJ_TABLE) {
      definition_edit(state, tab);
      return true;
    }
    const LaceColumn *c = tag_column(tab, tag);
    if (!c)
      return false;
    column_editor(state, tab, c);
    return true;
  }
  case SUBVIEW_INDEXES: return tag >= CATALOG_TAG_INDEX && tui_index_action(state, tab, HOTKEY_STRUCT_EDIT, tag);
  case SUBVIEW_FOREIGN_KEYS:
    return tag >= CATALOG_TAG_FK && tag < CATALOG_TAG_DEP && tui_fk_action(state, tab, HOTKEY_STRUCT_EDIT, tag);
  case SUBVIEW_INFORMATION: return tui_info_activate(state, tab, tag);
  default: return false;
  }
}
