/*
 * Lace
 * The command palette (Ctrl+P, :), as the notcurses TUI's (ui/palette.c):
 * one filterable list of the actions that apply where the focus is, the
 * tables, the connection's tabs, the workspaces and saved ones, the saved
 * and recent connections, and the on/off preferences. An action runs
 * through its first key, fed to the main loop's dispatcher, so it does
 * what that key does there.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "palette.h"
#include "connect_open.h"
#include "form_tab.h"
#include "list_draw.h"
#include "overlay.h"
#include "prompt.h"
#include "sort_picker.h"
#include "prefs_dialog.h"
#include "../config/saved_workspaces.h"
#include "../config/session.h"
#include "../core/chrome.h"
#include "../core/fuzzy.h"
#include "../core/prefs_model.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  PAL_ACTION,
  PAL_SORT,
  PAL_TABLE,
  PAL_VIEW,
  PAL_WORKSPACE,
  PAL_NEW_WS,
  PAL_RENAME_WS,
  PAL_CLOSE_WS,
  PAL_SAVE_WS,
  PAL_SAVED_WS,
  PAL_DELETE_SAVED_WS,
  PAL_SAVED_CONN,
  PAL_RECENT,
  PAL_PREF,
} PalKind;

typedef struct {
  ListRow *rows;
  size_t n, cap;
  char **owned; /* strings the rows' data point to */
  size_t num_owned;
} Builder;

static ListRow *add_row(Builder *b, const char *text, const char *detail, char *keys, int kind, void *data) {
  if (b->n == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 64;
    b->rows = safe_realloc(b->rows, b->cap * sizeof *b->rows);
  }
  ListRow *r = &b->rows[b->n++];
  *r = (ListRow){str_dup(text), detail ? str_dup(detail) : NULL, keys, kind, data, false};
  return r;
}

static void add_header(Builder *b, const char *name) { add_row(b, name, NULL, NULL, -1, NULL)->header = true; }

static char *own(Builder *b, const char *s) {
  b->owned = safe_realloc(b->owned, (b->num_owned + 1) * sizeof *b->owned);
  return b->owned[b->num_owned++] = str_dup(s ? s : "");
}

static int row_score(const char *needle, const ListRow *row) { return fuzzy_score(needle, row->text); }

/* A table tab's action on its current sub-view (notcurses'
 * subview_action_applies): the switch keys for the sub-views it has, the
 * table menu from any, the structure keys only in a list, and in a list
 * only the moves and the keys that are not the grid's */
static bool subview_applies(const Tab *t, HotkeyAction a) {
  if (t->type != TAB_TYPE_TABLE)
    return false;
  static const struct {
    HotkeyAction a;
    TabSubView sv;
  } sw[] = {{HOTKEY_SUBVIEW_DATA, SUBVIEW_DATA},
            {HOTKEY_SUBVIEW_STRUCTURE, SUBVIEW_STRUCTURE},
            {HOTKEY_SUBVIEW_INDEXES, SUBVIEW_INDEXES},
            {HOTKEY_SUBVIEW_FOREIGN_KEYS, SUBVIEW_FOREIGN_KEYS},
            {HOTKEY_SUBVIEW_INFORMATION, SUBVIEW_INFORMATION}};
  for (size_t i = 0; i < sizeof sw / sizeof sw[0]; i++)
    if (a == sw[i].a)
      return tab_has_subview(t, sw[i].sv);
  if (a == HOTKEY_TABLE_MENU)
    return true;
  HotkeyCategory cat = hotkey_get_category(a);
  if (cat == HOTKEY_CAT_STRUCTURE)
    return t->subview != SUBVIEW_DATA;
  if (t->subview == SUBVIEW_DATA)
    return true;
  switch (a) {
  case HOTKEY_MOVE_UP:
  case HOTKEY_MOVE_DOWN:
  case HOTKEY_PAGE_UP:
  case HOTKEY_PAGE_DOWN:
  case HOTKEY_FIRST_ROW:
  case HOTKEY_LAST_ROW:
  case HOTKEY_REFRESH:
  case HOTKEY_TOGGLE_SIDEBAR: return true;
  case HOTKEY_TOGGLE_FILTERS:
  case HOTKEY_ADD_FILTER: return tab_has_subview(t, SUBVIEW_DATA);
  default: return cat != HOTKEY_CAT_TABLE && cat != HOTKEY_CAT_NAVIGATION && cat != HOTKEY_CAT_FILTERS;
  }
}

