#include "model/workspace.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
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

static void fill(ListWidget *l, ConnTab *t) {
  ListRow *rows = safe_calloc(t->num_views ? t->num_views : 1, sizeof *rows);
  for (size_t i = 0; i < t->num_views; i++) {
    rows[i].text = t->views[i]->preview ? str_printf("%s (preview)", view_title(t->views[i]))
                                         : str_dup(view_title(t->views[i]));
    rows[i].detail = summary(t->views[i]);
    rows[i].kind = (int)i;
  }
  list_set_rows(l, rows, t->num_views);
}

static bool switcher_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  ListWidget *l = o->data;
  ConnTab *t = app_current_tab(app);
  if (!t) {
    overlay_pop_this(app, o);
    return true;
  }
  if (ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.key == 'x' && !ev->key.mods) {
    ListRow *r = list_selected(l);
    if (r) {
      size_t cur = l->cursor;
      conntab_remove_view(t, (size_t)r->kind);
      if (!t->num_views) {
        overlay_pop_this(app, o);
        return true;
      }
      fill(l, t);
      l->cursor = cur < t->num_views ? cur : t->num_views - 1;
    }
    return true;
  }
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(l, ev, 0, o->rows) : list_key(l, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(l);
    size_t idx = r ? (size_t)r->kind : 0;
    overlay_pop_this(app, o);
    app_switch_view(app, idx);
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
  ConnTab *t = app_current_tab(app);
  if (!t) return;
  if (!t->num_views) {
    app_set_status(app, false, "No views open");
    return;
  }
  ListWidget *l = safe_calloc(1, sizeof *l);
  list_init(l, false);
  fill(l, t);
  l->cursor = t->current < t->num_views ? t->current : 0;
  char *name = t->conn ? nc_connection_short_name(t->conn) : NULL;
  char *title = str_printf("Views in %s", name ? name : "connection");
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
