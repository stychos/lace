#include "model/view.h"
#include "core/utf8.h"
#include "keyset.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

#define MAX_COL_WIDTH 200

/* Main thread only */
static uint64_t g_next_uid = 1;

static View *view_new(ViewKind kind) {
  View *v = safe_calloc(1, sizeof *v);
  v->kind = kind;
  v->uid = g_next_uid++;
  v->cache = page_cache_new(TUI_PAGE_SIZE);
  v->text_sel_anchor = SIZE_MAX;
  v->affected = -1;
  return v;
}

View *view_new_table(const char *table) {
  View *v = view_new(VIEW_TABLE);
  v->table = str_dup(table ? table : "");
  return v;
}

View *view_new_query(const char *text) {
  View *v = view_new(VIEW_QUERY);
  v->text = str_dup(text ? text : "");
  v->text_cursor = strlen(v->text);
  return v;
}

static void free_col_names(View *v) {
  for (size_t i = 0; i < v->num_col_names; i++) free(v->col_names[i]);
  free(v->col_names);
  v->col_names = NULL;
  v->num_col_names = 0;
}

void view_clear_filters(View *v) {
  for (size_t i = 0; i < v->num_filters; i++) lace_filter_clear(&v->filters[i]);
  free(v->filters);
  v->filters = NULL;
  v->num_filters = 0;
}

void view_clear_sorts(View *v) {
  for (size_t i = 0; i < v->num_sorts; i++) free(v->sorts[i].column);
  free(v->sorts);
  v->sorts = NULL;
  v->num_sorts = 0;
}

void view_checkpoint_drop(View *v) {
  for (size_t i = 0; i < v->cp_num_sorts; i++) free(v->cp_sorts[i].column);
  free(v->cp_sorts);
  for (size_t i = 0; i < v->cp_num_filters; i++) lace_filter_clear(&v->cp_filters[i]);
  free(v->cp_filters);
  v->cp_sorts = NULL;
  v->cp_filters = NULL;
  v->cp_num_sorts = v->cp_num_filters = 0;
  v->has_checkpoint = false;
}

bool view_fail_blocks(const View *v) {
  return v->fail_set && v->fail_gen == page_cache_generation(v->cache) &&
         v->fail_cursor == v->cursor_row && v->fail_scroll == v->scroll_row;
}

bool view_checkpoint(View *v) {
  if (v->has_checkpoint) return false;
  v->has_checkpoint = true;
  if (v->num_sorts) v->cp_sorts = safe_calloc(v->num_sorts, sizeof *v->cp_sorts);
  for (size_t i = 0; i < v->num_sorts; i++) {
    v->cp_sorts[i].column = str_dup(v->sorts[i].column);
    v->cp_sorts[i].desc = v->sorts[i].desc;
  }
  v->cp_num_sorts = v->num_sorts;
  if (v->num_filters) v->cp_filters = safe_calloc(v->num_filters, sizeof *v->cp_filters);
  for (size_t i = 0; i < v->num_filters; i++) {
    const LaceFilter *f = &v->filters[i];
    v->cp_filters[i].column = f->column ? str_dup(f->column) : NULL;
    v->cp_filters[i].op = f->op;
    v->cp_filters[i].value = f->value ? str_dup(f->value) : NULL;
    v->cp_filters[i].value2 = f->value2 ? str_dup(f->value2) : NULL;
  }
  v->cp_num_filters = v->num_filters;
  v->cp_cursor_row = v->cursor_row;
  v->cp_cursor_col = v->cursor_col;
  v->cp_scroll_row = v->scroll_row;
  v->cp_scroll_col = v->scroll_col;
  return true;
}

bool view_revert(View *v) {
  if (!v->has_checkpoint) return false;
  view_clear_sorts(v);
  view_clear_filters(v);
  v->sorts = v->cp_sorts;
  v->num_sorts = v->cp_num_sorts;
  v->filters = v->cp_filters;
  v->num_filters = v->cp_num_filters;
  v->cp_sorts = NULL;
  v->cp_filters = NULL;
  v->cp_num_sorts = v->cp_num_filters = 0;
  v->cursor_row = v->cp_cursor_row;
  v->cursor_col = v->cp_cursor_col;
  v->scroll_row = v->cp_scroll_row;
  v->scroll_col = v->cp_scroll_col;
  v->has_checkpoint = false;
  return true;
}

void view_add_raw_filter(View *v, const char *condition) {
  if (!condition || !*condition) return;
  v->filters = safe_realloc(v->filters, (v->num_filters + 1) * sizeof(LaceFilter));
  LaceFilter *f = &v->filters[v->num_filters++];
  memset(f, 0, sizeof *f);
  f->op = LACE_FILTER_RAW;
  f->value = str_dup(condition);
}

