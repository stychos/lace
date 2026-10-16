#include "ui/filter_panel.h"
#include "model/workspace.h"
#include "ui/input.h"
#include "ui/list.h"
#include "ui/overlay.h"
#include "ui/panels.h"
#include "ui/sidebar.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

static struct { bool set; int y, x; } g_cursor;

bool filter_panel_enabled(const NcApp *app) { return prefs_section_bool(app->config, "filters_strip", true); }

int filter_panel_height(size_t num_filters, int avail) {
  size_t n = num_filters ? num_filters : 1;
  if (n > FP_MAX_ROWS) n = FP_MAX_ROWS;
  int h = 1 + (int)n;
  if (avail - h < FP_GRID_KEEP) h = avail - FP_GRID_KEEP;
  return h >= 2 ? h : 0;
}

int filter_panel_rows(const NcApp *app, const View *v) {
  if (!v || v->kind != VIEW_TABLE || !v->filters_visible || !filter_panel_enabled(app)) return 0;
  return filter_panel_height(v->num_filters, content_rect(app).rows);
}

Rect filter_panel_rect(const NcApp *app, const View *v) {
  Rect c = content_rect(app);
  int sw = sidebar_width(app);
  return (Rect){c.y, sw, filter_panel_rows(app, v), c.cols - sw};
}

bool filter_panel_cursor(int *y, int *x) {
  if (!g_cursor.set) return false;
  *y = g_cursor.y;
  *x = g_cursor.x;
  return true;
}

void filter_panel_layout(int cols, bool two_values, FpLayout *L) {
  bool narrow = cols < 60;
  L->col_x = 1;
  L->col_w = narrow ? 10 : 14;
  L->op_x = L->col_x + L->col_w + 2;
  L->op_w = narrow ? 9 : 12;
  L->val_x = L->op_x + L->op_w + 2;
  L->del_x = cols - 2;
  int vw = L->del_x - L->val_x - 1;
  if (vw < 4) vw = 4;
  if (two_values) {
    int half = (vw - 5) / 2;
    if (half < 3) half = 3;
    L->val_w = half;
    L->val2_x = L->val_x + half + 5; /* " and " */
    L->val2_w = vw - half - 5 > 3 ? vw - half - 5 : 3;
  } else {
    L->val_w = vw;
    L->val2_x = L->val_x + vw;
    L->val2_w = 0;
  }
}

/* ---- the rows ---- */

static bool is_raw(const LaceFilter *f) { return f->op == LACE_FILTER_RAW || !f->column; }

static bool field_ok(const LaceFilter *f, int field) {
  switch (field) {
  case FP_COLUMN: return true;
  case FP_OP: return !is_raw(f);
  case FP_VALUE: return is_raw(f) || lace_filter_op_needs_value(f->op);
  case FP_VALUE2: return !is_raw(f) && lace_filter_op_two_values(f->op);
  default: return false;
  }
}

static const char *cursor_column(const View *v) {
  const LaceColumn *c = page_cache_column(v->cache, v->cursor_col);
  if (c && c->name) return c->name;
  if (v->schema && v->schema->num_columns) return v->schema->columns[0].name;
  return NULL;
}

/* The panel always has a row to stand on */
static void ensure_row(View *v) {
  if (!v->num_filters) view_add_filter(v, cursor_column(v));
  if (v->fp.row >= v->num_filters) v->fp.row = v->num_filters - 1;
  if (!field_ok(&v->filters[v->fp.row], v->fp.field)) v->fp.field = FP_COLUMN;
}

static char *where_now(const View *v) { return view_where(v, LACE_DRIVER_SQLITE); }

/* Reload when the effective WHERE changed since before (taken) */
static void apply_if_changed(NcApp *app, View *v, char *before) {
  char *after = where_now(v);
  bool same = (!before && !after) || (before && after && strcmp(before, after) == 0);
  free(before);
  free(after);
  if (!same) {
    view_checkpoint(v);
    grid_state_changed(app, v);
  }
}

static void edit_start(View *v, const char *text) {
  free(v->fp.text);
  v->fp.text = str_dup(text ? text : "");
  v->fp.cursor = strlen(v->fp.text);
  v->fp.editing = true;
}

