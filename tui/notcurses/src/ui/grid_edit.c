/* Inline cell editing in the grid: Enter starts it on the cursor cell, the
 * cell turns into a text field, Enter saves through edit_cell(). The cell
 * editors check their own keys first (Ctrl+K copy, Ctrl+U paste, Ctrl+N NULL,
 * Ctrl+D empty), as the ncurses TUI's tui_cell_edit_key(). */
#include "edit_ops.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

static struct { bool valid; int y, x; } g_cursor;

void grid_edit_report(NcApp *app, const char *why) {
  app_set_status(app, why && strcmp(why, "No change") != 0, "%s", why ? why : "");
}

bool grid_editing(const View *v) { return v && v->editing; }

void grid_edit_cancel(View *v) {
  if (!v) return;
  str_secure_free(v->edit_text);
  v->edit_text = NULL;
  v->edit_cursor = 0;
  v->editing = false;
}

void grid_edit_start(NcApp *app, View *v) {
  EditContext ctx;
  const char *why = NULL;
  if (!edit_context_for_view(app, v, &ctx, &why)) {
    grid_edit_report(app, why);
    return;
  }
  if (page_cache_extent(v->cache) == 0) return;
  if ((why = edit_check_cell(&ctx, v->cursor_row, v->cursor_col))) {
    app_set_status(app, true, "%s", why);
    return;
  }
  v->preview = false; /* an edited preview is kept */
  const LaceValue *cell = page_cache_cell(v->cache, v->cursor_row, v->cursor_col);
  v->edit_row = v->cursor_row;
  v->edit_col = v->cursor_col;
  v->edit_gen = page_cache_generation(v->cache);
  v->editing = true;
  v->edit_text = cell->is_null || cell->type == LACE_TYPE_NULL ? NULL : lace_value_to_edit_text(cell);
  v->edit_cursor = v->edit_text ? strlen(v->edit_text) : 0;
  grid_follow(app, v, false);
}

/* Save what is in the field; the field closes unless the text was refused */
static void edit_save(NcApp *app, View *v, const char *text, bool set_null, bool set_empty) {
  EditContext ctx;
  const char *why = NULL;
  if (!edit_context_for_view(app, v, &ctx, &why)) {
    grid_edit_report(app, why);
    grid_edit_cancel(v);
    return;
  }
  if (edit_cell(&ctx, v->edit_row, v->edit_col, text, set_null, set_empty, v->edit_gen, &why)) {
    grid_edit_cancel(v);
    return;
  }
  grid_edit_report(app, why);
  if (!strcmp(why, "No change") || v->edit_gen != page_cache_generation(v->cache)) grid_edit_cancel(v);
  /* anything else: the field stays so the text can be fixed */
}

bool grid_edit_key(NcApp *app, View *v, const UiEvent *ev) {
  if (ev->type != UI_EVENT_KEY) return true;
  if (nc_key_special(ev, UI_KEY_ESCAPE)) {
    grid_edit_cancel(v);
    return true;
  }
  if (nc_key_special(ev, UI_KEY_ENTER)) {
    edit_save(app, v, v->edit_text, v->edit_text == NULL, false);
    return true;
  }
  if (nc_key_ctrl(ev, 'K')) {
    if (v->edit_text && v->edit_text[0]) {
      app_clipboard_set(app, v->edit_text);
      app_set_status(app, false, "Copied to clipboard");
    }
    return true;
  }
  if (nc_key_ctrl(ev, 'U')) {
    const char *clip = app_clipboard_get(app);
    if (clip && *clip) {
      TextInput in;
      text_input_adopt(&in, v->edit_text ? v->edit_text : str_dup(""), v->edit_cursor);
      text_input_insert(&in, clip);
      v->edit_text = text_input_release(&in, &v->edit_cursor);
    }
    return true;
  }
  if (hotkey_matches(app->config, ev, HOTKEY_EDITOR_NULL)) {
    edit_save(app, v, NULL, true, false);
    return true;
  }
  if (hotkey_matches(app->config, ev, HOTKEY_EDITOR_EMPTY)) {
    edit_save(app, v, "", false, true);
    return true;
  }
  /* Cursor movement, deletion and text; other keys are swallowed */
  bool was_null = v->edit_text == NULL;
  TextInput in;
  text_input_adopt(&in, was_null ? str_dup("") : v->edit_text, v->edit_cursor);
  nc_text_input_key(&in, ev);
  char *text = text_input_release(&in, &v->edit_cursor);
  if (was_null && !text[0]) { /* still NULL: nothing was typed */
    free(text);
    text = NULL;
    v->edit_cursor = 0;
  }
  v->edit_text = text;
  return true;
}

/* Called by grid_draw after the rows: the field over the cursor cell, wider
 * than a narrow column so there is room to type */
void grid_edit_draw(NcApp *app, View *v, int y, int x, int w, int right_edge) {
  g_cursor.valid = false;
  if (!v->editing) return;
  int want = 30;
  if (w < want) w = want;
  if (w > right_edge - x) w = right_edge - x;
  if (w < 1) return;
  const Theme *t = &app->theme;
  struct ncplane *p = app->std;
  if (!v->edit_text) {
    nc_style(p, t, TC_NULL_FG, TC_EDIT_BG, 0);
    nc_put(p, y, x, w, "NULL");
    g_cursor.valid = true;
    g_cursor.y = y;
    g_cursor.x = x;
    return;
  }
  char *shown = str_dup(v->edit_text); /* one line: breaks and tabs are one column each */
  for (char *c = shown; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  TextInput in = {.buf = shown, .len = strlen(shown), .cursor = v->edit_cursor};
  nc_style(p, t, TC_FG, TC_EDIT_BG, 0);
  nc_put(p, y, x, w, "");
  int cx = nc_text_input_draw(p, t, &in, y, x, w, true);
  str_secure_free(shown);
  g_cursor.valid = true;
  g_cursor.y = y;
  g_cursor.x = x + cx;
}

void grid_edit_begin_frame(void) { g_cursor.valid = false; }

bool grid_edit_cursor(int *y, int *x) {
  if (!g_cursor.valid) return false;
  *y = g_cursor.y;
  *x = g_cursor.x;
  return true;
}
