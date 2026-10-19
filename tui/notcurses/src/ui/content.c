#include "app.h"
#include "model/workspace.h"
#include "ui/filter_panel.h"
#include "ui/form_tab.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "ui/query_view.h"
#include "ui/sidebar.h"
#include <notcurses/notcurses.h>

/* The current connection tab: the sidebar on the left (when shown) and the
 * open view's grid or editor, else a hint. */
void content_draw(NcApp *app) {
  ConnTab *t = app_current_tab(app);
  if (t && t->form) {
    Rect c = content_rect(app);
    int cy = 0, cx = 0;
    bool cursor = form_tab_draw(app, t, c.y, c.rows, &cy, &cx);
    if (!app->num_overlays) {
      if (cursor) notcurses_cursor_enable(app->nc, cy, cx);
      else notcurses_cursor_disable(app->nc);
    }
    return;
  }
  if (!t || !t->conn) return;
  int sw = sidebar_width(app);
  Rect c = content_rect(app);
  if (sw) sidebar_draw(app, t, (Rect){c.y, 0, c.rows, sw});
  View *v = conntab_current_view(t);
  int cy = 0, cx = 0;
  bool cursor = false;
  if (v) {
    if (v->kind == VIEW_QUERY) {
      query_view_draw(app, v, grid_content_rect(app));
    } else {
      if (filter_panel_rows(app, v)) filter_panel_draw(app, v, filter_panel_rect(app, v));
      grid_draw(app, v, grid_content_rect(app));
    }
    cursor = grid_edit_cursor(&cy, &cx) || (v->kind == VIEW_QUERY && query_view_cursor(&cy, &cx)) ||
             (v->kind == VIEW_TABLE && v->fp.focused && filter_panel_cursor(&cy, &cx));
  } else {
    nc_style(app->std, &app->theme, TC_DIM, TC_BG, 0);
    int x = sw ? sw + 1 : 1;
    if (x + 2 < app->cols)
      nc_put(app->std, 2, x, app->cols - x - 1, sw ? "Pick a table from the list" : "Press t to show the sidebar");
  }
  if (!app->num_overlays) {
    if (sw && t->sidebar_focused && sidebar_cursor(&cy, &cx)) cursor = true;
    if (cursor) notcurses_cursor_enable(app->nc, cy, cx);
    else notcurses_cursor_disable(app->nc);
  }
}

bool content_handle_key(NcApp *app, const UiEvent *ev) {
  ConnTab *t = app_current_tab(app);
  if (t && t->form) return form_tab_handle_key(app, t, ev);
  if (!t || !t->conn || ev->type != UI_EVENT_KEY) return false;
  View *v = conntab_current_view(t);
  bool panel = v && v->kind == VIEW_TABLE && filter_panel_rows(app, v) > 0;
  if (panel && v->fp.focused && filter_panel_handle_key(app, v, ev)) return true;
  /* Tab / Shift+Tab move the focus around the ring sidebar, filter panel, content */
  if (ev->key.is_special && (ev->key.key == UI_KEY_TAB || ev->key.key == UI_KEY_BTAB) &&
      !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    bool forward = ev->key.key == UI_KEY_TAB;
    if (t->sidebar_focused) {
      t->sidebar_focused = false;
      if (forward && panel) filter_panel_focus(app, v, false);
      return true;
    }
    bool editor = v && v->kind == VIEW_QUERY && !v->focus_results; /* Tab types a tab there */
    if (!editor || !forward) {
      if (panel && v->fp.focused) return true; /* the panel moved the focus itself */
      if (forward ? sidebar_width(app) != 0 : !panel) {
        if (sidebar_width(app)) {
          if (panel) filter_panel_blur(app, v);
          t->sidebar_focused = true;
        }
      } else if (panel) {
        filter_panel_focus(app, v, !forward);
      }
      return true;
    }
  }
  if (sidebar_handle_key(app, t, ev)) return true;
  if (v && v->kind == VIEW_QUERY) return query_view_handle_key(app, v, ev, grid_content_rect(app));
  if (v) return grid_handle_key(app, v, ev, grid_content_rect(app));
  return false;
}

bool content_handle_mouse(NcApp *app, const UiEvent *ev) {
  ConnTab *t = app_current_tab(app);
  if (t && t->form) return form_tab_handle_mouse(app, t, ev);
  if (!t || !t->conn) return false;
  View *v = conntab_current_view(t);
  bool press = ev->type == UI_EVENT_MOUSE && ev->mouse.action == UI_MOUSE_PRESS;
  bool panel = v && v->kind == VIEW_TABLE && filter_panel_rows(app, v) > 0;
  if (sidebar_handle_mouse(app, t, ev)) {
    if (press && panel) filter_panel_blur(app, v);
    return true;
  }
  if (press) t->sidebar_focused = false;
  if (panel && filter_panel_handle_mouse(app, v, ev, filter_panel_rect(app, v))) return true;
  if (press && panel) filter_panel_blur(app, v);
  if (v && v->kind == VIEW_QUERY && !v->editing && query_view_handle_mouse(app, v, ev, grid_content_rect(app)))
    return true;
  return v && grid_handle_mouse(app, v, ev, grid_view_rect(app, v));
}