static void edit_cancel(View *v) {
  free(v->fp.text);
  v->fp.text = NULL;
  v->fp.editing = false;
}

static void edit_commit(NcApp *app, View *v) {
  if (!v->fp.editing) return;
  ensure_row(v);
  LaceFilter *f = &v->filters[v->fp.row];
  char *before = where_now(v);
  char *col = f->column ? str_dup(f->column) : NULL;
  char *a = str_dup(f->value ? f->value : ""), *b = f->value2 ? str_dup(f->value2) : NULL;
  if (v->fp.field == FP_VALUE2) view_filter_set(v, v->fp.row, col, f->op, a, v->fp.text);
  else view_filter_set(v, v->fp.row, col, f->op, v->fp.text, b);
  free(col);
  free(a);
  free(b);
  edit_cancel(v);
  apply_if_changed(app, v, before);
}

static void set_column(NcApp *app, View *v, size_t row, const char *name) {
  if (row >= v->num_filters) return;
  LaceFilter *f = &v->filters[row];
  char *before = where_now(v);
  LaceFilterOp op = !name ? LACE_FILTER_RAW : is_raw(f) ? LACE_FILTER_EQ : f->op;
  char *a = str_dup(f->value ? f->value : ""), *b = f->value2 ? str_dup(f->value2) : NULL;
  view_filter_set(v, row, name, op, a, b);
  free(a);
  free(b);
  apply_if_changed(app, v, before);
}

static void set_op(NcApp *app, View *v, size_t row, LaceFilterOp op) {
  if (row >= v->num_filters) return;
  LaceFilter *f = &v->filters[row];
  if (is_raw(f) || op == LACE_FILTER_RAW) return;
  char *before = where_now(v);
  char *col = str_dup(f->column), *a = str_dup(f->value ? f->value : "");
  char *b = lace_filter_op_two_values(op) && f->value2 ? str_dup(f->value2) : NULL;
  view_filter_set(v, row, col, op, a, b);
  free(col);
  free(a);
  free(b);
  apply_if_changed(app, v, before);
}

static void cycle_op(NcApp *app, View *v, int dir) {
  LaceFilter *f = &v->filters[v->fp.row];
  if (is_raw(f)) return;
  int n = LACE_FILTER_RAW; /* every operator but RAW */
  set_op(app, v, v->fp.row, (LaceFilterOp)(((int)f->op + dir + n) % n));
  if (!field_ok(&v->filters[v->fp.row], v->fp.field)) v->fp.field = FP_OP;
}

/* Move to the next (dir 1) or previous valid field, across rows; false when
 * that leaves the panel */
static bool move_field(View *v, int dir) {
  size_t row = v->fp.row;
  int field = v->fp.field;
  for (;;) {
    field += dir;
    if (field > FP_VALUE2) {
      if (row + 1 >= v->num_filters) return false;
      row++;
      field = FP_COLUMN;
    } else if (field < FP_COLUMN) {
      if (row == 0) return false;
      row--;
      field = FP_VALUE2;
    }
    if (field_ok(&v->filters[row], field)) break;
  }
  v->fp.row = row;
  v->fp.field = field;
  return true;
}

static void focus_row(NcApp *app, View *v, size_t row, int preferred) {
  ConnTab *t = app_view_tab(app, v);
  if (t) t->sidebar_focused = false;
  v->fp.focused = true;
  v->fp.row = row < v->num_filters ? row : 0;
  v->fp.field = preferred;
  if (!field_ok(&v->filters[v->fp.row], v->fp.field)) {
    v->fp.field = FP_COLUMN;
    const LaceFilter *f = &v->filters[v->fp.row];
    for (int fld = FP_VALUE; fld >= FP_COLUMN; fld--)
      if (field_ok(f, fld)) { v->fp.field = fld; break; }
  }
}

/* The row of the cursor column's filter; a blank row is given that column,
 * else one is added */
