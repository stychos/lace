/*
 * Lace
 * The sub-views of a table tab: the strip naming them on the first row of
 * the main window (Data, Structure, Indexes, Foreign keys, Information; a
 * database view's Definition in the Structure slot; a routine's or
 * trigger's Definition and Information), the read-only lists behind every
 * one but Data (rows from core/catalog_rows.c, read through db_compat when
 * first shown), and following foreign keys from the grid.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "core/catalog_rows.h"
#include "tui_internal.h"
#include "util/mem.h"
#include "views/list_picker.h"
#include <stdlib.h>
#include <string.h>

#define SV_COL_MAX 60 /* a cell wider than this is cut (the last column takes what is left) */

/* The strip's cells as last drawn, for clicks */
typedef struct { int x0, x1; TabSubView sv; } StripCell;
static StripCell g_strip[SUBVIEW_COUNT];
static size_t g_strip_n;

int tui_subview_strip_rows(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  return tab && tab->type == TAB_TYPE_TABLE && state->main_win ? 1 : 0;
}

static const char *subview_label(const Tab *tab, TabSubView sv) {
  switch (sv) {
  case SUBVIEW_DATA: return "Data";
  case SUBVIEW_STRUCTURE: return tab->object == LACE_OBJ_TABLE ? "Structure" : "Definition";
  case SUBVIEW_INDEXES: return "Indexes";
  case SUBVIEW_FOREIGN_KEYS: return "Foreign keys";
  case SUBVIEW_INFORMATION: return "Information";
  default: return "";
  }
}

static HotkeyAction subview_action(TabSubView sv) {
  static const HotkeyAction a[SUBVIEW_COUNT] = {HOTKEY_SUBVIEW_DATA, HOTKEY_SUBVIEW_STRUCTURE, HOTKEY_SUBVIEW_INDEXES,
                                                HOTKEY_SUBVIEW_FOREIGN_KEYS, HOTKEY_SUBVIEW_INFORMATION};
  return a[sv];
}

void tui_set_subview(TuiState *state, TabSubView sv) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return;
  if (!tab_has_subview(tab, sv)) {
    tui_set_status(state, "A %s has no %s", catalog_object_word(tab->object), subview_label(tab, sv));
    return;
  }
  if (tab->subview == SUBVIEW_DATA && sv != SUBVIEW_DATA) { /* the grid goes behind the list */
    if (state->editing)
      tui_cancel_edit(state);
    if (state->adding_row)
      tui_cancel_add_row(state);
    tui_set_filters_focused(state, false);
  }
  tab->subview = sv;
}

/* ==========================================================================
 * The strip
 * ========================================================================== */

/* The first key of the action's binding when it is short enough to show */
static const char *key_hint(TuiState *state, HotkeyAction a) {
  const HotkeyBinding *b = &state->app->config->hotkeys[a];
  if (!b->num_keys || !b->keys[0] || strlen(b->keys[0]) > 3)
    return NULL;
  return b->keys[0];
}

void tui_draw_subview_strip(TuiState *state) {
  g_strip_n = 0;
  if (!tui_subview_strip_rows(state))
    return;
  Tab *tab = TUI_TAB(state);
  WINDOW *win = state->main_win;
  int rows, cols;
  getmaxyx(win, rows, cols);
  (void)rows;
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(win, 0, 0, ' ', cols);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));
  int x = 0;
  for (int i = 0; i < SUBVIEW_COUNT && x < cols; i++) {
    TabSubView sv = (TabSubView)i;
    if (!tab_has_subview(tab, sv))
      continue;
    const char *key = key_hint(state, subview_action(sv));
    char *label = key ? str_printf(" %s %s ", key, subview_label(tab, sv)) : str_printf(" %s ", subview_label(tab, sv));
    int w = utf8_width(label, strlen(label));
    if (x + w > cols)
      w = cols - x;
    bool cur = sv == tab->subview;
    /* the current cell in reverse video, not the cursor's black on cyan */
    attr_t attr = cur ? A_REVERSE | A_BOLD : COLOR_PAIR(COLOR_BORDER);
    wattron(win, attr);
    tui_print_fit(win, 0, x, w, label);
    wattroff(win, attr);
    if (g_strip_n < SUBVIEW_COUNT)
      g_strip[g_strip_n++] = (StripCell){x, x + w, sv};
    x += w;
    if (x < cols) {
      wattron(win, COLOR_PAIR(COLOR_BORDER));
      mvwaddch(win, 0, x, ACS_VLINE);
      wattroff(win, COLOR_PAIR(COLOR_BORDER));
      x++;
    }
    free(label);
  }
}

