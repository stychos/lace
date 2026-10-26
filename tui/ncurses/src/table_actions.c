/*
 * Lace
 * The Information sub-view's actions (the counter, the comment, MySQL's
 * engine and charset, Analyze, Vacuum, Optimize, "Edit in a query tab") and
 * the table's actions pick list (table_menu, m): rename, truncate, drop, new
 * table, new view, new function. Every change goes through tui_ddl_run.
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

static int pick(TuiState *state, const char *title, const char *const *items, size_t n) {
  int x = state->sidebar_visible ? SIDEBAR_WIDTH + 2 : 2;
  return tui_pick_from_list(state->app->config, title, items, n, 0, 3, x, 30, state->term_cols - 4,
                            state->term_rows - 6);
}

static char *ask(const char *title, const char *label, const char *initial) {
  PromptOptions o = {.title = title, .label = label, .initial = initial, .allow_empty = true};
  return tui_prompt(&o);
}

/* ==========================================================================
 * Information's actions
 * ========================================================================== */

void tui_info_action_rows(TuiState *state, Tab *tab, CatalogRows *t) {
  LaceDriver d = tui_tab_driver(state);
  bool mysql = tui_driver_mysql(d), pg = d == LACE_DRIVER_POSTGRES, sqlite = d == LACE_DRIVER_SQLITE;
  catalog_rows_blank(t);
  catalog_rows_heading(t, "Actions");
  if (tab->object != LACE_OBJ_TABLE) {
    catalog_rows_span(t, SV_TAG_EDIT_DEFINITION, "Edit in a query tab");
    return;
  }
  if (tab->autoinc && tab->autoinc->column)
    catalog_rows_span(t, SV_TAG_AUTOINC, tab->autoinc->has_counter
                                             ? "Set the next auto-increment value\xe2\x80\xa6"
                                             : "Set the next auto-increment value (no counter: the maximum + 1)");
  if (pg || mysql)
    catalog_rows_span(t, SV_TAG_COMMENT, "Set the comment\xe2\x80\xa6");
  if (mysql) {
    catalog_rows_span(t, SV_TAG_ENGINE, "Set the engine\xe2\x80\xa6");
    catalog_rows_span(t, SV_TAG_CHARSET, "Set the charset and collation\xe2\x80\xa6");
  }
  catalog_rows_span(t, SV_TAG_ANALYZE, "Analyze (update the statistics)");
  if (pg)
    catalog_rows_span(t, SV_TAG_VACUUM, "Vacuum (VACUUM ANALYZE the table)");
  if (sqlite)
    catalog_rows_span(t, SV_TAG_VACUUM, "Vacuum (the whole file: SQLite has no per-table VACUUM)");
  if (mysql)
    catalog_rows_span(t, SV_TAG_OPTIMIZE, "Optimize (OPTIMIZE TABLE)");
}