static void focus_cursor_column(NcApp *app, View *v) {
  ensure_row(v);
  const char *name = cursor_column(v);
  size_t row = SIZE_MAX;
  for (size_t i = 0; i < v->num_filters && row == SIZE_MAX; i++)
    if (name && v->filters[i].column && strcmp(v->filters[i].column, name) == 0) row = i;
  for (size_t i = 0; i < v->num_filters && row == SIZE_MAX; i++)
    if (!view_filter_active(&v->filters[i]) && !is_raw(&v->filters[i])) {
      set_column(app, v, i, name);
      row = i;
    }
  if (row == SIZE_MAX) row = view_add_filter(v, name);
  focus_row(app, v, row, FP_VALUE);
}

static void hide(NcApp *app, View *v) {
  (void)app;
  edit_cancel(v);
  view_prune_filters(v);
  v->fp.focused = false;
  v->filters_visible = false;
  v->fp.row = 0;
  v->fp.scroll = 0;
}

void filter_panel_toggle(NcApp *app, View *v) {
  if (!v->schema) {
    app_set_status(app, false, "The table is still loading");
    return;
  }
  if (!v->filters_visible) {
    v->filters_visible = true;
    focus_cursor_column(app, v);
  } else if (v->fp.focused) {
    hide(app, v);
  } else {
    focus_cursor_column(app, v);
  }
}

void filter_panel_focus(NcApp *app, View *v, bool last) {
  ensure_row(v);
  if (last) {
    size_t row = v->num_filters - 1;
    focus_row(app, v, row, FP_VALUE2);
    if (!field_ok(&v->filters[row], FP_VALUE2)) {
      v->fp.field = FP_VALUE2 + 1;
      v->fp.row = row;
      move_field(v, -1);
    }
  } else {
    focus_row(app, v, 0, FP_COLUMN);
  }
}

void filter_panel_blur(NcApp *app, View *v) {
  if (v->fp.editing) edit_commit(app, v);
  v->fp.focused = false;
}

void filter_panel_add(NcApp *app, View *v) {
  if (!v->schema) {
    app_set_status(app, false, "The table is still loading");
    return;
  }
  if (v->fp.editing) edit_commit(app, v);
  v->filters_visible = true;
  ensure_row(v);
  size_t row = v->fp.row;
  if (view_filter_active(&v->filters[row]) || v->num_filters > 1 || is_raw(&v->filters[row]))
    row = view_add_filter(v, cursor_column(v));
  else
    set_column(app, v, row, cursor_column(v)); /* the one blank row: use it */
  focus_row(app, v, row, FP_VALUE);
  edit_start(v, "");
}

void filter_panel_remove(NcApp *app, View *v) {
  if (!v->filters_visible || !v->fp.focused || !v->num_filters) {
    app_set_status(app, false, "No filter selected");
    return;
  }
  edit_cancel(v);
  char *before = where_now(v);
  view_remove_filter(v, v->fp.row);
  apply_if_changed(app, v, before);
  ensure_row(v);
}

/* ---- pickers: list overlays anchored to the field ---- */

typedef struct {
  ListWidget list;
  uint64_t uid;
  size_t row;
  int ay, ax; /* the field's screen cell */
  bool ops;   /* operator picker, else column picker */
} Picker;

static bool picker_key(Overlay *o, NcApp *app, const UiEvent *ev) {
  Picker *p = o->data;
  View *v = app_find_view(app, p->uid);
  ListAction a = ev->type == UI_EVENT_MOUSE ? list_mouse(&p->list, ev, 0, o->rows) : list_key(&p->list, ev);
  if (a == LIST_CANCEL) {
    overlay_pop_this(app, o);
  } else if (a == LIST_ACTIVATE) {
    ListRow *r = list_selected(&p->list);
    int kind = r ? r->kind : -2;
    size_t row = p->row;
    bool ops = p->ops;
    overlay_pop_this(app, o);
    if (v && row < v->num_filters && kind != -2) {
      if (ops) {
        set_op(app, v, row, (LaceFilterOp)kind);
        if (!field_ok(&v->filters[row], v->fp.field)) v->fp.field = FP_OP;
      } else {
        set_column(app, v, row, kind < 0 ? NULL : v->schema->columns[kind].name);
        if (!field_ok(&v->filters[row], v->fp.field)) v->fp.field = FP_COLUMN;
      }
    }
  }
  return true;
}

