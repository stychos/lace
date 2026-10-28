/*
 * Lace
 * The one run path of every structure change and table action (ddl_edit.h):
 * the confirmation showing the statement, the run behind the processing
 * dialog, the history entry, and the reloads of every tab of the object.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ddl_edit.h"
#include "core/history.h"
#include "util/mem.h"
#include <stdlib.h>
#include <string.h>

LaceDriver tui_tab_driver(TuiState *state) {
  DbConnection *c = TUI_CONN(state);
  return c ? c->driver_type : LACE_DRIVER_SQLITE;
}

const LaceColumn *tui_schema_column(const Tab *tab, const char *name) {
  for (size_t i = 0; name && tab && tab->schema && i < tab->schema->num_columns; i++)
    if (tab->schema->columns[i].name && !strcmp(tab->schema->columns[i].name, name))
      return &tab->schema->columns[i];
  return NULL;
}

size_t tui_schema_column_names(const Tab *tab, const char ***out) {
  size_t n = tab && tab->schema ? tab->schema->num_columns : 0;
  *out = n ? safe_calloc(n, sizeof **out) : NULL;
  for (size_t i = 0; i < n; i++)
    (*out)[i] = tab->schema->columns[i].name ? tab->schema->columns[i].name : "";
  return n;
}

void tui_ddl_refused(TuiState *state, char *error) {
  tui_set_error(state, "%s", error && *error ? error : "Not possible here");
  free(error);
}

char *tui_ddl_rows_text(const Tab *tab) {
  const char *name = tab && tab->table_name ? tab->table_name : "";
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab->data || tab->filters.num_filters)
    return str_dup(name);
  char *n = catalog_commas((int64_t)tab->total_rows);
  char *out = str_printf("%s (%s%s %s)", name, tab->row_count_approximate ? "\xe2\x89\x88" : "", n,
                         tab->total_rows == 1 ? "row" : "rows");
  free(n);
  return out;
}

char **tui_ddl_names(TuiState *state, const char *what, const char *arg, size_t *n) {
  *n = 0;
  DbConnection *conn = TUI_CONN(state);
  if (!conn)
    return NULL;
  char **names = NULL;
  char *err = NULL;
  if (!db_list_names(conn, what, arg, &names, n, &err)) {
    free(err);
    *n = 0;
    return NULL;
  }
  return names;
}

/* ==========================================================================
 * The confirmation
 * ========================================================================== */


bool tui_ddl_confirm(TuiState *state, const char *title, const char *message, const char *yes_label,
                     bool destructive) {
  return tui_confirm(state, title, message, yes_label, destructive);
}

/* ==========================================================================
 * After a successful change
 * ========================================================================== */

static bool same_object(const Tab *t, size_t conn_idx, LaceObjectKind kind, const char *object) {
  if (!t->active || t->type != TAB_TYPE_TABLE || t->connection_index != conn_idx || !t->table_name || !object)
    return false;
  bool a = t->object == LACE_OBJ_TABLE || t->object == LACE_OBJ_VIEW;
  bool b = kind == LACE_OBJ_TABLE || kind == LACE_OBJ_VIEW;
  if (a != b || (!a && t->object != kind))
    return false;
  return strcmp(t->table_name, object) == 0;
}

/* Remove tab i of workspace w: the tab_ui entries shift as tab_close does */
static void close_elsewhere(TuiState *state, size_t w, size_t i) { tui_close_tab_at(state, w, i); }

static void after_change(TuiState *state, size_t conn_idx, LaceObjectKind kind, const char *object, DdlAfter after,
                         const char *arg) {
  AppState *app = state->app;
  size_t cur_ws = app->current_workspace;
  /* Other workspaces first: nothing there is on screen */
  for (size_t w = 0; w < app->num_workspaces; w++) {
    Workspace *ws = &app->workspaces[w];
    for (size_t i = ws->num_tabs; i-- > 0;) {
      Tab *t = &ws->tabs[i];
      if (!same_object(t, conn_idx, kind, object))
        continue;
      if (after == DDL_AFTER_DROP) {
        /* The view goes, never the connection: its last tab in this
         * workspace becomes the connection's tab (the sidebar of tables) */
        size_t others = 0;
        for (size_t k = 0; k < ws->num_tabs; k++)
          others += k != i && ws->tabs[k].type != TAB_TYPE_FORM && ws->tabs[k].connection_index == conn_idx;
        if (others == 0) {
          Connection *c = app_get_connection(app, conn_idx);
          tab_make_connection(t, conn_idx, c ? c->connstr : NULL);
          if (w == cur_ws && ws->current_tab == i)
            state->sidebar_visible = state->sidebar_focused = true;
        } else {
          if (w == cur_ws)
            tui_cancel_background_load(state);
          close_elsewhere(state, w, i); /* shifts the tab_ui entries as tab_close does */
        }
        if (w == cur_ws)
          tab_restore(state);
        continue;
      }
      if (after == DDL_AFTER_RENAME && arg) {
        free(t->table_name);
        t->table_name = str_dup(arg);
      }
      undo_stack_clear(&t->undo); /* the steps name columns that may be gone */
      tab_drop_catalog(t);
      t->needs_refresh = true; /* reloaded when shown (tab_restore, tui_refresh) */
    }
  }
  Tab *cur = TUI_TAB(state);
  if (cur && cur->needs_refresh && cur->type == TAB_TYPE_TABLE && cur->table_name) {
    cur->needs_refresh = false;
    tui_load_table_data(state, cur->table_name); /* the strip, the list or the grid show the new state */
  }
  bool sidebar = after == DDL_AFTER_TABLES || after == DDL_AFTER_RENAME || after == DDL_AFTER_DROP ||
                 after == DDL_AFTER_CREATE_TABLE;
  if (sidebar) {
    Connection *c = app_get_connection(app, conn_idx);
    if (c && c->conn) {
      tui_load_tables(state, c);
      state->sidebar_tables_key = NULL; /* the sidebar position belongs to the old list */
      tui_bind_connection(state);
    }
  }
  if (after == DDL_AFTER_CREATE_TABLE && arg) {
    Tab *t = TUI_TAB(state);
    if (t && t->connection_index == conn_idx && tab_create_object(state, LACE_OBJ_TABLE, arg, NULL, NULL))
      tui_set_subview(state, SUBVIEW_STRUCTURE);
  }
}

