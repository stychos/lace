#include "screen.h"
#include "ui/overlay.h"
#include "ui/tabstrip.h"
#include "util/mem.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

bool screen_init(NcApp *app, char **error) {
  notcurses_options o = {.flags = NCOPTION_SUPPRESS_BANNERS};
  app->nc = notcurses_core_init(&o, NULL);
  if (!app->nc) {
    if (error) *error = strdup("cannot initialize notcurses (is this a terminal?)");
    return false;
  }
  app->std = notcurses_stdplane(app->nc);
  unsigned r = 0, c = 0;
  ncplane_dim_yx(app->std, &r, &c);
  app->rows = (int)r;
  app->cols = (int)c;
  screen_apply_theme(app);
  notcurses_mice_enable(app->nc, NCMICE_BUTTON_EVENT | NCMICE_DRAG_EVENT);
  return true;
}

void screen_apply_theme(NcApp *app) {
  uint32_t bg = 0;
  bool dark = !(notcurses_default_background(app->nc, &bg) == 0 &&
                ((bg >> 16 & 0xff) + (bg >> 8 & 0xff) + (bg & 0xff)) / 3 > 127);
  theme_init(&app->theme, app->config ? app->config->general.color_scheme : 0, dark);
  uint64_t ch = 0;
  ncchannels_set_fg_rgb(&ch, app->theme.rgb[TC_FG]);
  ncchannels_set_bg_rgb(&ch, app->theme.rgb[TC_BG]);
  ncplane_set_base(app->std, " ", 0, ch);
}

void screen_stop(NcApp *app) {
  if (app->nc) notcurses_stop(app->nc);
  app->nc = NULL;
  app->std = NULL;
}

void hit_add(HitSpans *s, int x0, int x1, int value) {
  if (x1 <= x0) return;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 16;
    s->v = safe_realloc(s->v, s->cap * sizeof *s->v);
  }
  s->v[s->n++] = (HitSpan){x0, x1, value};
}

int hit_find(const HitSpans *s, int x) {
  for (size_t i = 0; i < s->n; i++)
    if (x >= s->v[i].x0 && x < s->v[i].x1) return s->v[i].value;
  return -1;
}

bool screen_too_small(const NcApp *app) { return app->rows < MIN_ROWS || app->cols < MIN_COLS; }

/* Below the floor nothing else is drawn (and no click lands on anything) */
static void draw_too_small(NcApp *app) {
  nc_style(app->std, &app->theme, TC_FG, TC_BG, 0);
  const char *msg = "Terminal too small";
  int w = nc_text_width(msg);
  if (w > app->cols) w = app->cols;
  nc_put(app->std, app->rows / 2, (app->cols - w) / 2, w, msg);
  if (app->status && app->rows > 2) { /* e.g. why q does not quit */
    nc_style(app->std, &app->theme, app->status_error ? TC_ERROR_FG : TC_DIM, TC_BG, 0);
    nc_put(app->std, app->rows / 2 + 1, 0, app->cols, app->status);
  }
  notcurses_cursor_disable(app->nc);
}

void screen_render(NcApp *app) {
  ncplane_erase(app->std);
  app->header_spans.n = app->status_spans.n = app->strip_spans.n = app->strip_close_spans.n = 0;
  if (screen_too_small(app)) {
    draw_too_small(app);
    overlay_hide_all(app);
    notcurses_render(app->nc);
    return;
  }
  header_draw(app);
  tabstrip_draw(app);
  if (app_current_tab(app)) content_draw(app);
  else welcome_draw(app);
  status_draw(app);
  overlay_render_all(app);
  notcurses_render(app->nc);
}

void screen_resized(NcApp *app) {
  unsigned r = 0, c = 0;
  notcurses_refresh(app->nc, &r, &c);
  app->rows = (int)r;
  app->cols = (int)c;
  if (!screen_too_small(app)) overlay_layout_all(app); /* done once it grows back */
}

int screen_rows(const NcApp *app) { return app->rows; }
int screen_cols(const NcApp *app) { return app->cols; }
