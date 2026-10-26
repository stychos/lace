#include "app.h"
#include "model/workspace.h"
#include "ui/filter_panel.h"
#include "ui/form_tab.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "ui/query_view.h"
#include "ui/sidebar.h"
#include "ui/subviews.h"
#include "core/utf8.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

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
      subviews_draw(app, v); /* the strip over the view's area */
      if (v->subview == SV_DATA) {
        if (filter_panel_rows(app, v)) filter_panel_draw(app, v, filter_panel_rect(app, v));
        grid_draw(app, v, grid_content_rect(app));
      } else {
        grid_edit_begin_frame();
        subview_draw(app, v, grid_content_rect(app));
      }
    }
    cursor = grid_edit_cursor(&cy, &cx) || (v->kind == VIEW_QUERY && query_view_cursor(&cy, &cx)) ||
             (v->kind == VIEW_TABLE && v->fp.focused && filter_panel_cursor(&cy, &cx));
  } else {
    nc_style(app->std, &app->theme, TC_DIM, TC_BG, 0);
    int x = sw ? sw + 1 : 1;
    if (x + 2 < app->cols)
      nc_put(app->std, 2, x, app->cols - x - 1, sw ? "Pick a table from the list" : "Press t to show the sidebar");
  }
  if (content_banner_draw(app, t)) cursor = false; /* the content is not driven meanwhile */
  if (!app->num_overlays) {
    if (sw && t->sidebar_focused && sidebar_cursor(&cy, &cx)) cursor = true;
    if (cursor) notcurses_cursor_enable(app->nc, cy, cx);
    else notcurses_cursor_disable(app->nc);
  }
}

char *content_banner_text(const char *what, int64_t elapsed_ms, int width) {
  char *first = str_dup(what ? what : "");
  char *nl = strpbrk(first, "\r\n");
  if (nl) *nl = '\0';
  for (char *c = first; *c; c++)
    if (*c == '\t') *c = ' ';
  char *tail = elapsed_ms >= 60000 ? str_printf("  %lld:%02lld  Esc cancels", (long long)(elapsed_ms / 60000), (long long)(elapsed_ms / 1000 % 60))
                                   : str_printf("  %lld.%lld s  Esc cancels", (long long)(elapsed_ms / 1000), (long long)(elapsed_ms / 100 % 10));
  int room = width - nc_text_width(tail) - 2; /* the spinner and a space */
  if (room < 4) room = 4;
  if (nc_text_width(first) > room) {
    size_t keep = utf8_fit(first, strlen(first), room - 1, NULL);
    first[keep] = '\0';
    char *cut = str_printf("%s\xe2\x80\xa6", first);
    free(first);
    first = cut;
  }
  char *out = str_printf("%s%s", first, tail);
  free(first);
  free(tail);
  return out;
}

bool content_banner_draw(NcApp *app, ConnTab *t) {
  const char *what = NULL;
  int64_t since = 0;
  if (!t || !t->conn || !nc_connection_user_busy(t->conn, &what, &since)) return false;
  Rect c = content_rect(app);
  int sw = sidebar_width(app);
  int x = sw, w = c.cols - sw;
  if (w < 8 || c.rows < 1) return true;
  char *text = content_banner_text(what, app_now_ms() - since, w - 2);
  nc_style(app->std, &app->theme, TC_CURSOR_FG, TC_CURSOR_BG, NCSTYLE_BOLD);
  nc_put(app->std, c.y, x, w, "");
  char spin[2] = {"|/-\\"[app->spinner % 4], 0};
  nc_put(app->std, c.y, x + 1, 1, spin);
  nc_put(app->std, c.y, x + 3, w - 3, text);
  free(text);
  return true;
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
  if (v && v->subview != SV_DATA) return subview_handle_key(app, v, ev, grid_content_rect(app)); /* a list, not the grid */
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
  if (v && v->kind == VIEW_TABLE && subviews_handle_mouse(app, v, ev, grid_content_rect(app))) { /* the strip, a list */
    if (press && panel) filter_panel_blur(app, v);
    return true;
  }
  if (panel && filter_panel_handle_mouse(app, v, ev, filter_panel_rect(app, v))) return true;
  if (press && panel) filter_panel_blur(app, v);
  if (v && v->kind == VIEW_QUERY && !v->editing && query_view_handle_mouse(app, v, ev, grid_content_rect(app)))
    return true;
  return v && grid_handle_mouse(app, v, ev, grid_view_rect(app, v));
}