/* Whether an action means something now (notcurses' app_action_applies):
 * decided by what is open, not by where the focus is */
static bool action_applies(TuiState *state, HotkeyAction a) {
  const Tab *t = TUI_TAB(state);
  bool connected = t && t->type != TAB_TYPE_FORM;
  bool view = t && (t->type == TAB_TYPE_TABLE || t->type == TAB_TYPE_QUERY);
  switch (a) {
  case HOTKEY_ADD_FILTER: /* the add-row keys: the panel's only while it has the focus */
    return t && t->type == TAB_TYPE_TABLE && tui_filters_visible(state) && tui_filters_focused(state);
  case HOTKEY_REMOVE_FILTER:
    return t && t->type == TAB_TYPE_TABLE && tui_filters_visible(state) && tui_filters_focused(state) &&
           t->filters.num_filters > 0;
  case HOTKEY_OPEN_QUERY:
  case HOTKEY_SIDEBAR_FILTER:
  case HOTKEY_TOGGLE_SIDEBAR:
  case HOTKEY_TOGGLE_HISTORY:
  case HOTKEY_VIEW_SWITCHER: return connected;
  case HOTKEY_SWITCH_DATABASE: { /* a server's databases; a SQLite file is one */
    Connection *c = connected ? app_get_connection(state->app, t->connection_index) : NULL;
    return c && c->conn && c->conn->driver_type != LACE_DRIVER_SQLITE;
  }
  case HOTKEY_SPLIT_UP:
  case HOTKEY_SPLIT_DOWN: return t && t->type == TAB_TYPE_QUERY;
  case HOTKEY_FOLLOW_FK:
  case HOTKEY_REFERENCED_BY: /* a table's rows: Data, or results that are one table's */
    return t && (t->type == TAB_TYPE_TABLE ? t->subview == SUBVIEW_DATA && tab_has_subview(t, SUBVIEW_DATA)
                                           : t->type == TAB_TYPE_QUERY && t->query_source_table);
  case HOTKEY_EXECUTE_QUERY:
  case HOTKEY_EXECUTE_ALL:
  case HOTKEY_EXECUTE_TRANSACTION: return t && t->type == TAB_TYPE_QUERY;
  case HOTKEY_NEW_TAB: /* the sidebar's highlighted table: its keys are the add-row keys */
    return connected && tui_sidebar_focused(state) && tui_sidebar_shown(state);
  case HOTKEY_PREV_TAB:
  case HOTKEY_NEXT_TAB:
  case HOTKEY_CLOSE_TAB: return t != NULL;
  case HOTKEY_CLEAR_SELECTIONS: return t && t->type == TAB_TYPE_TABLE && t->num_selected > 0;
  default: break;
  }
  switch (hotkey_get_category(a)) {
  case HOTKEY_CAT_TABLE:
  case HOTKEY_CAT_NAVIGATION:
  case HOTKEY_CAT_FILTERS:
  case HOTKEY_CAT_STRUCTURE: return t && subview_applies(t, a);
  case HOTKEY_CAT_QUERY:
  case HOTKEY_CAT_EDITOR: return view;
  default: return true;
  }
}