static void picker_draw(Overlay *o, NcApp *app) {
  Picker *p = o->data;
  list_draw(&p->list, o->plane, &app->theme, 1, 1, o->rows, o->cols, NULL, NULL);
}

static void picker_layout(Overlay *o, NcApp *app) {
  Picker *p = o->data;
  o->cols = 28;
  o->rows = (int)p->list.num_rows + 1; /* the filter input line */
  if (o->rows > app->rows - 6) o->rows = app->rows - 6;
  if (o->rows < 1) o->rows = 1;
  o->y = p->ay;
  o->x = p->ax - 1;
  if (o->y + o->rows + 2 > app->rows) o->y = app->rows - o->rows - 2;
  if (o->x + o->cols + 2 > app->cols) o->x = app->cols - o->cols - 2;
  if (o->y < 0) o->y = 0;
  if (o->x < 0) o->x = 0;
}

static void picker_free(void *data) {
  Picker *p = data;
  list_free(&p->list);
  free(p);
}

static void open_picker(NcApp *app, View *v, bool ops, int ay, int ax) {
  Picker *p = safe_calloc(1, sizeof *p);
  list_init(&p->list, true); /* typing narrows the list */
  p->uid = v->uid;
  p->row = v->fp.row;
  p->ops = ops;
  p->ay = ay;
  p->ax = ax;
  const LaceFilter *f = &v->filters[v->fp.row];
  ListRow *rows;
  size_t n = 0, at = 0; /* the row of the current value */
  if (ops) {
    rows = safe_calloc(LACE_FILTER_RAW, sizeof *rows);
    for (int op = 0; op < LACE_FILTER_RAW; op++) {
      rows[n++] = (ListRow){str_dup(lace_filter_op_label((LaceFilterOp)op)), NULL, NULL, op, NULL, false};
      if ((int)f->op == op) at = (size_t)op;
    }
  } else {
    size_t nc = v->schema ? v->schema->num_columns : 0;
    rows = safe_calloc(nc + 1, sizeof *rows);
    for (size_t i = 0; i < nc; i++) {
      const char *name = v->schema->columns[i].name ? v->schema->columns[i].name : "";
      rows[n++] = (ListRow){str_dup(name), NULL, NULL, (int)i, NULL, false};
      if (f->column && strcmp(f->column, name) == 0) at = i;
    }
    rows[n++] = (ListRow){str_dup("Raw SQL"), NULL, NULL, -1, NULL, false};
    if (is_raw(f)) at = n - 1;
  }
  list_set_rows(&p->list, rows, n);
  p->list.cursor = at;
  Overlay *o = overlay_new(ops ? "Operator" : "Column");
  o->footer = "Enter pick  Esc cancel";
  o->data = p;
  o->free_data = picker_free;
  o->draw = picker_draw;
  o->key = picker_key;
  o->layout = picker_layout;
  overlay_push(app, o);
}

/* ---- drawing ---- */

static void clamp_scroll(View *v, int vis) {
  if (vis < 1) vis = 1;
  if (v->fp.row < v->fp.scroll) v->fp.scroll = v->fp.row;
  if (v->fp.row >= v->fp.scroll + (size_t)vis) v->fp.scroll = v->fp.row - (size_t)vis + 1;
  if (v->fp.scroll + (size_t)vis > v->num_filters)
    v->fp.scroll = v->num_filters > (size_t)vis ? v->num_filters - (size_t)vis : 0;
}

static void field(NcApp *app, int y, int x, int w, const char *text, bool focused, bool dim) {
  if (w <= 0) return;
  if (focused) nc_style(app->std, &app->theme, TC_CURSOR_FG, TC_CURSOR_BG, NCSTYLE_BOLD);
  else nc_style(app->std, &app->theme, dim ? TC_DIM : TC_FG, TC_BG, 0);
  nc_put(app->std, y, x, w, text);
}

static int row_y(Rect r, const View *v, size_t row) { return r.y + 1 + (int)(row - v->fp.scroll); }

/* The first key bound to an action, for the title's hints */
static char *first_key(const Config *c, HotkeyAction a, const char *dflt) {
  char *all = hotkey_get_display(c, a);
  if (!all || !*all) { free(all); return str_dup(dflt); }
  char *comma = strchr(all, ',');
  if (comma) *comma = 0;
  return all;
}