/* ==========================================================================
 * The rows
 * ========================================================================== */

/* A trigger restored without its table: the object list knows it */
static void fill_object_table(TuiState *state, Tab *tab) {
  if (tab->object != LACE_OBJ_TRIGGER || tab->object_table)
    return;
  Connection *conn = TUI_TAB_CONNECTION(state);
  for (size_t i = 0; conn && i < conn->num_objects; i++) {
    const LaceObject *o = &conn->objects[i];
    if (o->kind == LACE_OBJ_TRIGGER && o->name && tab->table_name && !strcmp(o->name, tab->table_name) && o->table) {
      tab->object_table = str_dup(o->table);
      return;
    }
  }
}

static void load_definition(TuiState *state, Tab *tab) {
  if (tab->definition || tab->definition_error)
    return;
  DbConnection *conn = TUI_CONN(state);
  if (!conn)
    return;
  fill_object_table(state, tab);
  if (tab->object == LACE_OBJ_ROUTINE && !tab->object_detail) {
    Connection *c = TUI_TAB_CONNECTION(state);
    for (size_t i = 0; c && i < c->num_objects; i++)
      if (c->objects[i].kind == LACE_OBJ_ROUTINE && c->objects[i].name && !strcmp(c->objects[i].name, tab->table_name) &&
          c->objects[i].detail) {
        tab->object_detail = str_dup(c->objects[i].detail);
        break;
      }
  }
  LaceObject obj = {.kind = tab->object, .name = tab->table_name, .table = tab->object_table, .detail = tab->object_detail};
  char *err = NULL;
  if (!db_object_definition(conn, &obj, &tab->definition, &err)) {
    tab->definition_error = err ? err : str_dup("Cannot read the definition");
    return;
  }
  if (!tab->definition)
    tab->definition = str_dup("");
}

static void load_deps(TuiState *state, Tab *tab) {
  if (tab->deps_loaded || tab->deps_error)
    return;
  DbConnection *conn = TUI_CONN(state);
  if (!conn || !tab->table_name)
    return;
  char *err = NULL;
  if (!db_table_dependents(conn, tab->table_name, &tab->deps, &tab->num_deps, &err)) {
    tab->deps_error = err ? err : str_dup("Cannot read the referencing tables");
    return;
  }
  tab->deps_loaded = true;
}

static void load_info(TuiState *state, Tab *tab) {
  if (tab->info || tab->info_error || !(tab->object == LACE_OBJ_TABLE || tab->object == LACE_OBJ_VIEW))
    return;
  DbConnection *conn = TUI_CONN(state);
  if (!conn || !tab->table_name)
    return;
  char *err = NULL;
  if (!db_table_info(conn, tab->table_name, &tab->info, &tab->autoinc, &err))
    tab->info_error = err ? err : str_dup("Cannot read the table's information");
}

