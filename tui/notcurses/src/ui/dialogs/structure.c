#include "app.h"
#include "core/utf8.h"
#include "ui/grid.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **lines;
  bool *heading;
  size_t n, cap, scroll;
  int width;
} Structure;

static void add_line(Structure *s, char *line, bool heading) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 32;
    s->lines = safe_realloc(s->lines, s->cap * sizeof *s->lines);
    s->heading = safe_realloc(s->heading, s->cap * sizeof *s->heading);
  }
  s->lines[s->n] = line;
  s->heading[s->n++] = heading;
  int w = nc_text_width(line);
  if (w > s->width) s->width = w;
}

/* A row of cells padded to the column widths */
static char *join_cells(char *const *cells, const int *w, size_t n) {
  char *out = str_dup("");
  for (size_t i = 0; i < n; i++) {
    int pad = w[i] - nc_text_width(cells[i]);
    char *next = str_printf("%s%s%*s%s", out, cells[i], pad > 0 ? pad : 0, "", i + 1 < n ? "  " : "");
    free(out);
    out = next;
  }
  size_t len = strlen(out);
  while (len && out[len - 1] == ' ') out[--len] = 0;
  return out;
}

#define NCOLS 7

static void build(Structure *s, const LaceSchema *sc) {
  static const char *const head[NCOLS] = {"#", "Name", "Type", "Null", "PK", "Default", "Extra"};
  size_t n = sc->num_columns;
  char ***cells = safe_calloc(n + 1, sizeof *cells);
  int w[NCOLS] = {0};
  for (size_t r = 0; r <= n; r++) {
    cells[r] = safe_calloc(NCOLS, sizeof **cells);
    if (r == 0) {
      for (int c = 0; c < NCOLS; c++) cells[0][c] = str_dup(head[c]);
    } else {
      const LaceColumn *col = &sc->columns[r - 1];
      char *extra = str_dup("");
      const char *bits[3] = {col->auto_increment ? "auto-increment" : NULL, col->generated ? "generated" : NULL,
                             col->foreign_key ? col->foreign_key : NULL};
      for (int b = 0; b < 3; b++) {
        if (!bits[b]) continue;
        char *next = str_printf("%s%s%s%s", extra, *extra ? ", " : "", b == 2 ? "-> " : "", bits[b]);
        free(extra);
        extra = next;
      }
      cells[r][0] = str_printf("%zu", r);
      cells[r][1] = str_dup(col->name ? col->name : "");
      cells[r][2] = str_dup(col->type_name ? col->type_name : "");
      cells[r][3] = str_dup(col->nullable ? "NULL" : "NOT NULL");
      cells[r][4] = str_dup(col->primary_key ? "PK" : "");
      cells[r][5] = str_dup(col->default_val ? col->default_val : "");
      cells[r][6] = extra;
    }
    for (int c = 0; c < NCOLS; c++) {
      int cw = nc_text_width(cells[r][c]);
      if (cw > w[c]) w[c] = cw;
    }
  }
  for (size_t r = 0; r <= n; r++) {
    add_line(s, join_cells(cells[r], w, NCOLS), r == 0);
    for (int c = 0; c < NCOLS; c++) free(cells[r][c]);
    free(cells[r]);
  }
  free(cells);
  if (sc->num_indexes) {
    add_line(s, str_dup(""), false);
    add_line(s, str_dup("Indexes"), true);
    for (size_t i = 0; i < sc->num_indexes; i++) {
      const LaceIndex *ix = &sc->indexes[i];
      char *cols = str_dup("");
      for (size_t k = 0; k < ix->num_columns; k++) {
        char *next = str_printf("%s%s%s", cols, k ? ", " : "", ix->columns[k]);
        free(cols);
        cols = next;
      }
      add_line(s, str_printf("%s%s  (%s)%s%s", ix->name ? ix->name : "", ix->primary ? "  PRIMARY" : ix->unique ? "  UNIQUE" : "",
                             cols, ix->type ? "  " : "", ix->type ? ix->type : ""), false);
      free(cols);
    }
  }
  if (sc->num_foreign_keys) {
    add_line(s, str_dup(""), false);
    add_line(s, str_dup("Foreign keys"), true);
    for (size_t i = 0; i < sc->num_foreign_keys; i++) {
      const LaceForeignKey *fk = &sc->foreign_keys[i];
      char *a = str_dup(""), *b = str_dup("");
      for (size_t k = 0; k < fk->num_columns; k++) {
        char *na = str_printf("%s%s%s", a, k ? ", " : "", fk->columns[k]);
        free(a);
        a = na;
      }
      for (size_t k = 0; k < fk->num_ref_columns; k++) {
        char *nb = str_printf("%s%s%s", b, k ? ", " : "", fk->ref_columns[k]);
        free(b);
        b = nb;
      }
      add_line(s, str_printf("(%s) -> %s (%s)", a, fk->ref_table ? fk->ref_table : "", b), false);
      free(a);
      free(b);
    }
  }
}