void view_free(View *v) {
  if (!v) return;
  view_checkpoint_drop(v);
  free(v->fp.text);
  free(v->fetch_ctx);
  free(v->table);
  view_clear_filters(v);
  free(v->search);
  view_clear_sorts(v);
  lace_schema_free(v->schema);
  free(v->schema_error);
  free(v->source_table);
  page_cache_free(v->cache);
  free(v->selected);
  free(v->widths);
  free(v->user_width);
  free(v->error);
  str_secure_free(v->edit_text);
  str_secure_free(v->text);
  str_secure_free(v->base_sql);
  lace_result_free(v->once);
  undo_stack_clear(&v->undo);
  free_col_names(v);
  free(v);
}

void view_set_schema(View *v, LaceSchema *schema) {
  if (v->schema != schema) lace_schema_free(v->schema);
  v->schema = schema;
}

static void ensure_widths(View *v) {
  size_t n = page_cache_num_columns(v->cache);
  if (v->num_widths == n) return;
  free(v->widths);
  free(v->user_width);
  v->widths = n ? safe_calloc(n, sizeof(int)) : NULL;
  v->user_width = n ? safe_calloc(n, sizeof(bool)) : NULL;
  v->num_widths = n;
  for (size_t c = 0; c < n; c++) {
    const LaceColumn *col = page_cache_column(v->cache, c);
    int w = col && col->name ? utf8_width(col->name, strlen(col->name)) : 0;
    if (w < MIN_COL_WIDTH) w = MIN_COL_WIDTH;
    if (w > DEFAULT_COL_WIDTH) w = DEFAULT_COL_WIDTH;
    v->widths[c] = w;
  }
}

bool view_columns_changed(View *v) {
  size_t n = page_cache_num_columns(v->cache);
  bool same = n == v->num_col_names;
  for (size_t i = 0; same && i < n; i++) {
    const LaceColumn *c = page_cache_column(v->cache, i);
    const char *name = c && c->name ? c->name : "";
    same = strcmp(name, v->col_names[i]) == 0;
  }
  if (same) return false;
  free_col_names(v);
  if (n) v->col_names = safe_calloc(n, sizeof(char *));
  for (size_t i = 0; i < n; i++) {
    const LaceColumn *c = page_cache_column(v->cache, i);
    v->col_names[i] = str_dup(c && c->name ? c->name : "");
  }
  v->num_col_names = n;
  free(v->widths);
  free(v->user_width);
  v->widths = NULL;
  v->user_width = NULL;
  v->num_widths = 0;
  ensure_widths(v);
  if (n == 0) v->cursor_col = 0;
  else if (v->cursor_col >= n) v->cursor_col = n - 1;
  if (v->scroll_col >= (n ? n : 1)) v->scroll_col = n ? n - 1 : 0;
  view_clear_selection(v);
  return true;
}

void view_measure(View *v, size_t from_row, size_t to_row) {
  ensure_widths(v);
  for (size_t c = 0; c < v->num_widths; c++) {
    if (v->user_width[c]) continue;
    const LaceColumn *col = page_cache_column(v->cache, c);
    int w = v->widths[c];
    int hw = col && col->name ? utf8_width(col->name, strlen(col->name)) : 0;
    if (hw > w) w = hw;
    for (size_t r = from_row; r < to_row; r++) {
      const LaceValue *cell = page_cache_cell(v->cache, r, c);
      if (!cell) continue;
      char *t = lace_value_display_text(cell, MAX_COL_WIDTH);
      if (t) {
        int tw = utf8_width(t, strlen(t));
        if (tw > w) w = tw;
        free(t);
      }
    }
    if (w > DEFAULT_COL_WIDTH) w = DEFAULT_COL_WIDTH;
    if (w > v->widths[c]) v->widths[c] = w;
  }
}

void view_set_width(View *v, size_t col, int width) {
  ensure_widths(v);
  if (col >= v->num_widths) return;
  if (width < MIN_COL_WIDTH) width = MIN_COL_WIDTH;
  if (width > MAX_COL_WIDTH) width = MAX_COL_WIDTH;
  v->widths[col] = width;
  v->user_width[col] = true;
}

void view_set_cursor(View *v, size_t row, size_t col) {
  size_t extent = page_cache_extent(v->cache);
  size_t ncols = page_cache_num_columns(v->cache);
  if (extent == 0) row = 0;
  else if (row >= extent) row = extent - 1;
  if (ncols == 0) col = 0;
  else if (col >= ncols) col = ncols - 1;
  v->cursor_row = row;
  v->cursor_col = col;
}

void view_move(View *v, long drow, long dcol) {
  long r = (long)v->cursor_row + drow, c = (long)v->cursor_col + dcol;
  if (r < 0) r = 0;
  if (c < 0) c = 0;
  view_set_cursor(v, (size_t)r, (size_t)c);
}