bool tui_palette_lists_action(TuiState *state, HotkeyAction a) {
  const Config *cfg = state->app->config;
  if (!(hotkey_action_frontends(a) & CONFIG_FE_NCURSES) || !action_applies(state, a))
    return false;
  if (a == HOTKEY_NEXT_CONN_TAB || a == HOTKEY_PREV_CONN_TAB)
    return false; /* the same walk as next/prev_tab */
  if (a >= HOTKEY_CONN_TAB_1 && a <= HOTKEY_CONN_TAB_9) { /* only the tabs that exist */
    Workspace *ws = TUI_WORKSPACE(state);
    size_t n = 0;
    for (size_t i = 0; ws && i < ws->num_tabs; i++)
      n += ws->tabs[i].active;
    if ((size_t)(a - HOTKEY_CONN_TAB_1) >= n)
      return false;
  }
  /* Keys of an editor or a dialog, which only mean something there */
  HotkeyCategory cat = hotkey_get_category(a);
  if (a == HOTKEY_ROW_SAVE || a == HOTKEY_CONFIG_RESET || a == HOTKEY_CONFIG_RESET_ALL ||
      cat == HOTKEY_CAT_EDITOR || cat == HOTKEY_CAT_HISTORY || cat == HOTKEY_CAT_CONNECT)
    return false;
  return cfg->hotkeys[a].num_keys > 0;
}

typedef struct {
  Builder *b;
} ConnCtx;

static void add_conn(ConnectionItem *item, void *ctx) {
  Builder *b = ((ConnCtx *)ctx)->b;
  if (!connmgr_is_connection(item))
    return;
  const SavedConnection *c = &item->connection;
  const char *path[16];
  int depth = 0;
  for (ConnectionItem *f = item->parent; f && f->parent && depth < 16; f = f->parent)
    path[depth++] = f->folder.name;
  char *detail = NULL;
  for (int i = depth - 1; i >= 0; i--) {
    char *next = detail ? str_printf("%s/%s", detail, path[i]) : str_dup(path[i]);
    free(detail);
    detail = next;
  }
  char *text = str_printf("Connect to %s", c->name ? c->name : "(unnamed)");
  add_row(b, text, detail, NULL, PAL_SAVED_CONN, own(b, c->id));
  free(text);
  free(detail);
}

/* What a tab is called in the lists */
static char *tab_name(const Tab *tab) {
  if (tab->type == TAB_TYPE_FORM)
    return form_tab_label(tab);
  if (tab->type == TAB_TYPE_QUERY) { /* as the strip names it */
    char buf[256];
    return str_dup(chrome_query_title(tab->query_text, buf, sizeof buf));
  }
  return str_dup(tab->table_name ? tab->table_name : "?");
}

