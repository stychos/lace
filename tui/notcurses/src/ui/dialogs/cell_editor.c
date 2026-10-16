/* The floating cell editor: a multi-line buffer over the cursor cell */
#include "core/utf8.h"
#include "edit_ops.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "ui/overlay.h"
#include "ui/textbuf.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint64_t uid, gen;
  size_t row, col;
  TextBuf tb;
  bool is_null;  /* the cell was NULL and nothing has been typed */
  size_t scroll_line, scroll_col;
} CellEditor;

static void ed_layout(Overlay *o, NcApp *app) {
  o->cols = app->cols - 8 < 80 ? app->cols - 8 : 80;
  o->rows = app->rows - 6 < 20 ? app->rows - 6 : 20;
  if (o->cols < 20) o->cols = 20;
  if (o->rows < 3) o->rows = 3;
}

/* One line with tabs expanded to 4 columns (allocated) */
static char *expand(const char *s, size_t n) {
  char *out = safe_malloc(n * 4 + 1);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\t') { memset(out + o, ' ', 4); o += 4; }
    else if (s[i] == '\r') out[o++] = ' ';
    else out[o++] = s[i];
  }
  out[o] = 0;
  return out;
}

static void ed_draw(Overlay *o, NcApp *app) {
  CellEditor *e = o->data;
  struct ncplane *p = o->plane;
  const Theme *t = &app->theme;
  int w = o->cols - 2, h = o->rows;
  size_t line, col;
  textbuf_line_col(&e->tb, &line, &col);
  if (line < e->scroll_line) e->scroll_line = line;
  if (line >= e->scroll_line + (size_t)h) e->scroll_line = line - (size_t)h + 1;
  if (col < e->scroll_col) e->scroll_col = col;
  if (col >= e->scroll_col + (size_t)w - 1) e->scroll_col = col - (size_t)w + 2;
  nc_style(p, t, TC_OVERLAY_FG, TC_EDIT_BG, 0);
  for (int i = 0; i < h; i++) {
    nc_put(p, 1 + i, 1, o->cols, "");
    size_t l = e->scroll_line + (size_t)i;
    if (l >= textbuf_num_lines(&e->tb)) continue;
    size_t s = textbuf_line_start(&e->tb, l), en = textbuf_line_end(&e->tb, l);
    char *ex = expand(e->tb.text + s, en - s);
    size_t off = utf8_offset_at_col(ex, strlen(ex), (int)e->scroll_col);
    nc_put(p, 1 + i, 2, w, ex + off);
    str_secure_free(ex);
  }
  if (e->is_null && e->tb.len == 0) {
    nc_style(p, t, TC_NULL_FG, TC_EDIT_BG, 0);
    nc_put(p, 1, 2, w, "NULL");
  }
  o->wants_cursor = true;
  o->cursor_y = (int)(line - e->scroll_line);
  o->cursor_x = 1 + (int)(col - e->scroll_col);
}

/* Save through the shared write path; the editor closes unless the text was refused */
static void ed_save(Overlay *o, NcApp *app, const char *text, bool set_null, bool set_empty) {
  CellEditor *e = o->data;
  View *v = app_find_view(app, e->uid);
  EditContext ctx;
  const char *why = NULL;
  if (!v) {
    overlay_pop_this(app, o);
    return;
  }
  size_t row = e->row, col = e->col;
  if (!edit_context_for_view(app, v, &ctx, &why)) {
    grid_edit_report(app, why);
    overlay_pop_this(app, o);
    return;
  }
  if (edit_cell(&ctx, row, col, text, set_null, set_empty, e->gen, &why)) {
    overlay_pop_this(app, o);
    return;
  }
  grid_edit_report(app, why);
  if (!strcmp(why, "No change") || e->gen != page_cache_generation(v->cache)) overlay_pop_this(app, o);
}