static int col_w(const View *v, size_t c) {
  return c < v->num_widths ? v->widths[c] : DEFAULT_COL_WIDTH;
}

void view_scroll_into_view(View *v, int rows_visible, int cols_avail, int gutter) {
  if (rows_visible < 1) rows_visible = 1;
  if (v->cursor_row < v->scroll_row) v->scroll_row = v->cursor_row;
  else if (v->cursor_row >= v->scroll_row + (size_t)rows_visible)
    v->scroll_row = v->cursor_row - (size_t)rows_visible + 1;

  if (v->cursor_col < v->scroll_col) v->scroll_col = v->cursor_col;
  while (v->scroll_col < v->cursor_col) {
    long edge = gutter;
    for (size_t c = v->scroll_col; c <= v->cursor_col; c++) edge += col_w(v, c) + 1;
    if (edge <= cols_avail) break;
    v->scroll_col++;
  }
}

size_t view_first_visible_col(const View *v) { return v->scroll_col; }

size_t view_last_visible_col(const View *v, int cols_avail, int gutter) {
  size_t ncols = page_cache_num_columns(v->cache);
  if (ncols == 0) return 0;
  size_t last = v->scroll_col < ncols ? v->scroll_col : ncols - 1;
  long edge = gutter + col_w(v, last) + 1;
  while (last + 1 < ncols) {
    edge += col_w(v, last + 1) + 1;
    if (edge > cols_avail) break;
    last++;
  }
  return last;
}

void view_toggle_select(View *v, size_t row) {
  size_t i = 0;
  while (i < v->num_selected && v->selected[i] < row) i++;
  if (i < v->num_selected && v->selected[i] == row) {
    memmove(v->selected + i, v->selected + i + 1, (v->num_selected - i - 1) * sizeof(size_t));
    v->num_selected--;
    return;
  }
  v->selected = safe_realloc(v->selected, (v->num_selected + 1) * sizeof(size_t));
  memmove(v->selected + i + 1, v->selected + i, (v->num_selected - i) * sizeof(size_t));
  v->selected[i] = row;
  v->num_selected++;
}

void view_clear_selection(View *v) {
  free(v->selected);
  v->selected = NULL;
  v->num_selected = 0;
}

bool view_is_selected(const View *v, size_t row) {
  for (size_t i = 0; i < v->num_selected; i++)
    if (v->selected[i] == row) return true;
  return false;
}

static ptrdiff_t find_sort(const View *v, const char *column) {
  for (size_t i = 0; i < v->num_sorts; i++)
    if (strcmp(v->sorts[i].column, column) == 0) return (ptrdiff_t)i;
  return -1;
}

void view_cycle_sort(View *v, const char *column) {
  ptrdiff_t i = find_sort(v, column);
  if (i < 0) {
    v->sorts = safe_realloc(v->sorts, (v->num_sorts + 1) * sizeof(LaceSortSpec));
    memmove(v->sorts + 1, v->sorts, v->num_sorts * sizeof(LaceSortSpec));
    v->sorts[0].column = str_dup(column);
    v->sorts[0].desc = false;
    v->num_sorts++;
  } else if (!v->sorts[i].desc) {
    v->sorts[i].desc = true;
  } else {
    free(v->sorts[i].column);
    memmove(v->sorts + i, v->sorts + i + 1, (v->num_sorts - (size_t)i - 1) * sizeof(LaceSortSpec));
    v->num_sorts--;
  }
}

void view_set_filter(View *v, const char *column, LaceFilterOp op, const char *value,
                     const char *value2) {
  if (!column) return;
  LaceFilter *f = NULL;
  for (size_t i = 0; i < v->num_filters; i++)
    if (v->filters[i].column && strcmp(v->filters[i].column, column) == 0) f = &v->filters[i];
  if (f) {
    lace_filter_clear(f);
  } else {
    v->filters = safe_realloc(v->filters, (v->num_filters + 1) * sizeof(LaceFilter));
    f = &v->filters[v->num_filters++];
    memset(f, 0, sizeof *f);
  }
  f->column = str_dup(column);
  f->op = op;
  f->value = value ? str_dup(value) : NULL;
  f->value2 = value2 ? str_dup(value2) : NULL;
}

size_t view_add_filter(View *v, const char *column) {
  v->filters = safe_realloc(v->filters, (v->num_filters + 1) * sizeof(LaceFilter));
  LaceFilter *f = &v->filters[v->num_filters];
  memset(f, 0, sizeof *f);
  f->column = column ? str_dup(column) : NULL;
  f->op = column ? LACE_FILTER_EQ : LACE_FILTER_RAW;
  f->value = str_dup("");
  return v->num_filters++;
}