static void build(TuiState *state, Builder *b, SavedWorkspaces *saved) {
  AppState *app = state->app;
  add_header(b, "Actions");
  for (int a = 0; a < HOTKEY_COUNT; a++) {
    if (!tui_palette_lists_action(state, (HotkeyAction)a))
      continue;
    add_row(b, hotkey_action_name((HotkeyAction)a), NULL, hotkey_get_display(app->config, (HotkeyAction)a),
            PAL_ACTION, (void *)(intptr_t)a);
  }
  Tab *cur = TUI_TAB(state);
  if (cur && cur->type == TAB_TYPE_TABLE && tab_has_subview(cur, SUBVIEW_DATA))
    add_row(b, "Sort by column\xe2\x80\xa6", NULL, NULL, PAL_SORT, NULL);
  if (cur && cur->type != TAB_TYPE_FORM && TUI_NUM_TABLES(state) > 0) {
    add_header(b, "Tables");
    char **tables = TUI_TABLES(state);
    for (size_t i = 0; i < TUI_NUM_TABLES(state); i++) {
      if (!tables[i])
        continue;
      char *text = str_printf("Open table %s", tables[i]);
      add_row(b, text, NULL, NULL, PAL_TABLE, (void *)(intptr_t)i);
      free(text);
    }
  }
  Workspace *ws = TUI_WORKSPACE(state);
  if (ws && cur && cur->type != TAB_TYPE_FORM) {
    bool header = false;
    for (size_t i = 0; i < ws->num_tabs; i++) {
      Tab *t = &ws->tabs[i];
      if (!t->active || (t->type != TAB_TYPE_TABLE && t->type != TAB_TYPE_QUERY) ||
          t->connection_index != cur->connection_index)
        continue;
      if (!header)
        add_header(b, "Views");
      header = true;
      char *name = tab_name(t);
      add_row(b, name, NULL, NULL, PAL_VIEW, (void *)(intptr_t)i);
      free(name);
    }
  }
  add_header(b, "Workspaces");
  for (size_t i = 0; i < app->num_workspaces; i++) {
    const Workspace *w = &app->workspaces[i];
    if (!w->active)
      continue;
    char *text = w->name[0] ? str_printf("Switch to %s", w->name) : str_printf("Switch to workspace %zu", i + 1);
    add_row(b, text, NULL, NULL, PAL_WORKSPACE, (void *)(intptr_t)i);
    free(text);
  }
  add_row(b, "New workspace", NULL, NULL, PAL_NEW_WS, NULL);
  add_row(b, "Rename workspace", NULL, NULL, PAL_RENAME_WS, NULL);
  add_row(b, "Close workspace", NULL, NULL, PAL_CLOSE_WS, NULL);
  add_row(b, "Save workspace as\xe2\x80\xa6", NULL, NULL, PAL_SAVE_WS, NULL);
  for (size_t i = 0; i < saved->count; i++) {
    char *text = str_printf("Open saved workspace %s", saved->items[i].name);
    add_row(b, text, NULL, NULL, PAL_SAVED_WS, own(b, saved->items[i].name));
    free(text);
    text = str_printf("Delete saved workspace %s", saved->items[i].name);
    add_row(b, text, NULL, NULL, PAL_DELETE_SAVED_WS, own(b, saved->items[i].name));
    free(text);
  }
  add_header(b, "Connect");
  ConnCtx cc = {b};
  if (tui_connmgr(state))
    connmgr_for_each_connection(tui_connmgr(state), add_conn, &cc);
  RecentList *recent = tui_recent(state);
  for (size_t i = 0; recent && i < recent->count; i++) {
    char *text = str_printf("Connect to %s", recent->items[i].url);
    add_row(b, text, NULL, NULL, PAL_RECENT, own(b, recent->items[i].url));
    free(text);
  }
  add_header(b, "Preferences"); /* the on/off settings of the preferences */
  for (int i = 0; i < prefs_model_count(); i++) {
    if (!prefs_model_for(i, CONFIG_FE_NCURSES) || !prefs_model_is_toggle(i))
      continue;
    char *text = prefs_model_toggle_text(app->config, i, "ncurses");
    add_row(b, text, NULL, NULL, PAL_PREF, (void *)(intptr_t)i);
    free(text);
  }
}

/* Whether another action of this frontend takes the key where the focus is */
static bool key_taken_here(const Config *cfg, HotkeyAction a, const UiEvent *ev, unsigned place) {
  for (int b = 0; b < HOTKEY_COUNT; b++) {
    if (b == (int)a || !(hotkey_action_frontends((HotkeyAction)b) & CONFIG_FE_NCURSES) ||
        !(hotkey_action_contexts((HotkeyAction)b) & place))
      continue;
    if (hotkey_matches(cfg, ev, (HotkeyAction)b))
      return true;
  }
  return false;
}

/* An action through one of its keys, as if it was pressed (a dialog's
 * handler directly). The palette lists actions by what is open, not where
 * the focus is, so the focus goes where the key means that action first: a
 * grid key leaves the sidebar and the filter panel, and a plain key leaves
 * an editor or a field (it would be typed there) */