void filter_panel_draw(NcApp *app, View *v, Rect r) {
  g_cursor.set = false;
  if (r.rows < 2 || r.cols < 20) return;
  ensure_row(v);
  struct ncplane *p = app->std;
  const Theme *t = &app->theme;
  nc_style(p, t, TC_FG, TC_BG, 0);
  for (int y = r.y; y < r.y + r.rows; y++) nc_put(p, y, r.x, r.cols, "");
  size_t active = 0;
  for (size_t i = 0; i < v->num_filters; i++) active += view_filter_active(&v->filters[i]);
  /* the title: the count and the keys */
  char *title = active ? str_printf("Filters (%zu)", active) : str_dup("Filters");
  nc_style(p, t, TC_ACCENT, TC_BG, NCSTYLE_BOLD);
  int tw = nc_text_width(title);
  nc_put(p, r.y, r.x + 1, tw < r.cols - 2 ? tw : r.cols - 2, title);
  char *ka = first_key(app->config, HOTKEY_ADD_FILTER, "+");
  char *kr = first_key(app->config, HOTKEY_REMOVE_FILTER, "-");
  char *kc = first_key(app->config, HOTKEY_CLEAR_FILTERS, "X");
  char *kt = first_key(app->config, HOTKEY_TOGGLE_FILTERS, "f");
  char *hints = str_printf("%s add  %s remove  %s clear  Enter edit  Tab next  Esc table  %s close", ka, kr, kc, kt);
  free(ka); free(kr); free(kc); free(kt);
  int vis = r.rows - 1;
  char *pos = v->num_filters > (size_t)vis ? str_printf("%zu/%zu", v->fp.row + 1, v->num_filters) : NULL;
  int pw = pos ? nc_text_width(pos) : 0;
  int hx = r.x + 1 + tw + 2, hw = r.x + r.cols - 1 - pw - (pw ? 2 : 0) - hx;
  if (hw > 8) {
    nc_style(p, t, TC_DIM, TC_BG, 0);
    nc_put(p, r.y, hx, hw, hints);
  }
  if (pos) {
    nc_style(p, t, TC_DIM, TC_BG, 0);
    nc_put_right(p, r.y, r.x + r.cols - 1 - pw, pw, pos);
  }
  free(title);
  free(hints);
  free(pos);
  /* the rows */
  clamp_scroll(v, vis);
  for (size_t i = v->fp.scroll; i < v->num_filters && i < v->fp.scroll + (size_t)vis; i++) {
    const LaceFilter *f = &v->filters[i];
    int y = row_y(r, v, i);
    bool raw = is_raw(f), two = !raw && lace_filter_op_two_values(f->op);
    bool on = v->fp.focused && i == v->fp.row;
    bool inactive = !view_filter_active(f);
    FpLayout L;
    filter_panel_layout(r.cols, two, &L);
    field(app, y, r.x + L.col_x, L.col_w, raw ? "(raw SQL)" : f->column, on && v->fp.field == FP_COLUMN, inactive);
    field(app, y, r.x + L.op_x, L.op_w, raw ? "-" : lace_filter_op_label(f->op), on && v->fp.field == FP_OP,
          inactive || raw);
    if (field_ok(f, FP_VALUE)) {
      if (on && v->fp.editing && v->fp.field == FP_VALUE) {
        TextInput in = {.buf = v->fp.text, .len = strlen(v->fp.text), .cursor = v->fp.cursor};
        int cx = nc_text_input_draw(p, t, &in, y, r.x + L.val_x, L.val_w, true);
        g_cursor.set = true; g_cursor.y = y; g_cursor.x = r.x + L.val_x + cx;
      } else {
        field(app, y, r.x + L.val_x, L.val_w, f->value ? f->value : "", on && v->fp.field == FP_VALUE, false);
      }
    }
    if (two) {
      nc_style(p, t, TC_DIM, TC_BG, 0);
      nc_put(p, y, r.x + L.val_x + L.val_w, 5, " and ");
      if (on && v->fp.editing && v->fp.field == FP_VALUE2) {
        TextInput in = {.buf = v->fp.text, .len = strlen(v->fp.text), .cursor = v->fp.cursor};
        int cx = nc_text_input_draw(p, t, &in, y, r.x + L.val2_x, L.val2_w, true);
        g_cursor.set = true; g_cursor.y = y; g_cursor.x = r.x + L.val2_x + cx;
      } else {
        field(app, y, r.x + L.val2_x, L.val2_w, f->value2 ? f->value2 : "", on && v->fp.field == FP_VALUE2, false);
      }
    }
    nc_style(p, t, TC_DIM, TC_BG, 0);
    nc_put(p, y, r.x + L.del_x, 1, "\xc3\x97");
  }
}