void view_filter_set(View *v, size_t index, const char *column, LaceFilterOp op, const char *value,
                     const char *value2) {
  if (index >= v->num_filters) return;
  LaceFilter *f = &v->filters[index];
  /* the arguments may point into the filter itself */
  char *col = column && op != LACE_FILTER_RAW ? str_dup(column) : NULL;
  char *val = str_dup(value ? value : "");
  char *val2 = value2 ? str_dup(value2) : NULL;
  lace_filter_clear(f);
  f->column = col;
  f->op = column ? op : LACE_FILTER_RAW;
  f->value = val;
  f->value2 = val2;
}

bool view_filter_active(const LaceFilter *f) {
  const char *value = f->value ? f->value : "";
  if (f->op == LACE_FILTER_RAW || !f->column) {
    while (*value == ' ' || *value == '\t' || *value == '\n') value++;
    return *value != 0;
  }
  if (lace_filter_op_needs_value(f->op) && !*value) return false;
  if (lace_filter_op_two_values(f->op) && (!f->value2 || !*f->value2)) return false;
  return true;
}

void view_prune_filters(View *v) {
  for (size_t i = v->num_filters; i-- > 0;)
    if (!view_filter_active(&v->filters[i])) view_remove_filter(v, i);
}

void view_remove_filter(View *v, size_t index) {
  if (index >= v->num_filters) return;
  lace_filter_clear(&v->filters[index]);
  memmove(v->filters + index, v->filters + index + 1,
          (v->num_filters - index - 1) * sizeof(LaceFilter));
  v->num_filters--;
}

char *view_where(const View *v, LaceDriver d) {
  return lace_filter_build_where(v->filters, v->num_filters, v->search, v->schema, d);
}

char *view_order_by(const View *v, LaceDriver d) {
  if (v->schema && v->schema->num_columns) {
    LaceSort *ks_sorts = v->num_sorts ? safe_calloc(v->num_sorts, sizeof *ks_sorts) : NULL;
    size_t n = 0;
    for (size_t i = 0; i < v->num_sorts; i++) {
      for (size_t c = 0; c < v->schema->num_columns; c++) {
        const char *name = v->schema->columns[c].name;
        if (name && strcmp(name, v->sorts[i].column) == 0) {
          ks_sorts[n].column = c;
          ks_sorts[n].dir = v->sorts[i].desc ? LACE_SORT_DESC : LACE_SORT_ASC;
          n++;
          break;
        }
      }
    }
    LaceKeysetState *ks = lace_keyset_create(v->schema, ks_sorts, n, d);
    free(ks_sorts);
    if (ks) {
      char *order = lace_keyset_build_order(ks, true);
      lace_keyset_free(ks);
      return order;
    }
  }
  return lace_sort_build_order_by(v->sorts, v->num_sorts, d);
}

char *view_page_sql(const View *v, LaceDriver d, size_t page) {
  if (v->kind == VIEW_QUERY) {
    if (!v->base_sql) return NULL;
    return lace_sql_page_of_statement(v->base_sql, v->wrap, TUI_PAGE_SIZE, page * TUI_PAGE_SIZE);
  }
  char *where = view_where(v, d), *order = view_order_by(v, d);
  char *sql = lace_sql_table_page(d, v->table, where, order, TUI_PAGE_SIZE, page * TUI_PAGE_SIZE);
  free(where);
  free(order);
  return sql;
}

char *view_count_sql(const View *v, LaceDriver d) {
  if (v->kind == VIEW_QUERY) return v->base_sql ? lace_sql_count_of_statement(v->base_sql) : NULL;
  char *where = view_where(v, d);
  char *sql = lace_sql_table_count(d, v->table, where);
  free(where);
  return sql;
}

const char *view_title(const View *v) {
  if (v->kind == VIEW_TABLE) return v->table ? v->table : "";
  /* Valid until the next call on this thread */
  static _Thread_local char buf[256];
  const char *s = v->text ? v->text : "";
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  if (!*s) return "New query";
  size_t len = 0;
  while (s[len] && s[len] != '\n') len++;
  while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
  int cols;
  size_t fit = utf8_fit(s, len, 40, &cols);
  if (fit > sizeof buf - 1) fit = utf8_truncate(s, sizeof buf - 1);
  memcpy(buf, s, fit);
  buf[fit] = '\0';
  return buf;
}

bool view_row_numbers(const View *v, const Config *config) {
  if (!config) return true;
  int mode = config->general.row_numbers;
  if (mode == CONFIG_ROW_NUMBERS_NEVER) return false;
  if (mode == CONFIG_ROW_NUMBERS_ALWAYS || !v->schema) return true;
  size_t keys = 0;
  bool auto_int = false;
  for (size_t i = 0; i < v->schema->num_columns; i++) {
    const LaceColumn *c = &v->schema->columns[i];
    if (!c->primary_key) continue;
    keys++;
    auto_int = c->auto_increment && c->type == LACE_TYPE_INT;
  }
  return !(keys == 1 && auto_int);
}