void tui_run_action(TuiState *state, HotkeyAction a) {
  if (tui_run_dialog_action(state, a))
    return;
  const Config *cfg = state->app->config;
  if (cfg->hotkeys[a].num_keys == 0)
    return;
  Tab *t = TUI_TAB(state);
  HotkeyCategory cat = hotkey_get_category(a);
  bool grid_key = cat == HOTKEY_CAT_TABLE || cat == HOTKEY_CAT_NAVIGATION || cat == HOTKEY_CAT_STRUCTURE ||
                  a == HOTKEY_FOLLOW_FK || a == HOTKEY_REFERENCED_BY;
  if (grid_key && t && (t->type == TAB_TYPE_TABLE || t->type == TAB_TYPE_QUERY)) {
    state->sidebar_filter_active = false;
    if (tui_sidebar_focused(state))
      tui_set_sidebar_focused(state, false);
    if (cat != HOTKEY_CAT_FILTERS && tui_filters_focused(state)) {
      state->filters_editing = false;
      tui_set_filters_focused(state, false);
    }
  }
  /* The first key that means this action here (Ctrl+T is "new connection"
   * on a table but "run in a transaction" in a query tab) */
  unsigned place = tui_focus_context(state);
  UiEvent ev;
  bool found = false;
  for (size_t k = 0; k < cfg->hotkeys[a].num_keys && !found; k++)
    found = hotkey_key_event(cfg->hotkeys[a].keys[k], &ev) && !key_taken_here(cfg, a, &ev, place);
  if (!found && !hotkey_key_event(cfg->hotkeys[a].keys[0], &ev))
    return;
  bool plain = !ev.key.is_special && !(ev.key.mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SUPER));
  if (plain) {
    UITabState *ui = TUI_TAB_UI(state);
    if (place == HOTKEY_CTX_EDITOR && ui)
      ui->query_unfocused = true;
    if (place == HOTKEY_CTX_SIDEBAR)
      state->sidebar_filter_active = false;
    if (place == HOTKEY_CTX_FILTERS)
      state->filters_editing = false;
  }
  tui_dispatch_event(state, &ev);
}

static void toggle_pref(TuiState *state, int i) {
  if (!tui_prefs_change(state, i, 1))
    return;
  char *text = prefs_model_toggle_text(state->app->config, i, "ncurses");
  tui_set_status(state, "%s", text);
  free(text);
}

static char *ask(const char *title, const char *initial) {
  PromptOptions opts = {.title = title, .label = "Name", .initial = initial};
  return tui_prompt(&opts);
}