static void autoinc_set(TuiState *state, Tab *tab) {
  if (!tab->autoinc || !tab->autoinc->column)
    return;
  if (!tab->autoinc->has_counter) {
    tui_set_status(state, "A plain INTEGER PRIMARY KEY has no counter: the next rowid is the maximum + 1");
    return;
  }
  char *initial = tab->autoinc->next >= 0 ? str_printf("%lld", (long long)tab->autoinc->next) : str_dup("1");
  char *text = ask("Next auto-increment value", "Next value", initial);
  free(initial);
  if (!text)
    return;
  int64_t n;
  if (!str_to_int64(text, &n)) {
    if (*text)
      tui_set_error(state, "Not a number: %s", text);
    free(text);
    return;
  }
  free(text);
  char *err = NULL, *sql = lace_ddl_set_autoincrement(tui_tab_driver(state), tab->table_name, tab->autoinc, n, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
    return;
  }
  char *summary = str_printf("The next %s of %s becomes %lld%s", tab->autoinc->column, tab->table_name, (long long)n,
                             tab->autoinc->max >= 0 && n <= tab->autoinc->max
                                 ? " (MySQL refuses a value below the maximum; the rest allow it)" : "");
  tui_ddl_run(state, "Set auto-increment", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
  free(summary);
}

static void comment_set(TuiState *state, Tab *tab) {
  char *title = str_printf("Comment on %s", tab->table_name);
  char *text = ask(title, "Comment", tab->info ? tab->info->comment : NULL);
  free(title);
  if (!text)
    return;
  char *err = NULL;
  char *sql = lace_ddl_set_table_option(tui_tab_driver(state), tab->table_name, LACE_DDL_OPT_COMMENT, text, NULL, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = *text ? str_printf("Set the comment of %s to \"%s\"", tab->table_name, text)
                          : str_printf("Clear the comment of %s", tab->table_name);
    tui_ddl_run(state, "Set comment", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
    free(summary);
  }
  free(text);
}

static void engine_set(TuiState *state, Tab *tab) {
  size_t n = 0;
  char **engines = tui_ddl_names(state, "engines", NULL, &n);
  if (!n) {
    tui_set_error(state, "No engines to pick from");
    return;
  }
  int sel = pick(state, "Engine", (const char *const *)engines, n);
  if (sel >= 0 && (size_t)sel < n) {
    char *err = NULL;
    char *sql = lace_ddl_set_table_option(tui_tab_driver(state), tab->table_name, LACE_DDL_OPT_ENGINE, engines[sel], NULL, &err);
    if (!sql) {
      tui_ddl_refused(state, err);
    } else {
      char *summary = str_printf("Change the engine of %s to %s.\nThe table is rebuilt: it is locked meanwhile.",
                                 tab->table_name, engines[sel]);
      tui_ddl_run(state, "Set engine", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
      free(summary);
    }
  }
  lace_tables_free(engines, n);
}

enum { F_CHARSET = 1, F_COLLATION };

static void charset_set(TuiState *state, Tab *tab) {
  size_t n = 0;
  char **colls = tui_ddl_names(state, "collations", tab->info ? tab->info->charset : NULL, &n);
  char *title = str_printf("Charset of %s", tab->table_name);
  FormDialog f;
  fd_init(&f, state->app->config, title);
  fd_add_text(&f, F_CHARSET, "Charset", tab->info ? tab->info->charset : NULL, "utf8mb4");
  fd_add_pick(&f, F_COLLATION, "Collation", tab->info ? tab->info->collation : NULL, (const char *const *)colls, n, true);
  while (form_dialog_run(&f)) {
    const char *cs = fd_text(&f, F_CHARSET);
    if (!*cs) {
      fd_set_message(&f, "A charset is needed");
      continue;
    }
    char *cs_s = str_dup(cs), *coll = str_dup(fd_text(&f, F_COLLATION));
    char *err = NULL;
    char *sql = lace_ddl_set_table_option(tui_tab_driver(state), tab->table_name, LACE_DDL_OPT_CHARSET, cs_s,
                                          *coll ? coll : NULL, &err);
    bool done = false;
    if (!sql) {
      fd_set_message(&f, err ? err : "Not possible here");
      free(err);
    } else {
      char *summary = str_printf("Set the default charset of %s to %s%s%s (existing columns keep theirs)",
                                 tab->table_name, cs_s, *coll ? " with collation " : "", coll);
      tui_ddl_run(state, "Set charset", summary, sql, false, DDL_AFTER_SCHEMA, NULL);
      free(summary);
      done = true;
    }
    free(cs_s);
    free(coll);
    if (done)
      break;
  }
  form_dialog_free(&f);
  free(title);
  lace_tables_free(colls, n);
}

static void maintenance(TuiState *state, Tab *tab, LaceDdlMaintenance kind) {
  LaceDriver d = tui_tab_driver(state);
  char *err = NULL, *sql = lace_ddl_maintenance(d, tab->table_name, kind, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
    return;
  }
  const char *title = kind == LACE_DDL_ANALYZE ? "Analyze" : kind == LACE_DDL_VACUUM ? "Vacuum" : "Optimize";
  char *rows = tui_ddl_rows_text(tab);
  char *summary = kind == LACE_DDL_VACUUM && d == LACE_DRIVER_SQLITE
                      ? str_dup("Rebuild the whole database file (SQLite's VACUUM is not per table).\nLocks the database while it runs; Esc cancels.")
                      : str_printf("%s %s.\nLocks the table while it runs; Esc cancels.", title, rows);
  tui_ddl_run(state, title, summary, sql, false, DDL_AFTER_SCHEMA, NULL);
  free(summary);
  free(rows);
}

/* The definition, read now when the list has not shown it yet */
static bool ensure_definition(TuiState *state, Tab *tab) {
  if (tab->definition)
    return true;
  DbConnection *conn = TUI_CONN(state);
  if (!conn)
    return false;
  LaceObject obj = {.kind = tab->object, .name = tab->table_name, .table = tab->object_table, .detail = tab->object_detail};
  char *err = NULL;
  if (!db_object_definition(conn, &obj, &tab->definition, &err)) {
    tui_set_error(state, "%s", err ? err : "Cannot read the definition");
    free(err);
    return false;
  }
  if (!tab->definition)
    tab->definition = str_dup("");
  return true;
}

bool tui_info_activate(TuiState *state, Tab *tab, int tag) {
  switch (tag) {
  case SV_TAG_AUTOINC: autoinc_set(state, tab); return true;
  case SV_TAG_COMMENT: comment_set(state, tab); return true;
  case SV_TAG_ENGINE: engine_set(state, tab); return true;
  case SV_TAG_CHARSET: charset_set(state, tab); return true;
  case SV_TAG_ANALYZE: maintenance(state, tab, LACE_DDL_ANALYZE); return true;
  case SV_TAG_VACUUM: maintenance(state, tab, LACE_DDL_VACUUM); return true;
  case SV_TAG_OPTIMIZE: maintenance(state, tab, LACE_DDL_OPTIMIZE); return true;
  case SV_TAG_EDIT_DEFINITION:
    if (ensure_definition(state, tab))
      tui_ddl_open_query(state, tab->definition);
    return true;
  default: return false;
  }
}

/* ==========================================================================
 * The table's actions
 * ========================================================================== */

enum { M_RENAME, M_TRUNCATE, M_DROP, M_NEW_TABLE, M_NEW_VIEW, M_NEW_FUNCTION, M_EDIT_QUERY, M_COUNT };

static const char *object_word(LaceObjectKind k) {
  switch (k) {
  case LACE_OBJ_VIEW: return "view";
  case LACE_OBJ_ROUTINE: return "routine";
  case LACE_OBJ_TRIGGER: return "trigger";
  default: return "table";
  }
}

static const char *item_label(int item, LaceObjectKind kind) {
  switch (item) {
  case M_RENAME: return kind == LACE_OBJ_VIEW ? "Rename view\xe2\x80\xa6" : "Rename table\xe2\x80\xa6";
  case M_TRUNCATE: return "Truncate table\xe2\x80\xa6";
  case M_DROP:
    return kind == LACE_OBJ_VIEW ? "Drop view\xe2\x80\xa6" : kind == LACE_OBJ_ROUTINE ? "Drop routine\xe2\x80\xa6"
           : kind == LACE_OBJ_TRIGGER ? "Drop trigger\xe2\x80\xa6" : "Drop table\xe2\x80\xa6";
  case M_NEW_TABLE: return "New table\xe2\x80\xa6";
  case M_NEW_VIEW: return "New view\xe2\x80\xa6";
  case M_NEW_FUNCTION: return "New function\xe2\x80\xa6";
  case M_EDIT_QUERY: return "Edit in a query tab";
  default: return "";
  }
}

static void rename_object(TuiState *state, Tab *tab) {
  char *title = str_printf("Rename %s %s", object_word(tab->object), tab->table_name);
  PromptOptions o = {.title = title, .label = "New name", .initial = tab->table_name};
  char *name = tui_prompt(&o);
  free(title);
  if (!name || !*name || !strcmp(name, tab->table_name)) {
    free(name);
    return;
  }
  LaceDriver d = tui_tab_driver(state);
  char *err = NULL;
  char *sql = tab->object == LACE_OBJ_VIEW ? lace_ddl_rename_view(d, tab->table_name, name, &err)
                                           : lace_ddl_rename_table(d, tab->table_name, name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = str_printf("Rename the %s %s to %s.\nOpen tabs follow; query tabs keep their text.",
                               object_word(tab->object), tab->table_name, name);
    tui_ddl_run(state, tab->object == LACE_OBJ_VIEW ? "Rename view" : "Rename table", summary, sql, false,
                DDL_AFTER_RENAME, str_dup(name));
    free(summary);
  }
  free(name);
}

static void truncate_table(TuiState *state, Tab *tab) {
  char *err = NULL, *sql = lace_ddl_truncate(tui_tab_driver(state), tab->table_name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
    return;
  }
  char *rows = tui_ddl_rows_text(tab);
  char *summary = str_printf("Delete every row of %s.", rows);
  tui_ddl_run(state, "Truncate table", summary, sql, true, DDL_AFTER_SCHEMA, NULL);
  free(summary);
  free(rows);
}

/* "order_items (fk_items_order), invoices" */
static char *deps_text(const Tab *tab) {
  char *out = str_dup("");
  for (size_t i = 0; i < tab->num_deps; i++) {
    const LaceDependent *d = &tab->deps[i];
    char *n = str_printf("%s%s%s%s%s%s", out, *out ? ", " : "", d->table ? d->table : "", d->name ? " (" : "",
                         d->name ? d->name : "", d->name ? ")" : "");
    free(out);
    out = n;
  }
  return out;
}

static void drop_object(TuiState *state, Tab *tab) {
  LaceDriver d = tui_tab_driver(state);
  if (tab->object == LACE_OBJ_TABLE && !tab->deps_loaded && !tab->deps_error) { /* the confirmation names them */
    char *err = NULL;
    DbConnection *conn = TUI_CONN(state);
    if (conn && db_table_dependents(conn, tab->table_name, &tab->deps, &tab->num_deps, &err))
      tab->deps_loaded = true;
    free(err);
  }
  char *rows = tui_ddl_rows_text(tab), *deps = deps_text(tab);
  const char *word = object_word(tab->object);
  char *err = NULL, *sql = NULL;
  bool cascade = false;
  switch (tab->object) {
  case LACE_OBJ_VIEW: sql = lace_ddl_drop_view(d, tab->table_name, &err); break;
  case LACE_OBJ_ROUTINE: {
    LaceObject o = {.kind = LACE_OBJ_ROUTINE, .name = tab->table_name, .detail = tab->object_detail};
    sql = lace_ddl_drop_routine(d, &o, &err);
    break;
  }
  case LACE_OBJ_TRIGGER: sql = lace_ddl_drop_trigger(d, tab->table_name, tab->object_table, &err); break;
  default:
    if (d == LACE_DRIVER_POSTGRES && tab->num_deps) {
      /* PostgreSQL refuses while others reference it: CASCADE, after a second confirmation */
      char *msg = str_printf("Drop table %s?\nReferenced by %s: PostgreSQL drops the table only with CASCADE, "
                             "which also drops those foreign keys. The next confirmation asks for it.", rows, deps);
      bool go = tui_ddl_confirm(state, "Drop table", msg, "Continue", true);
      free(msg);
      if (!go) {
        tui_set_status(state, "Drop table: cancelled");
        free(rows);
        free(deps);
        return;
      }
      cascade = true;
    }
    sql = lace_ddl_drop_table(d, tab->table_name, cascade, &err);
    break;
  }
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = tab->object != LACE_OBJ_TABLE ? str_printf("Drop the %s %s.", word, tab->table_name)
                    : cascade ? str_printf("Drop table %s and the foreign keys of %s with it (CASCADE).", tab->table_name, deps)
                    : *deps ? str_printf("Drop table %s.\nReferenced by %s: the server may refuse, or drop their rows' keys.", rows, deps)
                            : str_printf("Drop table %s.", rows);
    char *title = str_printf("Drop %s", word);
    tui_ddl_run(state, title, summary, sql, true, DDL_AFTER_DROP, NULL);
    free(title);
    free(summary);
  }
  free(rows);
  free(deps);
}

static void new_table(TuiState *state) {
  PromptOptions o = {.title = "New table", .label = "Name"};
  char *name = tui_prompt(&o);
  if (!name)
    return;
  char *err = NULL, *sql = lace_ddl_create_table_with_id(tui_tab_driver(state), name, &err);
  if (!sql) {
    tui_ddl_refused(state, err);
  } else {
    char *summary = str_printf("Create the table %s with one column, id, then open it on Structure.", name);
    tui_ddl_run(state, "New table", summary, sql, false, DDL_AFTER_CREATE_TABLE, str_dup(name));
    free(summary);
  }
  free(name);
}

static void new_object(TuiState *state, bool function) {
  PromptOptions o = {.title = function ? "New function" : "New view", .label = "Name"};
  char *name = tui_prompt(&o);
  if (!name)
    return;
  LaceDriver d = tui_tab_driver(state);
  char *err = NULL;
  char *tpl = function ? lace_ddl_routine_template(d, name, &err) : lace_ddl_view_template(d, name, &err);
  if (!tpl) {
    tui_ddl_refused(state, err);
  } else if (tui_ddl_open_query(state, tpl)) {
    char *run_key = hotkey_get_display(state->app->config, HOTKEY_EXECUTE_QUERY);
    tui_set_status(state, "Edit the statement and run it (%s); the table list reloads then", run_key ? run_key : "Ctrl+R");
    free(run_key);
  }
  free(tpl);
  free(name);
}

void tui_table_menu(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->table_name) {
    tui_set_status(state, "The table actions need a table tab");
    return;
  }
  LaceDriver d = tui_tab_driver(state);
  int items[M_COUNT];
  size_t n = 0;
  switch (tab->object) {
  case LACE_OBJ_TABLE:
    items[n++] = M_RENAME;
    items[n++] = M_TRUNCATE;
    items[n++] = M_DROP;
    break;
  case LACE_OBJ_VIEW:
    items[n++] = M_RENAME;
    items[n++] = M_DROP;
    items[n++] = M_EDIT_QUERY;
    break;
  default:
    items[n++] = M_DROP;
    items[n++] = M_EDIT_QUERY;
    break;
  }
  items[n++] = M_NEW_TABLE;
  items[n++] = M_NEW_VIEW;
  if (d != LACE_DRIVER_SQLITE)
    items[n++] = M_NEW_FUNCTION;
  const char *labels[M_COUNT];
  for (size_t i = 0; i < n; i++)
    labels[i] = item_label(items[i], tab->object);
  char *title = str_printf("%s %s", catalog_object_word(tab->object), tab->table_name);
  int sel = pick(state, title, labels, n);
  free(title);
  if (sel < 0 || (size_t)sel >= n)
    return;
  switch (items[sel]) {
  case M_RENAME: rename_object(state, tab); break;
  case M_TRUNCATE: truncate_table(state, tab); break;
  case M_DROP: drop_object(state, tab); break;
  case M_NEW_TABLE: new_table(state); break;
  case M_NEW_VIEW: new_object(state, false); break;
  case M_NEW_FUNCTION: new_object(state, true); break;
  case M_EDIT_QUERY:
    if (ensure_definition(state, tab))
      tui_ddl_open_query(state, tab->definition);
    break;
  default: break;
  }
}