/* False with *message (allocated) when there is only a message to show */
static bool build_rows(TuiState *state, Tab *tab, CatalogRows *t, char **message) {
  catalog_rows_init(t, 0, NULL);
  bool data_like = tab_has_subview(tab, SUBVIEW_DATA);
  if (data_like && tab->table_error) {
    *message = str_dup(tab->table_error);
    return false;
  }
  if (data_like && !tab->schema) {
    *message = str_dup("No schema");
    return false;
  }
  int cols = 80;
  if (state->main_win) {
    int r;
    getmaxyx(state->main_win, r, cols);
    (void)r;
  }
  switch (tab->subview) {
  case SUBVIEW_STRUCTURE:
    if (tab->object == LACE_OBJ_TABLE) {
      if (!tab->schema->num_columns) {
        *message = str_dup("No columns");
        return false;
      }
      catalog_rows_structure(t, tab->schema);
      return true;
    }
    load_definition(state, tab);
    if (tab->definition_error) {
      *message = str_dup(tab->definition_error);
      return false;
    }
    catalog_rows_definition(t, tab->definition, cols - 2, tab->object == LACE_OBJ_VIEW ? tab->schema : NULL);
    return true;
  case SUBVIEW_INDEXES:
    if (!catalog_rows_indexes(t, tab->schema)) {
      *message = str_dup("No indexes or constraints");
      return false;
    }
    return true;
  case SUBVIEW_FOREIGN_KEYS:
    load_deps(state, tab);
    catalog_rows_fks(t, tab->schema, tab->table_name, tab->deps, tab->num_deps, tab->deps_loaded, tab->deps_error);
    return true;
  case SUBVIEW_INFORMATION: {
    load_info(state, tab);
    Connection *conn = TUI_TAB_CONNECTION(state);
    /* the exact count: asked for here, or Data's unfiltered total when it was counted */
    int64_t exact = tab->info_count >= 0 ? tab->info_count
                    : data_like && !tab->row_count_approximate && tab->unfiltered_total_rows > 0
                        ? (int64_t)tab->unfiltered_total_rows
                        : -1;
    catalog_rows_info(t, tab->object, tab->table_name, tab->object_table, tab->object_detail, tab->info, tab->autoinc,
                      tab->info_error, exact, false, conn ? conn->objects : NULL, conn ? conn->num_objects : 0);
    return true;
  }
  default:
    *message = str_dup("");
    return false;
  }
}

/* Clamp the cursor to a selectable row (the nearest one after it, else before) */
static void clamp_cursor(const CatalogRows *t, size_t *cursor) {
  if (!t->nrows) {
    *cursor = 0;
    return;
  }
  if (*cursor >= t->nrows)
    *cursor = t->nrows - 1;
  for (size_t r = *cursor; r < t->nrows; r++)
    if (catalog_rows_selectable(t, r)) {
      *cursor = r;
      return;
    }
  for (size_t r = *cursor + 1; r-- > 0;)
    if (catalog_rows_selectable(t, r)) {
      *cursor = r;
      return;
    }
}

static void move_cursor(const CatalogRows *t, size_t *cursor, long delta) {
  if (!t->nrows)
    return;
  long c = (long)*cursor, step = delta < 0 ? -1 : 1, left = delta < 0 ? -delta : delta, last = c;
  while (left > 0) {
    c += step;
    if (c < 0 || c >= (long)t->nrows)
      break;
    if (catalog_rows_selectable(t, (size_t)c)) {
      last = c;
      left--;
    }
  }
  *cursor = (size_t)last;
}

static int cell_width(const char *s, bool last) {
  int w = utf8_width(s, strlen(s));
  return !last && w > SV_COL_MAX ? SV_COL_MAX : w;
}

