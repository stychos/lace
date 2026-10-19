#include "ui/dialogs/workspaces.h"
#include "conn/connection.h"
#include "model/workspace.h"
#include "ui/dialogs/prompt.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdint.h>
#include <stdlib.h>

enum { ROW_OPEN = 1, ROW_SAVED };

/* The most urgent transaction mark of the workspace's connections */
static const char *tx_mark(const NcWorkspace *w) {
  int worst = NC_TX_NONE;
  for (size_t k = 0; k < w->num_tabs; k++) {
    const NcConnection *c = w->tabs[k]->conn;
    int s = c ? (int)nc_connection_tx_state(c) : NC_TX_NONE;
    int rank = s == NC_TX_LOST ? 3 : s == NC_TX_FAILED ? 2 : s == NC_TX_OPEN ? 1 : 0;
    int have = worst == NC_TX_LOST ? 3 : worst == NC_TX_FAILED ? 2 : worst == NC_TX_OPEN ? 1 : 0;
    if (rank > have) worst = s;
  }
  return worst == NC_TX_LOST ? "  [TX LOST]" : worst == NC_TX_FAILED ? "  [TX FAILED]" : worst == NC_TX_OPEN ? "  [TX]" : "";
}

static void fill(NcApp *app, ListWidget *l) {
  int keep_kind = 0;
  size_t keep_idx = 0;
  ListRow *sel = list_selected(l);
  if (sel) { keep_kind = sel->kind; keep_idx = (size_t)(intptr_t)sel->data; }
  bool first = l->num_rows == 0;
  size_t n = 1 + app->num_workspaces + (app->saved_ws.count ? 1 + app->saved_ws.count : 0), r = 0;
  ListRow *rows = safe_calloc(n, sizeof *rows);
  rows[r++] = (ListRow){str_dup("Open"), NULL, NULL, 0, NULL, true};
  for (size_t i = 0; i < app->num_workspaces; i++) {
    const NcWorkspace *w = app->workspaces[i];
    rows[r++] = (ListRow){str_printf("%zu  %s", i + 1, w->name ? w->name : "Workspace"),
                          str_printf("%s%zu tab%s%s", i == app->current_ws ? "current, " : "", w->num_tabs,
                                     w->num_tabs == 1 ? "" : "s", tx_mark(w)),
                          NULL, ROW_OPEN, (void *)(intptr_t)i, false};
  }
  if (app->saved_ws.count) {
    rows[r++] = (ListRow){str_dup("Saved"), NULL, NULL, 0, NULL, true};
    for (size_t i = 0; i < app->saved_ws.count; i++)
      rows[r++] = (ListRow){str_dup(app->saved_ws.items[i].name), NULL, NULL, ROW_SAVED, (void *)(intptr_t)i, false};
  }
  list_set_rows(l, rows, r);
  if (first) { keep_kind = ROW_OPEN; keep_idx = app->current_ws; }
  for (size_t i = 0; keep_kind && i < l->num_shown; i++) {
    const ListRow *row = &l->rows[l->shown[i]];
    if (row->kind == keep_kind && (size_t)(intptr_t)row->data == keep_idx) l->cursor = i;
  }
}

typedef struct { int kind; size_t idx; } Target;

static void renamed(NcApp *app, char *text, void *data) {
  Target *t = data;
  if (text && *text) {
    if (t->kind == ROW_OPEN) app_rename_workspace(app, t->idx, text);
    else app_rename_saved_workspace(app, t->idx, text);
  }
  free(text);
  free(t);
}

static void save_named(NcApp *app, char *text, void *data) {
  Target *t = data;
  if (text && *text) app_save_workspace_as(app, t->idx, text);
  free(text);
  free(t);
}

static Target *target(const ListRow *r) {
  Target *t = safe_malloc(sizeof *t);
  t->kind = r->kind;
  t->idx = (size_t)(intptr_t)r->data;
  return t;
}

static bool manager_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  ListWidget *l = o->data;
  fill(app, l);
  ListRow *r = list_selected(l);
  int kind = r ? r->kind : 0;
  size_t idx = r ? (size_t)(intptr_t)r->data : 0;
  if (ev->type == UI_EVENT_KEY && r) {
    bool plain = !ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT));
    if (hotkey_matches(app->config, ev, HOTKEY_CONN_RENAME)) {
      const char *name = kind == ROW_OPEN ? app->workspaces[idx]->name : app->saved_ws.items[idx].name;
      prompt_open(app, kind == ROW_OPEN ? "Rename workspace" : "Rename saved workspace", "Name", name, false, renamed,
                  target(r));
      return true;
    }
    if (plain && ev->key.key == 's' && kind == ROW_OPEN) {
      prompt_open(app, "Save workspace", "Name", app->workspaces[idx]->name, false, save_named, target(r));
      return true;
    }
    if (hotkey_matches(app->config, ev, HOTKEY_CONN_DELETE)) {
      if (kind == ROW_OPEN) app_close_workspace(app, idx);
      else app_delete_saved_workspace(app, idx);
      return true;
    }
  }
  if (ev->type == UI_EVENT_KEY && hotkey_matches(app->config, ev, HOTKEY_CONN_NEW)) {
    overlay_pop_this(app, o);
    app_new_workspace(app, NULL, true);
    return true;
  }
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(l, ev, 0, o->rows) : list_key(l, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE && (r = list_selected(l))) {
    kind = r->kind;
    idx = (size_t)(intptr_t)r->data;
    overlay_pop_this(app, o); /* frees the rows */
    if (kind == ROW_OPEN) app_switch_workspace(app, idx);
    else app_open_saved_workspace(app, idx);
  }
  return true;
}

static void manager_draw(Overlay *o, NcApp *app) {
  ListWidget *l = o->data;
  fill(app, l); /* follows renames, closes and saves made from here */
  list_draw(l, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void manager_layout(Overlay *o, NcApp *app) {
  ListWidget *l = o->data;
  o->cols = app->cols - 8 < 70 ? app->cols - 8 : 70;
  int want = (int)(l->num_rows < 4 ? 4 : l->num_rows) + 2;
  o->rows = want < app->rows - 6 ? want : app->rows - 6;
  if (o->rows < 3) o->rows = 3;
}

static void manager_free(void *data) {
  list_free(data);
  free(data);
}

void workspaces_open(NcApp *app) {
  ListWidget *l = safe_calloc(1, sizeof *l);
  list_init(l, false);
  fill(app, l);
  Overlay *o = overlay_new("Workspaces");
  o->footer = "Enter open  r rename  s save  x close/delete  n new  Esc close";
  o->data = l;
  o->free_data = manager_free;
  o->layout = manager_layout;
  o->draw = manager_draw;
  o->key = manager_key;
  overlay_push(app, o);
}