/* ==========================================================================
 * Confirming and running
 * ========================================================================== */

bool tui_ddl_run(TuiState *state, const char *title, const char *summary, char *sql, bool destructive,
                 DdlAfter after, char *arg) {
  Tab *tab = TUI_TAB(state);
  DbConnection *conn = TUI_CONN(state);
  if (!tab || !conn || !sql) {
    tui_set_error(state, "Not connected");
    str_secure_free(sql);
    free(arg);
    return false;
  }
  const char *tx_note = "";
  if (conn->in_transaction) {
    tx_note = tui_driver_mysql(conn->driver_type) ? "\n\nMySQL commits the open transaction first."
                                                  : "\n\nRuns inside the open transaction: ROLLBACK undoes it.";
  }
  char *msg = str_printf("%s\n\n%s%s", summary ? summary : "", sql, tx_note);
  bool yes = tui_ddl_confirm(state, title, msg, destructive ? title : "Run", destructive);
  free(msg);
  if (!yes) {
    tui_set_status(state, "%s: cancelled", title);
    str_secure_free(sql);
    free(arg);
    return false;
  }
  /* What the change is about, before the tabs move */
  size_t conn_idx = tab->connection_index;
  LaceObjectKind kind = tab->object;
  char *object = str_dup(tab->table_name ? tab->table_name : "");

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_EXEC;
  op.conn = conn;
  op.sql = str_dup(sql);
  bool ok = false;
  if (async_start(&op)) {
    char *pm = str_printf("%s\xe2\x80\xa6 (Esc cancels)", title);
    bool completed = tui_show_processing_dialog_ex(state, &op, pm, 0);
    free(pm);
    if (completed && op.state == ASYNC_STATE_COMPLETED) {
      ok = true;
    } else if (op.state == ASYNC_STATE_CANCELLED) {
      tui_set_status(state, "%s: cancelled", title);
    } else {
      tui_set_error(state, "%s: %s", title, op.error ? op.error : "failed");
    }
  } else {
    tui_set_error(state, "%s: could not start", title);
  }
  async_free(&op);
  if (ok) {
    app_record_history(state->app, conn, sql, history_detect_type(sql));
    after_change(state, conn_idx, kind, object, after, arg);
    tui_set_status(state, "%s: done", title); /* after the reloads, which set their own */
  }
  free(object);
  str_secure_free(sql);
  free(arg);
  return ok;
}

/* ==========================================================================
 * A query tab for templates and definitions
 * ========================================================================== */

/* The query tabs that reload the table list after a statement ran (by uid) */
static uint64_t g_reload_uids[32];
static size_t g_num_reload;

bool tui_ddl_open_query(TuiState *state, const char *text) {
  if (!tab_create_query(state))
    return false;
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_QUERY)
    return false;
  size_t len = text ? strlen(text) : 0;
  if (len + 1 > tab->query_capacity) {
    tab->query_capacity = len + 1;
    tab->query_text = safe_realloc(tab->query_text, tab->query_capacity);
  }
  memcpy(tab->query_text, text ? text : "", len + 1);
  tab->query_len = len;
  tab->query_cursor = len;
  tab->query_scroll_line = tab->query_scroll_col = 0;
  if (g_num_reload == sizeof g_reload_uids / sizeof *g_reload_uids)
    memmove(g_reload_uids, g_reload_uids + 1, (g_num_reload - 1) * sizeof *g_reload_uids), g_num_reload--;
  g_reload_uids[g_num_reload++] = tab->uid;
  return true;
}

void tui_ddl_query_ran(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_QUERY)
    return;
  for (size_t i = 0; i < g_num_reload; i++) {
    if (g_reload_uids[i] != tab->uid)
      continue;
    Connection *c = app_get_connection(state->app, tab->connection_index);
    if (c && c->conn) {
      tui_load_tables(state, c);
      state->sidebar_tables_key = NULL;
      tui_bind_connection(state);
    }
    return;
  }
}