void tui_draw_subview(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  WINDOW *win = state->main_win;
  if (!tab || !win)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  int top = tui_subview_strip_rows(state);
  for (int y = top; y < rows; y++)
    mvwhline(win, y, 0, ' ', cols);
  CatalogRows t;
  char *msg = NULL;
  if (!build_rows(state, tab, &t, &msg)) {
    const char *m = msg ? msg : "Nothing to show";
    int w = utf8_width(m, strlen(m));
    if (w > cols)
      w = cols;
    tui_print_fit(win, top + (rows - top) / 2, (cols - w) / 2, w, m);
    free(msg);
    catalog_rows_free(&t);
    return;
  }
  int *w = safe_calloc(t.ncols ? t.ncols : 1, sizeof *w);
  for (size_t i = 0; t.head && i < t.ncols; i++)
    w[i] = cell_width(t.head[i], i + 1 == t.ncols);
  for (size_t r = 0; r < t.nrows; r++) {
    if (t.span[r])
      continue;
    for (size_t i = 0; i < t.ncols; i++) {
      int cw = cell_width(t.cells[r][i], i + 1 == t.ncols);
      if (cw > w[i])
        w[i] = cw;
    }
  }
  int y = top;
  if (t.head) {
    wattron(win, A_BOLD | A_UNDERLINE);
    int x = 1;
    for (size_t i = 0; i < t.ncols && x < cols; i++) {
      int cw = w[i] < cols - x ? w[i] : cols - x;
      tui_print_fit(win, y, x, cw, t.head[i]);
      x += w[i] + 2;
    }
    wattroff(win, A_BOLD | A_UNDERLINE);
    y++;
  }
  size_t *cursor = &tab->sv_cursor[tab->subview], *scroll = &tab->sv_scroll[tab->subview];
  int vis = rows - y;
  clamp_cursor(&t, cursor);
  if (vis >= 1) {
    if (*cursor < *scroll)
      *scroll = *cursor;
    if (*cursor >= *scroll + (size_t)vis)
      *scroll = *cursor - (size_t)vis + 1;
    if (t.nrows <= (size_t)vis)
      *scroll = 0;
  }
  bool focused = !tui_sidebar_focused(state);
  for (int i = 0; i < vis; i++, y++) {
    size_t r = *scroll + (size_t)i;
    if (r >= t.nrows)
      break;
    bool cur = r == *cursor && !t.heading[r] && focused;
    attr_t attr = t.heading[r] ? A_BOLD : cur ? A_REVERSE : A_NORMAL;
    wattron(win, attr);
    if (cur)
      mvwhline(win, y, 0, ' ', cols);
    if (t.span[r]) {
      tui_print_fit(win, y, 1, cols - 1, t.cells[r][0]);
    } else {
      int x = 1;
      for (size_t c = 0; c < t.ncols && x < cols; c++) {
        int cw = w[c] < cols - x ? w[c] : cols - x;
        tui_print_fit(win, y, x, cw, t.cells[r][c]);
        x += w[c] + 2;
      }
    }
    wattroff(win, attr);
  }
  free(w);
  catalog_rows_free(&t);
}

/* ==========================================================================
 * Keys, mouse, actions
 * ========================================================================== */

/* Open an object in a new tab (the lists' Enter) */
static void open_in_new_tab(TuiState *state, LaceObjectKind kind, const char *name, const char *table,
                            const char *detail, TabSubView sv) {
  char *n = str_dup(name), *t = table ? str_dup(table) : NULL, *d = detail ? str_dup(detail) : NULL;
  if (tab_create_object(state, kind, n, t, d)) {
    Tab *nt = TUI_TAB(state);
    if (nt && tab_has_subview(nt, sv))
      nt->subview = sv;
  }
  free(n);
  free(t);
  free(d);
}

static void activate(TuiState *state, Tab *tab, int tag) {
  if (tag == CATALOG_TAG_ROWS && tab->subview == SUBVIEW_INFORMATION) {
    DbConnection *conn = TUI_CONN(state);
    size_t count = 0;
    bool approx = false;
    char *err = NULL;
    if (conn && db_count_rows(conn, tab->table_name, &count, &approx, &err)) {
      tab->info_count = (int64_t)count;
    } else {
      tui_set_error(state, "Count failed: %s", err ? err : "unknown error");
    }
    free(err);
    return;
  }
  if (tag >= CATALOG_TAG_TRIGGER) {
    Connection *conn = TUI_TAB_CONNECTION(state);
    size_t i = (size_t)(tag - CATALOG_TAG_TRIGGER);
    if (!conn || i >= conn->num_objects || conn->objects[i].kind != LACE_OBJ_TRIGGER)
      return;
    open_in_new_tab(state, LACE_OBJ_TRIGGER, conn->objects[i].name, conn->objects[i].table, NULL, SUBVIEW_STRUCTURE);
    return;
  }
  if (tag >= CATALOG_TAG_DEP && tag < CATALOG_TAG_TRIGGER) {
    size_t i = (size_t)(tag - CATALOG_TAG_DEP);
    if (i >= tab->num_deps || !tab->deps[i].table)
      return;
    open_in_new_tab(state, LACE_OBJ_TABLE, tab->deps[i].table, NULL, NULL, SUBVIEW_FOREIGN_KEYS);
  }
}