static void dispatch(TuiState *state, int kind, intptr_t idx, const char *str) {
  AppState *app = state->app;
  switch (kind) {
  case PAL_ACTION: tui_run_action(state, (HotkeyAction)idx); break;
  case PAL_PREF: toggle_pref(state, (int)idx); break;
  case PAL_SORT: tui_sort_picker_open(state); break;
  case PAL_TABLE: sidebar_open_table(state, (size_t)idx); break;
  case PAL_VIEW: tab_switch(state, (size_t)idx); break;
  case PAL_WORKSPACE: tui_workspace_switch(state, (size_t)idx); break;
  case PAL_NEW_WS: tui_new_workspace_form(state); break;
  case PAL_RENAME_WS: {
    Workspace *ws = TUI_WORKSPACE(state);
    if (!ws)
      break;
    char *name = ask("Rename workspace", ws->name);
    if (name && *name)
      snprintf(ws->name, sizeof ws->name, "%s", name);
    free(name);
    break;
  }
  case PAL_CLOSE_WS: {
    size_t i = app->current_workspace;
    char *q = tui_workspace_close_question(state, i);
    if (!q || tui_show_confirm_dialog(state, q))
      tui_workspace_close_at(state, i);
    free(q);
    break;
  }
  case PAL_SAVE_WS: {
    Workspace *ws = TUI_WORKSPACE(state);
    char *name = ws ? ask("Save workspace", ws->name) : NULL;
    if (!name || !*name) {
      free(name);
      break;
    }
    size_t skipped = 0;
    cJSON *json = session_workspace_to_json(state, app->current_workspace, &skipped);
    if (!json) {
      tui_set_error(state, "Its tabs use connections that are not saved: save the connection first");
      free(name);
      break;
    }
    SavedWorkspaces sw = {0};
    saved_workspaces_load(&sw);
    saved_workspaces_put(&sw, name, json);
    char *err = NULL;
    if (saved_workspaces_save(&sw, &err)) {
      snprintf(ws->name, sizeof ws->name, "%s", name);
      tui_set_status(state, "Saved \"%s\"", name);
    } else {
      tui_set_error(state, "workspaces.json: %s", err ? err : "cannot save");
    }
    free(err);
    saved_workspaces_free(&sw);
    free(name);
    break;
  }
  case PAL_SAVED_WS:
  case PAL_DELETE_SAVED_WS: {
    SavedWorkspaces sw = {0};
    saved_workspaces_load(&sw);
    size_t i = saved_workspaces_find(&sw, str);
    char *err = NULL;
    if (i != SIZE_MAX && kind == PAL_SAVED_WS) {
      if (!session_open_workspace_json(state, sw.items[i].workspace, sw.items[i].name, &err))
        tui_set_error(state, "Could not open \"%s\": %s", str, err ? err : "unknown error");
    } else if (i != SIZE_MAX) {
      saved_workspaces_remove(&sw, i);
      if (saved_workspaces_save(&sw, &err))
        tui_set_status(state, "Deleted \"%s\"", str);
      else
        tui_set_error(state, "workspaces.json: %s", err ? err : "cannot save");
    }
    free(err);
    saved_workspaces_free(&sw);
    break;
  }
  case PAL_SAVED_CONN:
  case PAL_RECENT: {
    SavedConnection *model = NULL;
    char *connstr = NULL;
    const char *saved_id = NULL;
    bool own_model = false;
    if (kind == PAL_SAVED_CONN) {
      ConnectionItem *it = connmgr_find_by_id(tui_connmgr(state), str);
      if (!it || !connmgr_is_connection(it))
        break;
      model = &it->connection;
      saved_id = it->connection.id;
      connstr = tui_saved_connstr(model);
    } else {
      model = connmgr_parse_connstr(str, NULL);
      own_model = model != NULL;
      connstr = str_dup(str);
    }
    char *msg = NULL;
    bool cancelled = false;
    if (connstr && !tui_connect_and_place(state, model, saved_id, connstr, PLACE_WORKSPACE, app->current_workspace,
                                          0, &msg, &cancelled) &&
        msg)
      tui_set_error(state, "%s", msg);
    free(msg);
    str_secure_free(connstr);
    if (own_model) {
      connmgr_free_connection(model);
      free(model);
    }
    break;
  }
  default: break;
  }
}

void tui_palette_open(TuiState *state) {
  if (!state)
    return;
  Builder b = {0};
  SavedWorkspaces saved = {0};
  saved_workspaces_load(&saved);
  build(state, &b, &saved);
  saved_workspaces_free(&saved);
  ListWidget list;
  list_init(&list, true);
  list.score = row_score;
  list_set_rows(&list, b.rows, b.n);

  int sr, sc;
  getmaxyx(stdscr, sr, sc);
  TuiOverlay o;
  if (!tui_overlay_open(&o, "Command palette", "Enter run  Esc close  \xe2\x86\x91\xe2\x86\x93 move",
                        sr - 6 < 24 ? sr - 6 : 24, sc - 8 < 90 ? sc - 8 : 90, -1, -1)) {
    list_free(&list);
    for (size_t i = 0; i < b.num_owned; i++)
      free(b.owned[i]);
    free(b.owned);
    return;
  }
  int picked_kind = -1;
  intptr_t picked_idx = 0;
  char *picked_str = NULL;
  ListRow *r = tui_list_overlay_run(&o, &list, NULL, NULL);
  if (r) {
    picked_kind = r->kind;
    if (r->kind == PAL_ACTION || r->kind == PAL_TABLE || r->kind == PAL_VIEW || r->kind == PAL_WORKSPACE ||
        r->kind == PAL_PREF)
      picked_idx = (intptr_t)r->data;
    else if (r->data)
      picked_str = str_dup(r->data);
  }
  tui_overlay_close(&o);
  list_free(&list);
  for (size_t i = 0; i < b.num_owned; i++)
    free(b.owned[i]);
  free(b.owned);
  tui_recreate_windows(state);
  tui_refresh(state);
  if (picked_kind >= 0) {
    dispatch(state, picked_kind, picked_idx, picked_str);
    tui_refresh(state);
  }
  free(picked_str);
}
