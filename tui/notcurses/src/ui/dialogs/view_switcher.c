#include "conn/connection.h"
#include "model/workspace.h"
#include "ui/form_tab.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
#include "ui/tabstrip.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>

static char *summary(const View *v) {
  if (v->kind == VIEW_QUERY) return str_dup(v->running ? "running" : v->ran ? "ran" : "");
  char *s = str_dup("");
  if (v->num_filters) {
    char *n = str_printf("%s%zu filter%s", *s ? ", " : "", v->num_filters, v->num_filters == 1 ? "" : "s");
    char *j = str_printf("%s%s", s, n);
    free(s); free(n); s = j;
  }
  if (v->num_sorts) {
    char *n = str_printf("%s%zu sort%s", *s ? ", " : "", v->num_sorts, v->num_sorts == 1 ? "" : "s");
    char *j = str_printf("%s%s", s, n);
    free(s); free(n); s = j;
  }
  return s;
}

/* The views of every connection tab in the strip's order; a row's data is
 * its ConnTab, kind the view's index. Returns the row of the current view (or 0). */
static size_t fill(ListWidget *l, const NcWorkspace *ws) {
  StripEntry *e;
  size_t current;
  size_t n = tabstrip_entries(ws, &e, &current);
  ListRow *rows = safe_calloc(n ? n : 1, sizeof *rows);
  for (size_t i = 0; i < n; i++) {
    ConnTab *t = ws->tabs[e[i].tab];
    if (e[i].view == SIZE_MAX) { /* a tab without views: its own row */
      const char *name = t->conn ? nc_connection_name(t->conn) : NULL;
      rows[i].text = t->form ? str_dup(form_tab_label(t->form)) : str_printf("%s (no views)", name ? name : "connection");
      rows[i].kind = -1;
      rows[i].data = t;
      continue;
    }
    const View *v = t->views[e[i].view];
    rows[i].text = v->preview ? str_printf("%s (preview)", view_title(v)) : str_dup(view_title(v));
    rows[i].detail = summary(v);
    rows[i].kind = (int)e[i].view;
    rows[i].data = t;
  }
  free(e);
  list_set_rows(l, rows, n);
  return current == SIZE_MAX ? 0 : current;
}

static size_t tab_index(const NcWorkspace *ws, const ConnTab *t) {
  for (size_t k = 0; k < ws->num_tabs; k++)
    if (ws->tabs[k] == t) return k;
  return SIZE_MAX;
}

static size_t views_open(const NcWorkspace *ws) {
  size_t n = 0;
  for (size_t k = 0; k < ws->num_tabs; k++) n += ws->tabs[k]->num_views;
  return n;
}

static bool switcher_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  ListWidget *l = o->data;
  NcWorkspace *ws = app_current_workspace(app);
  if (!ws || !views_open(ws)) {
    overlay_pop_this(app, o);
    return true;
  }
  if (ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.key == 'x' && !ev->key.mods) {
    ListRow *r = list_selected(l);
    if (r && r->data && r->kind >= 0 && tab_index(ws, r->data) != SIZE_MAX) {
      size_t cur = l->cursor;
      conntab_remove_view(r->data, (size_t)r->kind);
      if (!views_open(ws)) {
        overlay_pop_this(app, o);
        return true;
      }
      fill(l, ws);
      l->cursor = cur < l->num_shown ? cur : l->num_shown - 1;
    }
    return true;
  }
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(l, ev, 0, o->rows) : list_key(l, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(l);
    size_t tab = r && r->data ? tab_index(ws, r->data) : SIZE_MAX;
    bool has_view = r && r->kind >= 0; /* a tab without views has no view to switch to */
    size_t idx = has_view ? (size_t)r->kind : 0;
    overlay_pop_this(app, o); /* frees the rows: nothing of r is read past here */
    if (tab != SIZE_MAX) {
      app_switch_tab(app, tab);
      if (has_view) app_switch_view(app, idx);
    }
  }
  return true;
}

static void switcher_draw(Overlay *o, NcApp *app) {
  list_draw(o->data, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void switcher_layout(Overlay *o, NcApp *app) {
  ListWidget *l = o->data;
  o->cols = app->cols - 8 < 70 ? app->cols - 8 : 70;
  o->rows = (int)l->num_rows < app->rows - 6 ? (int)l->num_rows : app->rows - 6;
  if (o->rows < 3) o->rows = 3;
}

static void switcher_free(void *data) {
  list_free(data);
  free(data);
}

void view_switcher_open(NcApp *app) {
  NcWorkspace *ws = app_current_workspace(app);
  ConnTab *t = app_current_tab(app);
  if (!ws || !t) return;
  if (!views_open(ws)) {
    app_set_status(app, false, "No views open");
    return;
  }
  ListWidget *l = safe_calloc(1, sizeof *l);
  list_init(l, false);
  l->cursor = fill(l, ws);
  char *name = t->conn ? nc_connection_short_name(t->conn) : NULL;
  char *title = ws->num_tabs > 1 ? str_dup("Views") : str_printf("Views in %s", name ? name : "connection");
  free(name);
  Overlay *o = overlay_new(title);
  free(title);
  o->footer = "Enter switch  x close view  Esc cancel";
  o->data = l;
  o->free_data = switcher_free;
  o->draw = switcher_draw;
  o->key = switcher_key;
  o->layout = switcher_layout;
  overlay_push(app, o);
}
