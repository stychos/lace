#include "ui/conn_picker.h"
#include "core/connstr_display.h"
#include "core/utf8.h"
#include "ui/dialogs/confirm.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_version = 1;
void cp_changed(void) { g_version++; }

void nc_draw_input(struct ncplane *pl, const Theme *t, int y, int x, int w, const TextInput *in,
                   ThemeColor fg, ThemeColor bg, int *cursor_x) {
  nc_style(pl, t, fg, bg, 0);
  nc_put(pl, y, x, w, "");
  const char *buf = in->buf ? in->buf : "";
  size_t len = in->len, cur = in->cursor < len ? in->cursor : len;
  char *disp;
  size_t dcur;
  if (in->secret) {
    size_t n = utf8_count(buf, len);
    disp = safe_malloc(n + 1);
    memset(disp, '*', n);
    disp[n] = 0;
    dcur = utf8_count(buf, cur);
  } else {
    disp = str_dup(buf);
    dcur = cur;
  }
  size_t start = 0, dl = strlen(disp);
  while (start < dcur && utf8_width(disp + start, dcur - start) > w - 1) start = utf8_next(disp, dl, start);
  nc_put(pl, y, x, w, disp + start);
  if (cursor_x) *cursor_x = x + utf8_width(disp + start, dcur - start);
  free(disp);
}

/* ---- rows ---- */

typedef struct { ListRow *rows; size_t n, cap; } Rows;

static void add(Rows *r, const char *text, const char *detail, int kind, size_t idx, bool header) {
  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 32;
    r->rows = safe_realloc(r->rows, r->cap * sizeof *r->rows);
  }
  r->rows[r->n++] = (ListRow){str_dup(text), detail ? str_dup(detail) : NULL, NULL, kind,
                              (void *)(intptr_t)idx, header};
}

static bool contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return true;
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

/* "data.sqlite" / "db on host": what a narrow panel can show of a URL */
static char *short_name(const char *url) {
  LaceSavedConnection *m = lace_saved_conn_from_connstr(url, NULL);
  if (!m) return NULL;
  char *out;
  if (m->driver && !strcmp(m->driver, "sqlite")) {
    const char *slash = m->database ? strrchr(m->database, '/') : NULL;
    out = str_dup(slash && slash[1] ? slash + 1 : m->database ? m->database : "sqlite");
  } else {
    out = str_printf("%s on %s", m->database && *m->database ? m->database : m->driver ? m->driver : "database",
                     m->host && *m->host ? m->host : "localhost");
  }
  lace_saved_conn_free(m);
  return out;
}

static void restore(ConnPicker *p, int kind, size_t idx) {
  ListWidget *l = &p->list;
  for (size_t i = 0; i < l->num_shown; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    if (!r->header && r->kind == kind && (size_t)(intptr_t)r->data == idx) { l->cursor = i; return; }
  }
}

void cp_rebuild(NcApp *app, ConnPicker *p) {
  int keep_kind = -1;
  size_t keep_idx = 0;
  ListRow *sel = list_selected(&p->list);
  if (sel) { keep_kind = sel->kind; keep_idx = (size_t)(intptr_t)sel->data; }
  const char *filter = p->in.buf ? p->in.buf : "";
  Rows r = {0};
  size_t n = app->connmgr ? connmgr_count_filtered(app->connmgr, filter) : 0;
  if (n) add(&r, "Saved connections", NULL, 0, 0, true);
  for (size_t i = 0; i < n; i++) {
    ConnectionItem *it = connmgr_get_filtered_item(app->connmgr, filter, i);
    if (!it) continue;
    int depth = connmgr_get_item_depth(it);
    int indent = depth > 1 ? (depth - 1) * 2 : 0;
    if (it->type == CONN_ITEM_FOLDER) {
      char *text = str_printf("%*s%s %s", indent, "", it->folder.expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8",
                              connmgr_item_name(it));
      add(&r, text, NULL, CPK_FOLDER, i, false);
      free(text);
    } else {
      char *text = str_printf("%*s  %s", indent, "", connmgr_item_name(it));
      char *open = app_conn_open_in(app, it->connection.id);
      char *detail = open ? str_printf("\xe2\x97\x8f open in %s", open) : NULL;
      add(&r, text, detail, CPK_CONN, i, false);
      free(text);
      free(detail);
      free(open);
    }
  }
  bool header = false;
  for (size_t i = 0; i < app->recent.count; i++) {
    char *d = connstr_display(app->recent.items[i].url);
    char *name = short_name(app->recent.items[i].url);
    const char *label = d ? d : app->recent.items[i].url;
    if (contains_ci(label, filter) || contains_ci(app->recent.items[i].url, filter)) {
      if (!header) { add(&r, "Recent", NULL, 0, 0, true); header = true; }
      char *text = str_printf("  %s", name ? name : label);
      add(&r, text, NULL, CPK_RECENT, i, false);
      free(text);
    }
    free(name);
    free(d);
  }
  list_set_rows(&p->list, r.rows, r.n);
  p->seen = g_version;
  if (keep_kind > 0) restore(p, keep_kind, keep_idx);
}

void cp_init(NcApp *app, ConnPicker *p, bool input_focused) {
  memset(p, 0, sizeof *p);
  text_input_init(&p->in, NULL, false);
  list_init(&p->list, false);
  p->input_focused = input_focused;
  cp_rebuild(app, p);
}

void cp_free(ConnPicker *p) {
  list_free(&p->list);
  text_input_free(&p->in);
}

/* ---- drawing ---- */

void cp_draw_list(NcApp *app, ConnPicker *p, struct ncplane *pl, int y, int x, int rows, int cols) {
  cp_rebuild(app, p); /* every draw: "open in" follows the tabs */
  list_draw(&p->list, pl, &app->theme, y, x, rows, cols, NULL, NULL);
}
