#include "ui/tabstrip.h"
#include "conn/connection.h"
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

size_t tabstrip_entries(const NcWorkspace *ws, StripEntry **out, size_t *current) {
  *current = SIZE_MAX;
  size_t n = 0;
  for (size_t k = 0; ws && k < ws->num_tabs; k++) n += ws->tabs[k]->num_views ? ws->tabs[k]->num_views : 1;
  StripEntry *e = safe_calloc(n + 1, sizeof *e);
  n = 0;
  for (size_t k = 0; ws && k < ws->num_tabs; k++) {
    const ConnTab *t = ws->tabs[k];
    if (!t->num_views) { /* the tab itself */
      if (k == ws->current) *current = n;
      e[n++] = (StripEntry){k, SIZE_MAX};
      continue;
    }
    for (size_t i = 0; i < t->num_views; i++) {
      if (k == ws->current && i == t->current) *current = n;
      e[n++] = (StripEntry){k, i};
    }
  }
  *out = e;
  return n;
}

static const View *entry_view(const NcWorkspace *ws, const StripEntry *e) {
  return e->view == SIZE_MAX ? NULL : ws->tabs[e->tab]->views[e->view];
}

/* A view's title, or the connection's name for a tab without views */
static const char *entry_label(const NcWorkspace *ws, const StripEntry *e) {
  const View *v = entry_view(ws, e);
  if (v) return view_title(v);
  const ConnTab *t = ws->tabs[e->tab];
  const char *name = t->conn ? nc_connection_name(t->conn) : NULL;
  return name ? name : "(no views)";
}

/* Close what a cell stands for: the view, or the tab that has none */
static void close_entry(NcApp *app, const StripEntry *e) {
  if (e->view == SIZE_MAX) {
    app_close_tab(app, e->tab);
  } else {
    app_switch_tab(app, e->tab);
    app_close_view(app, e->view);
  }
}

void tabstrip_draw(NcApp *app) {
  app->strip_spans.n = app->strip_close_spans.n = 0;
  if (!tabstrip_rows(app)) return;
  NcWorkspace *ws = app_current_workspace(app);
  struct ncplane *p = app->std;
  const Theme *th = &app->theme;
  nc_style(p, th, TC_DIM, TC_STRIPE_BG, 0);
  nc_put(p, 1, 0, app->cols, "");
  StripEntry *e;
  size_t current;
  size_t n = tabstrip_entries(ws, &e, &current);
  if (!n) {
    nc_put(p, 1, 1, app->cols - 1, "No views open: pick a table or press p for a query");
    free(e);
    return;
  }
  int *w = safe_calloc(n, sizeof *w), *x0 = safe_calloc(n, sizeof *x0), *x1 = safe_calloc(n, sizeof *x1);
  for (size_t i = 0; i < n; i++) w[i] = label_width(entry_label(ws, &e[i])) + (i == current ? 2 : 0);
  /* no current view (the current tab has none): the strip stays at its start */
  size_t first = tabstrip_layout(w, n, current == SIZE_MAX ? 0 : current, app->cols, x0, x1);
  if (first) nc_put(p, 1, 0, 1, "\xe2\x80\xb9"); /* ‹ */
  size_t last_shown = first;
  for (size_t i = first; i < n; i++) {
    if (x1[i] == x0[i]) break;
    last_shown = i;
    bool cur = i == current;
    const View *v = entry_view(ws, &e[i]);
    const char *title = entry_label(ws, &e[i]);
    int lw = label_width(title);
    unsigned italic = !v || v->preview ? NCSTYLE_ITALIC : 0; /* the preview tab, or a tab without views */
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
  free(e);
}

bool tabstrip_handle_mouse(NcApp *app, const UiEvent *ev) {
  if (ev->type != UI_EVENT_MOUSE || !tabstrip_rows(app) || ev->mouse.y != 1) return false;
  if (ev->mouse.action != UI_MOUSE_PRESS) return true;
  NcWorkspace *ws = app_current_workspace(app);
  if (!ws) return true;
  int close = hit_find(&app->strip_close_spans, ev->mouse.x);
  int cell = hit_find(&app->strip_spans, ev->mouse.x);
  StripEntry *e;
  size_t current;
  size_t n = tabstrip_entries(ws, &e, &current);
  if (cell >= 0 && (size_t)cell >= n) cell = -1; /* the strip changed since the draw */
  if (close >= 0 && (size_t)close >= n) close = -1;
  if (ev->mouse.button == UI_MOUSE_LEFT && close >= 0) {
    close_entry(app, &e[close]);
  } else if (ev->mouse.button == UI_MOUSE_LEFT && cell >= 0) {
    app_switch_tab(app, e[cell].tab);
    if (e[cell].view != SIZE_MAX) app_switch_view(app, e[cell].view);
  } else if (ev->mouse.button == UI_MOUSE_MIDDLE && (cell >= 0 || close >= 0)) {
    close_entry(app, &e[cell >= 0 ? cell : close]);
  }
  free(e);
  return true;
}
