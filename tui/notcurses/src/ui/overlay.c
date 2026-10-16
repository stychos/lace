#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

Overlay *overlay_new(const char *title) {
  Overlay *o = safe_calloc(1, sizeof *o);
  o->title = str_dup(title ? title : "");
  o->y = o->x = -1;
  return o;
}

static void apply_layout(Overlay *o, NcApp *app) {
  if (o->layout) o->layout(o, app);
  int max_rows = app->rows - 5, max_cols = app->cols - 4; /* the shadow below must not cover the status line */
  if (max_rows < 1) max_rows = 1;
  if (max_cols < 1) max_cols = 1;
  if (o->rows > max_rows) o->rows = max_rows;
  if (o->cols > max_cols) o->cols = max_cols;
  if (o->rows < 1) o->rows = 1;
  if (o->cols < 1) o->cols = 1;
  o->fy = o->y >= 0 ? o->y : (app->rows - (o->rows + 2)) / 2;
  o->fx = o->x >= 0 ? o->x : (app->cols - (o->cols + 2)) / 2;
  if (o->fy < 0) o->fy = 0;
  if (o->fx < 0) o->fx = 0;
}

static struct ncplane *make_plane(NcApp *app, const Overlay *o, int dy, int dx) {
  struct ncplane_options opts = {.y = o->fy + dy, .x = o->fx + dx,
                                 .rows = (unsigned)(o->rows + 2), .cols = (unsigned)(o->cols + 2)};
  return ncplane_create(app->std, &opts);
}

void overlay_push(NcApp *app, Overlay *o) {
  apply_layout(o, app);
  o->shadow = make_plane(app, o, 1, 2);
  o->plane = make_plane(app, o, 0, 0);
  if (o->shadow) {
    uint64_t ch = 0;
    ncchannels_set_bg_rgb(&ch, app->theme.rgb[TC_SHADOW_BG]);
    ncchannels_set_fg_rgb(&ch, app->theme.rgb[TC_SHADOW_BG]);
    ncplane_set_base(o->shadow, " ", 0, ch);
  }
  if (o->plane) {
    uint64_t ch = 0;
    ncchannels_set_bg_rgb(&ch, app->theme.rgb[TC_OVERLAY_BG]);
    ncchannels_set_fg_rgb(&ch, app->theme.rgb[TC_OVERLAY_FG]);
    ncplane_set_base(o->plane, " ", 0, ch);
  }
  app->overlays = safe_realloc(app->overlays, (app->num_overlays + 1) * sizeof *app->overlays);
  app->overlays[app->num_overlays++] = o;
}

static void destroy(Overlay *o) {
  if (o->plane) ncplane_destroy(o->plane);
  if (o->shadow) ncplane_destroy(o->shadow);
  o->plane = o->shadow = NULL;
  if (o->free_data) o->free_data(o->data);
  free(o->title);
  free(o);
}

void overlay_pop_this(NcApp *app, Overlay *o) {
  size_t i = 0;
  while (i < app->num_overlays && app->overlays[i] != o) i++;
  if (i == app->num_overlays) return;
  memmove(app->overlays + i, app->overlays + i + 1, (app->num_overlays - i - 1) * sizeof *app->overlays);
  app->num_overlays--;
  if (o->closed) o->closed(o, app);
  destroy(o);
  if (app->nc) notcurses_cursor_disable(app->nc);
}

void overlay_pop(NcApp *app) {
  if (app->num_overlays) overlay_pop_this(app, app->overlays[app->num_overlays - 1]);
}

Overlay *overlay_top(NcApp *app) {
  return app->num_overlays ? app->overlays[app->num_overlays - 1] : NULL;
}

void overlay_free_all(NcApp *app) {
  while (app->num_overlays) {
    Overlay *o = app->overlays[--app->num_overlays];
    o->plane = o->shadow = NULL; /* gone with notcurses */
    if (o->closed) o->closed(o, app);
    destroy(o);
  }
  free(app->overlays);
  app->overlays = NULL;
}