static bool list_move(TuiState *state, Tab *tab, long delta, bool to_end) {
  CatalogRows t;
  char *msg = NULL;
  bool ok = build_rows(state, tab, &t, &msg);
  free(msg);
  size_t *cursor = &tab->sv_cursor[tab->subview];
  if (ok) {
    if (to_end) {
      *cursor = delta < 0 ? 0 : t.nrows ? t.nrows - 1 : 0;
      clamp_cursor(&t, cursor);
      if (delta < 0)
        for (size_t r = 0; r < t.nrows; r++)
          if (catalog_rows_selectable(&t, r)) {
            *cursor = r;
            break;
          }
    } else {
      clamp_cursor(&t, cursor);
      move_cursor(&t, cursor, delta);
    }
  }
  catalog_rows_free(&t);
  return true;
}

static bool list_activate(TuiState *state, Tab *tab) {
  CatalogRows t;
  char *msg = NULL;
  bool ok = build_rows(state, tab, &t, &msg);
  free(msg);
  int tag = CATALOG_TAG_NONE;
  size_t cursor = tab->sv_cursor[tab->subview];
  if (ok) {
    clamp_cursor(&t, &cursor);
    if (cursor < t.nrows && catalog_rows_selectable(&t, cursor))
      tag = t.tag[cursor];
  }
  catalog_rows_free(&t);
  if (tag != CATALOG_TAG_NONE)
    activate(state, tab, tag);
  return true;
}

void tui_subview_refresh(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return;
  tab_drop_catalog(tab);
  if (tab_has_subview(tab, SUBVIEW_DATA) && tab->table_name)
    tui_load_table_data(state, tab->table_name); /* the schema, keys and indexes again */
}

bool tui_handle_subview_input(TuiState *state, const UiEvent *event) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE || !event || event->type != UI_EVENT_KEY)
    return false;
  if (tui_sidebar_focused(state))
    return false;
  const Config *cfg = state->app->config;
  for (int i = 0; i < SUBVIEW_COUNT; i++)
    if (hotkey_matches(cfg, event, subview_action((TabSubView)i))) {
      tui_set_subview(state, (TabSubView)i);
      return true;
    }
  if (tab->subview == SUBVIEW_DATA)
    return false;
  /* a list: the moves, Enter, refresh; the filter keys go back to Data */
  int page = 1;
  if (state->main_win) {
    int r, c;
    getmaxyx(state->main_win, r, c);
    (void)c;
    page = r > 3 ? r - 3 : 1;
  }
  if (render_event_is_special(event, UI_KEY_UP) || hotkey_matches(cfg, event, HOTKEY_MOVE_UP))
    return list_move(state, tab, -1, false);
  if (render_event_is_special(event, UI_KEY_DOWN) || hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN))
    return list_move(state, tab, 1, false);
  if (render_event_is_special(event, UI_KEY_PAGEUP) || hotkey_matches(cfg, event, HOTKEY_PAGE_UP))
    return list_move(state, tab, -page, false);
  if (render_event_is_special(event, UI_KEY_PAGEDOWN) || hotkey_matches(cfg, event, HOTKEY_PAGE_DOWN))
    return list_move(state, tab, page, false);
  if (render_event_is_special(event, UI_KEY_HOME) || hotkey_matches(cfg, event, HOTKEY_FIRST_ROW))
    return list_move(state, tab, -1, true);
  if (render_event_is_special(event, UI_KEY_END) || hotkey_matches(cfg, event, HOTKEY_LAST_ROW))
    return list_move(state, tab, 1, true);
  if (render_event_is_special(event, UI_KEY_ENTER))
    return list_activate(state, tab);
  if (hotkey_matches(cfg, event, HOTKEY_REFRESH)) {
    tui_subview_refresh(state);
    return true;
  }
  if (hotkey_matches(cfg, event, HOTKEY_TOGGLE_FILTERS) || hotkey_matches(cfg, event, HOTKEY_ADD_FILTER)) {
    if (tab_has_subview(tab, SUBVIEW_DATA))
      tui_set_subview(state, SUBVIEW_DATA);
    return false; /* the filters open on Data */
  }
  /* the grid's own keys (edit, delete, copy, sort, ...) do not reach the grid behind the list */
  for (int a = 0; a < HOTKEY_COUNT; a++) {
    HotkeyAction act = (HotkeyAction)a;
    if (act == HOTKEY_TOGGLE_SIDEBAR || !hotkey_matches(cfg, event, act))
      continue;
    HotkeyCategory cat = hotkey_get_category(act);
    if (cat == HOTKEY_CAT_TABLE || cat == HOTKEY_CAT_NAVIGATION || cat == HOTKEY_CAT_FILTERS)
      return true;
  }
  return false;
}