/* ---- keys ---- */

static bool plain_special(const UiEvent *ev, int key) {
  return nc_key_special(ev, key) && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT));
}

static void leave_to_grid(View *v) { v->fp.focused = false; }

bool filter_panel_handle_key(NcApp *app, View *v, const UiEvent *ev) {
  if (ev->type != UI_EVENT_KEY || !v->fp.focused) return false;
  ensure_row(v);
  bool tab = plain_special(ev, UI_KEY_TAB), btab = plain_special(ev, UI_KEY_BTAB);
  bool up = plain_special(ev, UI_KEY_UP), down = plain_special(ev, UI_KEY_DOWN);
  if (v->fp.editing) {
    if (plain_special(ev, UI_KEY_ESCAPE)) { edit_cancel(v); return true; }
    if (plain_special(ev, UI_KEY_ENTER)) { edit_commit(app, v); return true; }
    if (tab || btab || up || down) {
      edit_commit(app, v); /* then the move below */
    } else if (hotkey_matches(app->config, ev, HOTKEY_ADD_FILTER) && ev->key.is_special) {
      edit_commit(app, v); /* Insert: a new row; the character keys type */
      filter_panel_add(app, v);
      return true;
    } else {
      TextInput in;
      text_input_adopt(&in, v->fp.text ? v->fp.text : str_dup(""), v->fp.cursor);
      nc_text_input_key(&in, ev);
      v->fp.text = text_input_release(&in, &v->fp.cursor);
      return true;
    }
  }
  if (hotkey_matches(app->config, ev, HOTKEY_TOGGLE_FILTERS)) { hide(app, v); return true; }
  if (hotkey_matches(app->config, ev, HOTKEY_ADD_FILTER)) { filter_panel_add(app, v); return true; }
  if (hotkey_matches(app->config, ev, HOTKEY_REMOVE_FILTER)) { filter_panel_remove(app, v); return true; }
  if (hotkey_matches(app->config, ev, HOTKEY_CLEAR_FILTERS)) {
    grid_do_action(app, v, HOTKEY_CLEAR_FILTERS); /* filters and sorts, as on the grid */
    v->fp.row = 0;
    ensure_row(v);
    v->fp.field = FP_VALUE;
    return true;
  }
  if (plain_special(ev, UI_KEY_ESCAPE)) { leave_to_grid(v); return true; }
  if (tab) {
    if (!move_field(v, 1)) leave_to_grid(v);
    return true;
  }
  if (btab) {
    if (!move_field(v, -1)) {
      ConnTab *t = app_view_tab(app, v);
      v->fp.focused = false;
      if (t && t->sidebar && sidebar_width(app)) t->sidebar_focused = true;
    }
    return true;
  }
  if (up || down) {
    if (down && v->fp.row + 1 < v->num_filters) v->fp.row++;
    else if (up && v->fp.row > 0) v->fp.row--;
    if (!field_ok(&v->filters[v->fp.row], v->fp.field)) v->fp.field = FP_COLUMN;
    return true;
  }
  if (plain_special(ev, UI_KEY_LEFT) || plain_special(ev, UI_KEY_RIGHT)) {
    size_t row = v->fp.row;
    int f = v->fp.field;
    if (!move_field(v, ev->key.key == UI_KEY_LEFT ? -1 : 1) || v->fp.row != row) { v->fp.row = row; v->fp.field = f; }
    return true;
  }
  if (nc_key_special(ev, UI_KEY_LEFT) || nc_key_special(ev, UI_KEY_RIGHT)) { /* Alt: the operator, from any field */
    if (ev->key.mods & UI_MOD_ALT) cycle_op(app, v, ev->key.key == UI_KEY_LEFT ? -1 : 1);
    return true;
  }
  const LaceFilter *f = &v->filters[v->fp.row];
  Rect r = filter_panel_rect(app, v);
  FpLayout L;
  filter_panel_layout(r.cols, !is_raw(f) && lace_filter_op_two_values(f->op), &L);
  int y = row_y(r, v, v->fp.row);
  if (plain_special(ev, UI_KEY_ENTER)) {
    switch (v->fp.field) {
    case FP_COLUMN: open_picker(app, v, false, y, r.x + L.col_x); break;
    case FP_OP: open_picker(app, v, true, y, r.x + L.op_x); break;
    case FP_VALUE: edit_start(v, f->value); break;
    case FP_VALUE2: edit_start(v, f->value2); break;
    }
    return true;
  }
  bool text_field = v->fp.field == FP_VALUE || v->fp.field == FP_VALUE2;
  if (text_field && (plain_special(ev, UI_KEY_DELETE) || plain_special(ev, UI_KEY_BACKSPACE))) {
    edit_start(v, "");
    edit_commit(app, v);
    return true;
  }
  char buf[8];
  const char *typed = nc_key_typed(ev, buf);
  if (typed && v->fp.field == FP_OP && typed[0] == ' ') { cycle_op(app, v, 1); return true; }
  if (typed && text_field) {
    edit_start(v, "");
    TextInput in;
    text_input_adopt(&in, v->fp.text, v->fp.cursor);
    text_input_insert(&in, typed);
    v->fp.text = text_input_release(&in, &v->fp.cursor);
    return true;
  }
  return false; /* the hotkeys may have it */
}

