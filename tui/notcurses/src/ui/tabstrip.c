#include "ui/tabstrip.h"
#include "core/utf8.h"
#include "model/workspace.h"
#include "ui/panels.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

#define STRIP_MIN_ROWS 14 /* below this the strip gives its row to the content */

int tabstrip_rows(const NcApp *app) {
  if (app->rows < STRIP_MIN_ROWS || !prefs_section_bool(app->config, "tabs_strip", true)) return 0;
  const ConnTab *t = app->num_workspaces ? workspace_current_tab(app->workspaces[app->current_ws]) : NULL;
  return t && t->conn ? 1 : 0;
}

size_t tabstrip_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1) {
  for (size_t i = 0; i < n; i++) x0[i] = x1[i] = 0;
  if (!n || width < 4) return 0;
  if (current >= n) current = n - 1;
  size_t first = 0;
  for (;;) {
    /* a scroll mark on the left when tabs are hidden there, one on the right when some follow */
    int x = first ? 2 : 0;
    bool fits = false;
    size_t i;
    for (i = first; i < n; i++) {
      int cell = widths[i] + 2;
      int limit = width - (i + 1 < n ? 2 : 0); /* room for the right mark */
      if (x + cell > limit) break;
      if (i == current) fits = true;
      x += cell + 1;
    }
    if (fits || first == current || first + 1 >= n) break;
    first++;
  }
  int x = first ? 2 : 0;
  for (size_t i = first; i < n; i++) {
    int cell = widths[i] + 2;
    int limit = width - (i + 1 < n ? 2 : 0);
    if (x + cell > limit) break;
    x0[i] = x;
    x1[i] = x + cell;
    x += cell + 1;
  }
  return first;
}

static int label_width(const char *label) {
  int w = utf8_width(label, strlen(label));
  return w > TABSTRIP_LABEL_MAX ? TABSTRIP_LABEL_MAX : w;
}

void tabstrip_draw(NcApp *app) {
  app->strip_spans.n = app->strip_close_spans.n = 0;
  if (!tabstrip_rows(app)) return;
  ConnTab *t = app_current_tab(app);
  struct ncplane *p = app->std;
  const Theme *th = &app->theme;
  nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
  nc_put(p, 1, 0, app->cols, "");
  size_t n = t->num_views;
  if (!n) {
    nc_put(p, 1, 1, app->cols - 1, "No views open: pick a table or press p for a query");
    return;
  }
  int *w = safe_calloc(n, sizeof *w), *x0 = safe_calloc(n, sizeof *x0), *x1 = safe_calloc(n, sizeof *x1);
  for (size_t i = 0; i < n; i++) w[i] = label_width(view_title(t->views[i])) + (i == t->current ? 2 : 0);
  size_t first = tabstrip_layout(w, n, t->current, app->cols, x0, x1);
  if (first) nc_put(p, 1, 0, 1, "\xe2\x80\xb9"); /* ‹ */
  size_t last_shown = first;
  for (size_t i = first; i < n; i++) {
    if (x1[i] == x0[i]) break;
    last_shown = i;
    bool cur = i == t->current;
    const char *title = view_title(t->views[i]);
    int lw = label_width(title);
    unsigned italic = t->views[i]->preview ? NCSTYLE_ITALIC : 0; /* the preview tab */
    if (cur) nc_style(p, th, TC_FG, TC_BG, NCSTYLE_BOLD | italic);
    else nc_style(p, th, TC_DIM, TC_STRIPE_BG, italic);
    nc_put(p, 1, x0[i], x1[i] - x0[i], "");
    nc_put(p, 1, x0[i] + 1, lw, title);
    if (cur) {
      nc_style(p, th, TC_DIM, TC_BG, 0);
      nc_put(p, 1, x0[i] + 1 + lw + 1, 1, "\xc3\x97"); /* × */
      hit_add(&app->strip_close_spans, x0[i] + 1 + lw + 1, x1[i], (int)i);
      hit_add(&app->strip_spans, x0[i], x0[i] + 1 + lw + 1, (int)i);
    } else {
      hit_add(&app->strip_spans, x0[i], x1[i], (int)i);
    }
    if (i + 1 < n && x1[i + 1] > x1[i]) {
      nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
      nc_put(p, 1, x1[i], 1, "\xe2\x94\x82");
    }
  }
  if (last_shown + 1 < n) {
    nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
    nc_put(p, 1, app->cols - 1, 1, "\xe2\x80\xba"); /* › */
  }
  free(w);
  free(x0);
  free(x1);
}

bool tabstrip_handle_mouse(NcApp *app, const UiEvent *ev) {
  if (ev->type != UI_EVENT_MOUSE || !tabstrip_rows(app) || ev->mouse.y != 1) return false;
  if (ev->mouse.action != UI_MOUSE_PRESS) return true;
  ConnTab *t = app_current_tab(app);
  if (!t) return true;
  int close = hit_find(&app->strip_close_spans, ev->mouse.x);
  int tab = hit_find(&app->strip_spans, ev->mouse.x);
  if (ev->mouse.button == UI_MOUSE_LEFT && close >= 0) {
    app_close_view(app, (size_t)close);
  } else if (ev->mouse.button == UI_MOUSE_LEFT && tab >= 0) {
    app_switch_view(app, (size_t)tab);
  } else if (ev->mouse.button == UI_MOUSE_MIDDLE && (tab >= 0 || close >= 0)) {
    app_close_view(app, (size_t)(tab >= 0 ? tab : close));
  }
  return true;
}