bool tui_subview_click(TuiState *state, int rel_x, int rel_y) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return false;
  int top = tui_subview_strip_rows(state);
  if (top && rel_y == 0) {
    for (size_t i = 0; i < g_strip_n; i++)
      if (rel_x >= g_strip[i].x0 && rel_x < g_strip[i].x1) {
        tui_set_subview(state, g_strip[i].sv);
        break;
      }
    return true;
  }
  if (tab->subview == SUBVIEW_DATA)
    return false;
  CatalogRows t;
  char *msg = NULL;
  bool ok = build_rows(state, tab, &t, &msg);
  free(msg);
  if (ok) {
    int first = top + (t.head ? 1 : 0);
    if (rel_y >= first) {
      size_t r = tab->sv_scroll[tab->subview] + (size_t)(rel_y - first);
      if (r < t.nrows && catalog_rows_selectable(&t, r))
        tab->sv_cursor[tab->subview] = r;
    }
  }
  catalog_rows_free(&t);
  return true;
}

/* ==========================================================================
 * Following foreign keys
 * ========================================================================== */

/* The rows and schema the cursor is on: a table tab's Data, or query
 * results that are one table's rows (the editing rule) */
static bool follow_source(TuiState *state, Tab *tab, ResultSet **data, const LaceSchema **schema, size_t *row,
                          size_t *col, const char **table) {
  if (tab->type == TAB_TYPE_TABLE && tab->subview == SUBVIEW_DATA && tab_has_subview(tab, SUBVIEW_DATA)) {
    *data = tab->data;
    *schema = tab->schema;
    *row = tab->cursor_row;
    *col = tab->cursor_col;
    *table = tab->table_name;
  } else if (tab->type == TAB_TYPE_QUERY && tab->query_source_table) {
    *data = tab->query_results;
    *schema = tab->query_source_schema;
    *row = tab->query_result_row;
    *col = tab->query_result_col;
    *table = tab->query_source_table;
  } else {
    tui_set_status(state, "Foreign keys are followed from a table's rows");
    return false;
  }
  if (!*schema) {
    tui_set_status(state, "The table is still loading");
    return false;
  }
  if (!*data || !(*data)->rows || *row >= (*data)->num_rows || *col >= (*data)->num_columns) {
    tui_set_status(state, "No row to follow");
    return false;
  }
  return true;
}

/* The cursor row's values of the named columns as edit text (allocated;
 * NULL with a status when one is missing or NULL) */