/* ---- mouse ---- */

bool filter_panel_handle_mouse(NcApp *app, View *v, const UiEvent *ev, Rect r) {
  if (ev->type != UI_EVENT_MOUSE || r.rows <= 0) return false;
  int x = ev->mouse.x, y = ev->mouse.y;
  if (y < r.y || y >= r.y + r.rows || x < r.x || x >= r.x + r.cols) return false;
  if (ev->mouse.action != UI_MOUSE_PRESS) return true;
  ensure_row(v);
  if (ev->mouse.button == UI_MOUSE_SCROLL_UP || ev->mouse.button == UI_MOUSE_SCROLL_DOWN) {
    if (ev->mouse.button == UI_MOUSE_SCROLL_UP && v->fp.scroll) v->fp.scroll--;
    else if (ev->mouse.button == UI_MOUSE_SCROLL_DOWN && v->fp.scroll + (size_t)(r.rows - 1) < v->num_filters) v->fp.scroll++;
    return true;
  }
  if (ev->mouse.button != UI_MOUSE_LEFT || y == r.y) return true;
  size_t row = v->fp.scroll + (size_t)(y - r.y - 1);
  if (row >= v->num_filters) return true;
  const LaceFilter *f = &v->filters[row];
  FpLayout L;
  filter_panel_layout(r.cols, !is_raw(f) && lace_filter_op_two_values(f->op), &L);
  int rx = x - r.x;
  if (rx >= L.del_x) {
    if (v->fp.editing) edit_commit(app, v);
    v->fp.focused = true;
    v->fp.row = row;
    filter_panel_remove(app, v);
    return true;
  }
  int fld = rx >= L.val2_x && L.val2_w ? FP_VALUE2 : rx >= L.val_x ? FP_VALUE : rx >= L.op_x ? FP_OP : FP_COLUMN;
  if (!field_ok(f, fld)) fld = FP_COLUMN;
  bool again = v->fp.focused && !v->fp.editing && v->fp.row == row && v->fp.field == fld;
  if (v->fp.editing && !(v->fp.row == row && v->fp.field == fld)) edit_commit(app, v);
  focus_row(app, v, row, fld);
  f = &v->filters[row];
  if (again) { /* a second click on the field: edit or pick */
    if (fld == FP_VALUE) edit_start(v, f->value);
    else if (fld == FP_VALUE2) edit_start(v, f->value2);
    else open_picker(app, v, fld == FP_OP, y, x);
  }
  return true;
}