static bool ed_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  CellEditor *e = o->data;
  TextBuf *tb = &e->tb;
  if (ev->type != UI_EVENT_KEY) return true;
  if (nc_key_special(ev, UI_KEY_ESCAPE)) return false;
  if (hotkey_matches(app->config, ev, HOTKEY_EDITOR_NULL)) {
    ed_save(o, app, NULL, true, false);
    return true;
  }
  if (hotkey_matches(app->config, ev, HOTKEY_EDITOR_EMPTY)) {
    ed_save(o, app, "", false, true);
    return true;
  }
  /* Ctrl+J is Enter on a legacy terminal (notcurses cannot tell them apart); Alt+Enter
   always arrives as itself */
  bool shift_enter = nc_key_special(ev, UI_KEY_ENTER) && (ev->key.mods & (UI_MOD_SHIFT | UI_MOD_ALT));
  if (shift_enter || nc_key_ctrl(ev, 'J')) {
    textbuf_insert(tb, "\n", 1);
    e->is_null = false;
    return true;
  }
  if (nc_key_special(ev, UI_KEY_ENTER) || hotkey_matches(app->config, ev, HOTKEY_EDITOR_SAVE)) {
    ed_save(o, app, e->is_null && !tb->len ? NULL : tb->text, e->is_null && !tb->len, false);
    return true;
  }
  if (nc_key_ctrl(ev, 'K')) {
    if (tb->len) {
      app_clipboard_set(app, tb->text);
      app_set_status(app, false, "Copied to clipboard");
    }
    return true;
  }
  if (nc_key_ctrl(ev, 'U')) {
    const char *clip = app_clipboard_get(app);
    if (clip && *clip) {
      textbuf_insert(tb, clip, strlen(clip));
      e->is_null = false;
    }
    return true;
  }
  if (ev->key.is_special) {
    switch (ev->key.key) {
    case UI_KEY_LEFT: textbuf_move_char(tb, -1); break;
    case UI_KEY_RIGHT: textbuf_move_char(tb, 1); break;
    case UI_KEY_UP: textbuf_move_line(tb, -1); break;
    case UI_KEY_DOWN: textbuf_move_line(tb, 1); break;
    case UI_KEY_PAGEUP: for (int i = 0; i < o->rows - 1; i++) textbuf_move_line(tb, -1); break;
    case UI_KEY_PAGEDOWN: for (int i = 0; i < o->rows - 1; i++) textbuf_move_line(tb, 1); break;
    case UI_KEY_HOME: textbuf_home(tb); break;
    case UI_KEY_END: textbuf_end(tb); break;
    case UI_KEY_BACKSPACE: textbuf_delete(tb, true); e->is_null = false; break;
    case UI_KEY_DELETE: textbuf_delete(tb, false); e->is_null = false; break;
    case UI_KEY_TAB: textbuf_insert(tb, "\t", 1); e->is_null = false; break;
    default: break;
    }
    return true;
  }
  if (nc_key_ctrl(ev, 'A')) { textbuf_home(tb); return true; }
  if (nc_key_ctrl(ev, 'E')) { textbuf_end(tb); return true; }
  char buf[8];
  const char *typed = nc_key_typed(ev, buf);
  if (typed) {
    textbuf_insert(tb, typed, strlen(typed));
    e->is_null = false;
  }
  return true;
}

static void ed_free(void *data) {
  CellEditor *e = data;
  textbuf_free(&e->tb);
  free(e);
}

void cell_editor_open(NcApp *app, View *v, size_t row, size_t col) {
  EditContext ctx;
  const char *why = NULL;
  if (!edit_context_for_view(app, v, &ctx, &why)) {
    grid_edit_report(app, why);
    return;
  }
  if ((why = edit_check_cell(&ctx, row, col))) {
    app_set_status(app, true, "%s", why);
    return;
  }
  const LaceValue *cell = page_cache_cell(v->cache, row, col);
  const LaceColumn *c = page_cache_column(v->cache, col);
  CellEditor *e = safe_calloc(1, sizeof *e);
  e->uid = v->uid;
  e->gen = page_cache_generation(v->cache);
  e->row = row;
  e->col = col;
  e->is_null = cell->is_null || cell->type == LACE_TYPE_NULL;
  char *text = e->is_null ? NULL : lace_value_to_edit_text(cell);
  textbuf_init(&e->tb, text);
  str_secure_free(text);
  char *title = str_printf("Editor: %s", c->name ? c->name : "");
  Overlay *o = overlay_new(title);
  free(title);
  o->footer = "Enter save  \xe2\x87\xa7" "Enter newline  ^N NULL  ^D empty  Esc cancel";
  o->data = e;
  o->free_data = ed_free;
  o->layout = ed_layout;
  o->draw = ed_draw;
  o->key = ed_key;
  overlay_push(app, o);
}