static char **row_values(TuiState *state, const ResultSet *data, size_t row, char *const *columns, size_t n) {
  char **vals = safe_calloc(n + 1, sizeof *vals);
  for (size_t k = 0; k < n; k++) {
    size_t ci = SIZE_MAX;
    for (size_t c = 0; c < data->num_columns; c++)
      if (data->columns[c].name && columns[k] && !strcmp(data->columns[c].name, columns[k])) {
        ci = c;
        break;
      }
    const LaceValue *cell = ci == SIZE_MAX || ci >= data->rows[row].num_cells ? NULL : &data->rows[row].cells[ci];
    const char *why = ci == SIZE_MAX ? "is not in the results"
                      : !cell ? "is not loaded"
                      : cell->is_null ? "is NULL: nothing to follow"
                      : cell->omitted ? "is too large to load"
                                      : NULL;
    if (why) {
      tui_set_status(state, "%s %s", columns[k], why);
      for (size_t j = 0; j < k; j++)
        str_secure_free(vals[j]);
      free(vals);
      return NULL;
    }
    vals[k] = lace_value_to_edit_text(cell);
    if (!vals[k])
      vals[k] = str_dup("");
  }
  return vals;
}

static void free_values(char **vals, size_t n) {
  for (size_t k = 0; vals && k < n; k++)
    str_secure_free(vals[k]);
  free(vals);
}

/* Open table filtered by columns = values: an open tab of it on this
 * connection gets the filters, else a new tab */
static void open_filtered(TuiState *state, const char *table, char *const *columns, char *const *values, size_t n) {
  Workspace *ws = TUI_WORKSPACE(state);
  Tab *cur = TUI_TAB(state);
  if (!ws || !cur)
    return;
  size_t conn_idx = cur->connection_index;
  char *name = str_dup(table);
  bool found = false;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    Tab *t = &ws->tabs[i];
    if (t->active && t->type == TAB_TYPE_TABLE && t->object == LACE_OBJ_TABLE && t->table_name &&
        !strcmp(t->table_name, name) && t->connection_index == conn_idx) {
      tab_switch(state, i);
      found = true;
      break;
    }
  }
  if (!found && !tab_create_object(state, LACE_OBJ_TABLE, name, NULL, NULL)) {
    free(name);
    return;
  }
  Tab *tab = TUI_TAB(state);
  if (!tab || !tab->schema) {
    free(name);
    return;
  }
  tab->subview = SUBVIEW_DATA;
  filters_clear(&tab->filters);
  for (size_t k = 0; k < n; k++) {
    size_t idx = db_schema_column_index(tab->schema, columns[k]);
    if (idx == SIZE_MAX) {
      tui_set_error(state, "%s has no column %s", name, columns[k]);
      free(name);
      return;
    }
    filters_add(&tab->filters, idx, FILTER_OP_EQ, values[k]);
  }
  tui_apply_filters(state);
  char *cond = str_dup("");
  for (size_t k = 0; k < n; k++) {
    char *next = str_printf("%s%s%s = %s", cond, k ? ", " : "", columns[k], values[k]);
    free(cond);
    cond = next;
  }
  tui_set_status(state, "%s where %s", name, cond);
  free(cond);
  free(name);
}

static int pick(TuiState *state, const char *title, char **items, size_t n) {
  int x = state->sidebar_visible ? SIDEBAR_WIDTH + 2 : 2;
  return tui_pick_from_list(state->app->config, title, (const char *const *)items, n, 0, 3, x, 30,
                            state->term_cols - 4, state->term_rows - 6);
}