/* Move every overlay's planes off the screen (they would show over the "too small" notice) */
void overlay_hide_all(NcApp *app) {
  for (size_t i = 0; i < app->num_overlays; i++) {
    /* off the screen: an erased plane still has its opaque base cell */
    if (app->overlays[i]->shadow) ncplane_move_yx(app->overlays[i]->shadow, app->rows, app->cols);
    if (app->overlays[i]->plane) ncplane_move_yx(app->overlays[i]->plane, app->rows, app->cols);
  }
}

void overlay_layout_all(NcApp *app) {
  for (size_t i = 0; i < app->num_overlays; i++) {
    Overlay *o = app->overlays[i];
    apply_layout(o, app);
    if (o->shadow) {
      ncplane_move_yx(o->shadow, o->fy + 1, o->fx + 2);
      ncplane_resize_simple(o->shadow, (unsigned)(o->rows + 2), (unsigned)(o->cols + 2));
    }
    if (o->plane) {
      ncplane_move_yx(o->plane, o->fy, o->fx);
      ncplane_resize_simple(o->plane, (unsigned)(o->rows + 2), (unsigned)(o->cols + 2));
    }
  }
}

static void render_one(NcApp *app, Overlay *o) {
  struct ncplane *p = o->plane;
  if (!p) return;
  const Theme *t = &app->theme;
  ncplane_erase(p);
  uint64_t ch = 0;
  ncchannels_set_fg_rgb(&ch, t->rgb[TC_OVERLAY_BORDER]);
  ncchannels_set_bg_rgb(&ch, t->rgb[TC_OVERLAY_BG]);
  nc_style(p, t, TC_OVERLAY_BORDER, TC_OVERLAY_BG, 0);
  ncplane_cursor_move_yx(p, 0, 0);
  ncplane_perimeter_rounded(p, 0, ch, 0);
  int w = o->cols + 2;
  if (o->title && *o->title) {
    nc_style(p, t, TC_ACCENT, TC_OVERLAY_BG, NCSTYLE_BOLD);
    char *label = str_printf(" %s ", o->title);
    { int lw = nc_text_width(label); nc_put(p, 0, 2, lw < w - 4 ? lw : w - 4, label); }
    free(label);
  }
  if (o->footer && *o->footer) {
    nc_style(p, t, TC_KEY_FG, TC_OVERLAY_BG, 0);
    char *label = str_printf(" %s ", o->footer);
    { int lw = nc_text_width(label); nc_put(p, o->rows + 1, 2, lw < w - 4 ? lw : w - 4, label); }
    free(label);
  }
  nc_style(p, t, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
  if (o->draw) o->draw(o, app);
}

void overlay_render_all(NcApp *app) {
  if (!app->num_overlays) return;
  for (size_t i = 0; i < app->num_overlays; i++) render_one(app, app->overlays[i]);
  Overlay *top = overlay_top(app);
  if (top->wants_cursor)
    notcurses_cursor_enable(app->nc, top->fy + 1 + top->cursor_y, top->fx + 1 + top->cursor_x);
  else
    notcurses_cursor_disable(app->nc);
}

bool overlay_handle_event(NcApp *app, const UiEvent *ev) {
  Overlay *top = overlay_top(app);
  if (!top) return false;
  if (ev->type == UI_EVENT_KEY) {
    if (top->key && top->key(top, app, ev)) return true;
    if (ev->key.is_special && ev->key.key == UI_KEY_ESCAPE) overlay_pop_this(app, top);
    return true;
  }
  if (ev->type == UI_EVENT_MOUSE) {
    int mx = ev->mouse.x, my = ev->mouse.y;
    bool inside = mx >= top->fx && mx < top->fx + top->cols + 2 && my >= top->fy &&
                  my < top->fy + top->rows + 2;
    if (!inside) {
      if (ev->mouse.action == UI_MOUSE_PRESS && ev->mouse.button != UI_MOUSE_SCROLL_UP &&
          ev->mouse.button != UI_MOUSE_SCROLL_DOWN)
        overlay_pop_this(app, top);
      return true;
    }
    if (top->key) {
      UiEvent c = *ev;
      c.mouse.x = mx - top->fx - 1;
      c.mouse.y = my - top->fy - 1;
      top->key(top, app, &c);
    }
    return true;
  }
  return false;
}
