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

/* Greedy word wrap by display width into lines (allocated) */
static size_t wrap_lines(const char *text, int width, char ***out) {
  *out = NULL;
  size_t n = 0, cap = 0;
  const char *p = text;
  if (width < 4)
    width = 4;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t seg = eol ? (size_t)(eol - p) : strlen(p);
    size_t pos = 0;
    do {
      size_t keep = utf8_fit(p + pos, seg - pos, width, NULL);
      if (keep < seg - pos) { /* break at the last space when there is one past the first third */
        size_t sp = keep;
        while (sp > 0 && p[pos + sp] != ' ')
          sp--;
        if (sp > keep / 3)
          keep = sp;
      }
      if (keep == 0)
        keep = utf8_next(p + pos, seg - pos, 0);
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        *out = safe_realloc(*out, cap * sizeof **out);
      }
      (*out)[n++] = str_ndup(p + pos, keep);
      pos += keep;
      while (pos < seg && p[pos] == ' ')
        pos++;
    } while (pos < seg);
    p += seg;
    if (*p == '\n')
      p++;
  }
  return n;
}

bool tui_ddl_confirm(TuiState *state, const char *title, const char *message, const char *yes_label,
                     bool destructive) {
  (void)state;
  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);
  int width = screen_w - 4 < 72 ? screen_w - 4 : 72;
  if (width < 24 || screen_h < 8)
    return false;
  char **lines;
  size_t n = wrap_lines(message ? message : "", width - 4, &lines);
  int height = (int)n + 5;
  if (height > screen_h - 2)
    height = screen_h - 2;
  int shown = height - 5;
  WINDOW *dlg = dialog_create(height, width, screen_h, screen_w);
  if (!dlg) {
    for (size_t i = 0; i < n; i++)
      free(lines[i]);
    free(lines);
    return false;
  }
  int focus = 0; /* 0 Cancel, 1 yes */
  bool running = true, yes = false;
  char *ylabel = str_printf("[ %s ]", yes_label && *yes_label ? yes_label : "Yes");
  while (running) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);
    char *t = str_printf(" %s ", title ? title : "Confirm");
    int tw = utf8_width(t, strlen(t));
    if (tw > width - 4)
      tw = width - 4;
    WITH_ATTR(dlg, A_BOLD, tui_print_clip(dlg, 0, (width - tw) / 2, tw, t));
    free(t);
    for (int i = 0; i < shown && (size_t)i < n; i++)
      tui_print_clip(dlg, 2 + i, 2, width - 4, lines[i]);
    int cw = (int)strlen("[ Cancel ]"), yw = utf8_width(ylabel, strlen(ylabel));
    int cx = (width - (cw + 2 + yw)) / 2;
    if (cx < 1)
      cx = 1;
    WITH_ATTR(dlg, focus == 0 ? A_REVERSE : A_NORMAL, mvwprintw(dlg, height - 2, cx, "[ Cancel ]"));
    attr_t ya = focus == 1 ? A_REVERSE | A_BOLD : destructive ? COLOR_PAIR(COLOR_ERROR_TEXT) | A_BOLD : A_NORMAL;
    WITH_ATTR(dlg, ya, tui_print_clip(dlg, height - 2, cx + cw + 2, yw, ylabel));
    wrefresh(dlg);
    int ch = wgetch(dlg);
    switch (ch) {
    case KEY_LEFT: case KEY_RIGHT: case '\t': case KEY_BTAB: focus = !focus; break;
    case 'y': case 'Y': yes = true; running = false; break;
    case 'n': case 'N': case 27: running = false; break;
    case '\n': case KEY_ENTER: yes = focus == 1; running = false; break;
    default: break;
    }
  }
  free(ylabel);
  for (size_t i = 0; i < n; i++)
    free(lines[i]);
  free(lines);
  delwin(dlg);
  touchwin(stdscr);
  return yes;
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

/* Close tab i of workspace w, which is not the current workspace: the
 * tab_ui entries shift as tab_close does for the current one */
static void close_elsewhere(TuiState *state, size_t w, size_t i) {
  Workspace *ws = &state->app->workspaces[w];
  size_t old = ws->num_tabs;
  workspace_close_tab(ws, i);
  if (old > 0 && state->tab_ui && w < state->tab_ui_ws_capacity && state->tab_ui[w] && old <= state->tab_ui_capacity[w]) {
    for (size_t k = i; k < old - 1; k++)
      state->tab_ui[w][k] = state->tab_ui[w][k + 1];
    memset(&state->tab_ui[w][old - 1], 0, sizeof(UITabState));
  }
}

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
        if (w == cur_ws) {
          tab_switch(state, i);
          tab_close(state);
        } else {
          close_elsewhere(state, w, i);
        }
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