void tui_follow_fk(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  ResultSet *data;
  const LaceSchema *schema;
  size_t row, col;
  const char *table;
  if (!tab || !follow_source(state, tab, &data, &schema, &row, &col, &table))
    return;
  const char *name = data->columns[col].name;
  const LaceForeignKey *fks[LACE_MAX_PK_COLUMNS + 8];
  size_t n = 0;
  for (size_t i = 0; name && i < schema->num_foreign_keys && n < sizeof fks / sizeof *fks; i++) {
    const LaceForeignKey *fk = &schema->foreign_keys[i];
    if (fk->num_columns && fk->columns[0] && !strcmp(fk->columns[0], name))
      fks[n++] = fk;
  }
  if (!n) {
    tui_set_status(state, "%s is not a foreign key column", name ? name : "This column");
    return;
  }
  size_t which = 0;
  if (n > 1) {
    char **items = safe_calloc(n, sizeof *items);
    for (size_t i = 0; i < n; i++) {
      char *cols = catalog_join(fks[i]->ref_columns, fks[i]->num_ref_columns);
      items[i] = str_printf("%s (%s)%s%s", fks[i]->ref_table ? fks[i]->ref_table : "?", cols, fks[i]->name ? "  " : "",
                            fks[i]->name ? fks[i]->name : "");
      free(cols);
    }
    int sel = pick(state, "Follow which key?", items, n);
    for (size_t i = 0; i < n; i++)
      free(items[i]);
    free(items);
    if (sel < 0)
      return;
    which = (size_t)sel;
  }
  const LaceForeignKey *fk = fks[which];
  if (!fk->ref_table || fk->num_ref_columns != fk->num_columns) {
    tui_set_error(state, "The foreign key %s is incomplete", fk->name ? fk->name : "");
    return;
  }
  char **vals = row_values(state, data, row, fk->columns, fk->num_columns);
  if (!vals)
    return;
  /* copies: opening a tab may reload the schema the key lives in */
  size_t nc = fk->num_columns;
  char **cols = safe_calloc(nc + 1, sizeof *cols);
  for (size_t k = 0; k < nc; k++)
    cols[k] = str_dup(fk->ref_columns[k]);
  char *ref = str_dup(fk->ref_table);
  open_filtered(state, ref, cols, vals, nc);
  free(ref);
  for (size_t k = 0; k < nc; k++)
    free(cols[k]);
  free(cols);
  free_values(vals, nc);
}

void tui_referenced_by(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  ResultSet *data;
  const LaceSchema *schema;
  size_t row, col;
  const char *table;
  if (!tab || !follow_source(state, tab, &data, &schema, &row, &col, &table))
    return;
  /* the dependents: a table tab keeps them, a query tab reads them now */
  LaceDependent *deps = tab->type == TAB_TYPE_TABLE ? tab->deps : NULL;
  size_t num_deps = tab->type == TAB_TYPE_TABLE ? tab->num_deps : 0;
  LaceDependent *own = NULL;
  size_t num_own = 0;
  if (tab->type == TAB_TYPE_TABLE) {
    load_deps(state, tab);
    if (tab->deps_error) {
      tui_set_error(state, "%s", tab->deps_error);
      return;
    }
    deps = tab->deps;
    num_deps = tab->num_deps;
  } else {
    char *err = NULL;
    if (!db_table_dependents(TUI_CONN(state), table, &own, &num_own, &err)) {
      tui_set_error(state, "%s", err ? err : "Cannot read the referencing tables");
      free(err);
      return;
    }
    deps = own;
    num_deps = num_own;
  }
  if (!num_deps) {
    tui_set_status(state, "No table references %s", table);
    lace_dependents_free(own, num_own);
    return;
  }
  char **items = safe_calloc(num_deps, sizeof *items);
  for (size_t i = 0; i < num_deps; i++) {
    char *cols = catalog_join(deps[i].columns, deps[i].num_columns);
    items[i] = str_printf("%s (%s)%s%s", deps[i].table ? deps[i].table : "?", cols,
                          deps[i].on_delete ? "  ON DELETE " : "", deps[i].on_delete ? deps[i].on_delete : "");
    free(cols);
  }
  int sel = pick(state, "Referenced by", items, num_deps);
  for (size_t i = 0; i < num_deps; i++)
    free(items[i]);
  free(items);
  if (sel >= 0 && (size_t)sel < num_deps) {
    const LaceDependent *d = &deps[sel];
    size_t n = d->num_columns;
    char **vals = row_values(state, data, row, d->ref_columns, n);
    if (vals) {
      char **cols = safe_calloc(n + 1, sizeof *cols);
      for (size_t k = 0; k < n; k++)
        cols[k] = str_dup(d->columns[k]);
      char *dt = str_dup(d->table ? d->table : "");
      open_filtered(state, dt, cols, vals, n);
      free(dt);
      for (size_t k = 0; k < n; k++)
        free(cols[k]);
      free(cols);
      free_values(vals, n);
    }
  }
  lace_dependents_free(own, num_own);
}