static void scroll_to(Overlay *o, Structure *s, long pos) {
  long max = (long)s->n - o->rows;
  if (max < 0) max = 0;
  if (pos > max) pos = max;
  if (pos < 0) pos = 0;
  s->scroll = (size_t)pos;
}

static bool structure_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Structure *s = o->data;
  if (ev->type == UI_EVENT_MOUSE) {
    if (ev->mouse.button == UI_MOUSE_SCROLL_UP) scroll_to(o, s, (long)s->scroll - 3);
    if (ev->mouse.button == UI_MOUSE_SCROLL_DOWN) scroll_to(o, s, (long)s->scroll + 3);
    return true;
  }
  if (ev->type != UI_EVENT_KEY) return true;
  long pos = (long)s->scroll;
  if (ev->key.is_special) {
    switch (ev->key.key) {
    case UI_KEY_UP: pos--; break;
    case UI_KEY_DOWN: pos++; break;
    case UI_KEY_PAGEUP: pos -= o->rows; break;
    case UI_KEY_PAGEDOWN: pos += o->rows; break;
    case UI_KEY_HOME: pos = 0; break;
    case UI_KEY_END: pos = (long)s->n; break;
    case UI_KEY_ENTER: overlay_pop_this(app, o); return true;
    default: return false;
    }
  } else if (ev->key.key == 'j' || ev->key.key == 'k') {
    pos += ev->key.key == 'j' ? 1 : -1;
  } else if (ev->key.key == 'q' || ev->key.key == 's') {
    overlay_pop_this(app, o);
    return true;
  }
  scroll_to(o, s, pos);
  return true;
}

static void structure_draw(Overlay *o, NcApp *app) {
  Structure *s = o->data;
  scroll_to(o, s, (long)s->scroll);
  for (int i = 0; i < o->rows; i++) {
    size_t k = s->scroll + (size_t)i;
    if (k >= s->n) break;
    if (s->heading[k]) nc_style(o->plane, &app->theme, TC_ACCENT, TC_OVERLAY_BG, NCSTYLE_BOLD);
    else nc_style(o->plane, &app->theme, TC_OVERLAY_FG, TC_OVERLAY_BG, 0);
    nc_put(o->plane, 1 + i, 2, o->cols - 2, s->lines[k]);
  }
}

static void structure_layout(Overlay *o, NcApp *app) {
  Structure *s = o->data;
  o->cols = s->width + 4;
  if (o->cols < 40) o->cols = 40;
  if (o->cols > app->cols - 8) o->cols = app->cols - 8;
  o->rows = (int)s->n;
  if (o->rows > app->rows - 8) o->rows = app->rows - 8;
}

static void structure_free(void *data) {
  Structure *s = data;
  for (size_t i = 0; i < s->n; i++) free(s->lines[i]);
  free(s->lines);
  free(s->heading);
  free(s);
}

void structure_open(NcApp *app, View *v) {
  if (!v->schema) return;
  Structure *s = safe_calloc(1, sizeof *s);
  build(s, v->schema);
  char *title = str_printf("Structure: %s", v->table ? v->table : "");
  Overlay *o = overlay_new(title);
  free(title);
  o->footer = "Esc close  \xe2\x86\x91\xe2\x86\x93 scroll";
  o->data = s;
  o->free_data = structure_free;
  o->draw = structure_draw;
  o->key = structure_key;
  o->layout = structure_layout;
  overlay_push(app, o);
}
