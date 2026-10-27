/*
 * Lace
 * Workspaces window: the workspaces manager. One list of the open
 * workspaces and the saved layouts, an input on top that searches it, a
 * details pane for the selection, and a footer with the keys that apply to
 * it. Connecting happens in connection tabs (form_tab.c), not here.
 *
 * It fills the terminal and follows its resizes. Every change to saved
 * workspaces is written at once.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_internal.h"
#include "text_input_draw.h"
#include "../config/session.h"
#include "util/json_helpers.h"
#include <stdint.h>

/* ============================================================================
 * Helpers
 * ============================================================================
 */

char *connect_flatten_message(const char *msg) {
  if (!msg)
    return NULL;
  /* Server messages come with newlines and tabs (libpq: "...\n\tIs the
   * server running..."): one line, runs of white space as one space */
  size_t n = strlen(msg);
  char *out = safe_malloc(n + 1);
  size_t o = 0;
  bool space = false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)msg[i];
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      space = o > 0;
      continue;
    }
    if (c < ' ')
      continue;
    if (space) {
      out[o++] = ' ';
      space = false;
    }
    out[o++] = (char)c;
  }
  out[o] = '\0';
  return out;
}

static void set_error(DialogState *ds, char *owned) {
  free(ds->error_msg);
  ds->error_msg = owned;
}

static void set_success(DialogState *ds, char *owned) {
  free(ds->success_msg);
  ds->success_msg = owned;
}

const char *connect_key_hint(const Config *config, HotkeyAction action,
                             const char *fallback) {
  static char buf[16];
  buf[0] = '\0';
  char *display = config ? hotkey_get_display(config, action) : NULL;
  if (display) {
    /* The first key only (before a comma) */
    size_t len = strcspn(display, ",");
    if (len > sizeof(buf) - 1)
      len = sizeof(buf) - 1;
    memcpy(buf, display, len);
    buf[len] = '\0';
    free(display);
  }
  return buf[0] ? buf : fallback;
}



static bool save_layouts(DialogState *ds) {
  char *err = NULL;
  if (saved_workspaces_save(&ds->saved_ws, &err))
    return true;
  set_error(ds, err);
  return false;
}

static const char *connect_filter(DialogState *ds) {
  return ds->input.len > 0 ? ds->input.buf : NULL;
}

static AppState *app_of(DialogState *ds) { return ds->state ? ds->state->app : NULL; }

static size_t num_workspaces(DialogState *ds) {
  AppState *app = app_of(ds);
  return app ? app->num_workspaces : 0;
}

/* "today 14:02", "yesterday", "3 days ago", "2025-06-01" */
static char *when(time_t t) {
  if (t <= 0)
    return str_dup("");
  time_t now = time(NULL);
  struct tm then_tm, now_tm;
  localtime_r(&t, &then_tm);
  localtime_r(&now, &now_tm);
  char buf[32];
  long days = (long)((now - t) / 86400);
  if (then_tm.tm_year == now_tm.tm_year && then_tm.tm_yday == now_tm.tm_yday) {
    strftime(buf, sizeof(buf), "today %H:%M", &then_tm);
    return str_dup(buf);
  }
  if (days <= 1)
    return str_dup("yesterday");
  if (days < 7)
    return str_printf("%ld days ago", days);
  strftime(buf, sizeof(buf), "%Y-%m-%d", &then_tm);
  return str_dup(buf);
}

/* Case-insensitive "needle in haystack" (NULL haystack: no) */
static bool matches(const char *haystack, const char *needle) {
  return haystack && needle && strcasestr(haystack, needle) != NULL;
}

/* ============================================================================
 * Workspaces, connections and what they are called
 * ============================================================================
 */

/* A short name for a connection string: a SQLite file's name, else
 * "database on host" (NULL if it does not parse) */
static char *short_name(const char *connstr) {
  ConnString *cs = connstr ? connstr_parse(connstr, NULL) : NULL;
  if (!cs)
    return NULL;
  char *out;
  if (cs->driver && str_eq(cs->driver, "sqlite")) {
    const char *db = cs->database ? cs->database : "";
    const char *slash = strrchr(db, '/');
    out = str_dup(slash && slash[1] ? slash + 1 : db);
  } else {
    const char *host = cs->host && cs->host[0] ? cs->host : "localhost";
    out = cs->database && cs->database[0] ? str_printf("%s on %s", cs->database, host)
                                          : str_dup(host);
  }
  connstr_free(cs);
  return out;
}

/* A saved connection's name, else a short name for the string */
static char *conn_name(DialogState *ds, const Connection *c) {
  if (!c)
    return NULL;
  if (c->saved_conn_id) {
    ConnectionItem *item = connmgr_find_by_id(ds->mgr, c->saved_conn_id);
    if (item && connmgr_is_connection(item) && item->connection.name &&
        item->connection.name[0])
      return str_dup(item->connection.name);
  }
  char *n = short_name(c->connstr);
  return n ? n : tui_connstr_display(c->connstr);
}

static Workspace *workspace_at(DialogState *ds, size_t i) {
  AppState *app = app_of(ds);
  return app && i < app->num_workspaces ? &app->workspaces[i] : NULL;
}

/* Its name, else its first tab's connection */
static char *ws_title(DialogState *ds, size_t i) {
  Workspace *ws = workspace_at(ds, i);
  if (!ws)
    return str_dup("");
  if (ws->name[0])
    return str_dup(ws->name);
  char *n = ws->num_tabs > 0
                ? conn_name(ds, app_get_connection(app_of(ds), ws->tabs[0].connection_index))
                : NULL;
  return n ? n : str_printf("Workspace %zu", i + 1);
}

/* The transaction state of a connection, for people ("" if none) */
static const char *tx_label(const Connection *c) {
  if (!c || !c->conn)
    return "";
  if (c->conn->transaction_lost)
    return "[TX LOST]";
  if (c->conn->transaction_failed)
    return "[TX FAILED]";
  if (c->conn->in_transaction)
    return "[TX]";
  return "";
}

/* The most urgent transaction state among a workspace's connections */
static const char *ws_tx(DialogState *ds, size_t i) {
  Workspace *ws = workspace_at(ds, i);
  const char *best = "";
  for (size_t t = 0; ws && t < ws->num_tabs; t++) {
    const char *l = tx_label(app_get_connection(app_of(ds), ws->tabs[t].connection_index));
    if (strlen(l) > strlen(best))
      best = l; /* "[TX LOST]" and "[TX FAILED]" over "[TX]" */
  }
  return best;
}

static bool ws_matches(DialogState *ds, size_t i, const char *filter) {
  char *title = ws_title(ds, i);
  bool hit = matches(title, filter);
  free(title);
  Workspace *ws = workspace_at(ds, i);
  for (size_t t = 0; !hit && ws && t < ws->num_tabs; t++) {
    Connection *c = app_get_connection(app_of(ds), ws->tabs[t].connection_index);
    char *n = conn_name(ds, c);
    hit = matches(ws->tabs[t].table_name, filter) || matches(n, filter);
    free(n);
  }
  return hit;
}


static size_t saved_ws_tabs(const SavedWorkspace *s) {
  cJSON *tabs = json_get_array(s->workspace, "tabs");
  return tabs ? (size_t)json_array_size(tabs) : 0;
}


/* ============================================================================
 * The list
 * ============================================================================
 */

void connect_add_row(DialogState *ds, RowKind kind, size_t index,
                     ConnectionItem *item, int depth, const char *text) {
  if (ds->num_rows == ds->rows_cap) {
    ds->rows_cap = ds->rows_cap ? ds->rows_cap * 2 : 32;
    ds->rows = safe_reallocarray(ds->rows, ds->rows_cap, sizeof(ListRow));
  }
  ds->rows[ds->num_rows++] = (ListRow){kind, index, item, depth, text};
}

static bool selectable(const ListRow *r) {
  return r->kind != ROW_HEADER && r->kind != ROW_NOTE;
}

static ListRow *selected(DialogState *ds) {
  return ds->sel < ds->num_rows && selectable(&ds->rows[ds->sel]) ? &ds->rows[ds->sel]
                                                                   : NULL;
}

/* What is selected, so it is found again after the list changes */
static void remember_selection(DialogState *ds) {
  ListRow *r = selected(ds);
  if (!r)
    return;
  ds->sel_kind = r->kind;
  ds->sel_index = r->index;
}

/* A section's title, after an empty line unless it is the first */
static void add_header(DialogState *ds, const char *title) {
  if (ds->num_rows > 0)
    connect_add_row(ds, ROW_NOTE, 0, NULL, 0, "");
  connect_add_row(ds, ROW_HEADER, 0, NULL, 0, title);
}

static void build_rows(DialogState *ds) {
  ds->num_rows = 0;
  const char *filter = connect_filter(ds);
  size_t before = ds->num_rows;
  for (size_t i = 0; i < num_workspaces(ds); i++) {
    if (!filter || ws_matches(ds, i, filter)) {
      if (ds->num_rows == before)
        add_header(ds, "OPEN");
      connect_add_row(ds, ROW_WORKSPACE, i, NULL, 0, NULL);
    }
  }
  before = ds->num_rows;
  for (size_t i = 0; i < ds->saved_ws.count; i++) {
    if (!filter || matches(ds->saved_ws.items[i].name, filter)) {
      if (ds->num_rows == before)
        add_header(ds, "SAVED WORKSPACES");
      connect_add_row(ds, ROW_SAVED_WS, i, NULL, 0, NULL);
    }
  }
  if (ds->num_rows == 0)
    connect_add_row(ds, ROW_NOTE, 0, NULL, 0, "(nothing matches)");
}

/* Rebuild the list and find the selection again: the same item, else the
 * same place, else the nearest selectable row */
static void rebuild(DialogState *ds) {
  build_rows(ds);
  size_t found = SIZE_MAX;
  for (size_t i = 0; i < ds->num_rows && found == SIZE_MAX; i++) {
    ListRow *r = &ds->rows[i];
    if (!selectable(r))
      continue;
    if (r->kind == ds->sel_kind && r->index == ds->sel_index)
      found = i;
  }
  if (found == SIZE_MAX && ds->num_rows > 0) {
    size_t at = ds->sel < ds->num_rows ? ds->sel : ds->num_rows - 1;
    for (size_t i = at; i < ds->num_rows && found == SIZE_MAX; i++)
      if (selectable(&ds->rows[i]))
        found = i;
    for (size_t i = at + 1; i-- > 0 && found == SIZE_MAX;)
      if (selectable(&ds->rows[i]))
        found = i;
  }
  ds->sel = found == SIZE_MAX ? 0 : found;
  remember_selection(ds);
}

/* The first selectable row (after the input changed) */
static void select_first(DialogState *ds) {
  build_rows(ds);
  ds->sel = 0;
  ds->scroll = 0;
  ds->sel_kind = ROW_NOTE; /* Matches nothing: the nearest selectable row */
  for (size_t i = 0; i < ds->num_rows; i++) {
    if (selectable(&ds->rows[i])) {
      ds->sel = i;
      break;
    }
  }
  rebuild(ds);
}

static void move_selection(DialogState *ds, int delta) {
  size_t i = ds->sel;
  int step = delta < 0 ? -1 : 1;
  for (int n = abs(delta); n > 0;) {
    if ((step < 0 && i == 0) || (step > 0 && i + 1 >= ds->num_rows))
      break;
    i = (size_t)((long)i + step);
    if (selectable(&ds->rows[i])) {
      ds->sel = i;
      n--;
    }
  }
  remember_selection(ds);
}

static void select_edge(DialogState *ds, bool last) {
  ds->sel = last ? (ds->num_rows ? ds->num_rows - 1 : 0) : 0;
  if (!selected(ds))
    move_selection(ds, last ? -1 : 1);
  remember_selection(ds);
}

/* ============================================================================
 * Layout
 * ============================================================================
 */

/* The window fills the terminal. Rows: 0 title, 1 input, 2 rule, list and
 * details from 3 to h-5, h-4 rule, h-3 message, h-2 keys, h-1 border. */
static bool layout_dialog(DialogState *ds) {
  int term_h, term_w;
  getmaxyx(stdscr, term_h, term_w);
  if (ds->win && term_h == ds->term_h && term_w == ds->term_w)
    return !ds->too_small;
  ds->term_h = term_h;
  ds->term_w = term_w;
  if (ds->win)
    delwin(ds->win);
  ds->win = newwin(term_h, term_w, 0, 0);
  if (ds->win)
    keypad(ds->win, TRUE);
  ds->too_small = term_h < MIN_DIALOG_HEIGHT || term_w < MIN_DIALOG_WIDTH;
  ds->wide = term_w >= WIDE_DIALOG_WIDTH;
  ds->list_y = 3;
  ds->list_h = term_h - 7;
  ds->list_x = 1;
  if (ds->wide) {
    ds->list_w = term_w * 2 / 5;
    if (ds->list_w < 34)
      ds->list_w = 34;
    if (ds->list_w > 60)
      ds->list_w = 60;
    ds->pane_x = ds->list_x + ds->list_w + 2;
    ds->pane_w = term_w - ds->pane_x - 2;
  } else {
    ds->list_w = term_w - 2;
    ds->pane_x = 2;
    ds->pane_w = term_w - 4;
  }
  ds->pane_y = ds->list_y;
  ds->pane_h = ds->list_h;
  return !ds->too_small;
}

int connect_pane_height(DialogState *ds, int want_h) {
  /* From the rule under the input to the one above the footer */
  int max_h = ds->term_h - 5;
  return want_h < max_h ? want_h : max_h;
}

WINDOW *connect_pane_window(DialogState *ds, int want_h, int *h, int *w) {
  layout_dialog(ds);
  int x = ds->wide ? ds->list_x + ds->list_w : 0;
  *h = connect_pane_height(ds, want_h);
  *w = ds->term_w - x;
  if (*h < 5 || *w < 20)
    return NULL;
  WINDOW *win = newwin(*h, *w, 2, x);
  if (win)
    keypad(win, TRUE);
  return win;
}

/* ============================================================================
 * Drawing: the list
 * ============================================================================
 */

/* left text, and right text against the right edge (clipped first) */
static void draw_two(WINDOW *win, int y, int x, int w, const char *left,
                     const char *right, attr_t right_attr) {
  int rw = right && right[0] ? utf8_width(right, strlen(right)) : 0;
  if (rw > w / 2)
    rw = 0; /* No room: the left text matters more */
  tui_print_clip(win, y, x, rw ? w - rw - 1 : w, left);
  if (rw) {
    wattron(win, right_attr);
    tui_print_clip(win, y, x + w - rw, rw, right);
    wattroff(win, right_attr);
  }
}

static void draw_row(DialogState *ds, const ListRow *r, int y, bool sel) {
  WINDOW *win = ds->win;
  int x = ds->list_x + 1, w = ds->list_w - 2;
  attr_t base = A_NORMAL;
  if (sel)
    base = ds->input_focused ? A_REVERSE : (attr_t)(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  char *left = NULL, *right = NULL;
  attr_t right_attr = sel ? A_NORMAL : A_DIM;

  switch (r->kind) {
  case ROW_HEADER:
    WITH_ATTR(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD, tui_print_clip(win, y, x, w, r->text));
    return;
  case ROW_NOTE:
    WITH_ATTR(win, A_DIM, tui_print_clip(win, y, x + 2, w - 2, r->text));
    return;
  case ROW_WORKSPACE: {
    AppState *app = app_of(ds);
    char *title = ws_title(ds, r->index);
    bool current = app && r->index == app->current_workspace;
    left = str_printf("%s %zu  %s", current ? "\xE2\x97\x8F" : " ", r->index + 1, title);
    free(title);
    Workspace *ws = workspace_at(ds, r->index);
    size_t n = ws ? ws->num_tabs : 0;
    const char *tx = ws_tx(ds, r->index);
    right = str_printf("%zu tab%s%s%s", n, n == 1 ? "" : "s", tx[0] ? "  " : "", tx);
    if (tx[0] && !sel)
      right_attr = COLOR_PAIR(COLOR_ERROR);
    break;
  }
  case ROW_SAVED_WS: {
    SavedWorkspace *s = &ds->saved_ws.items[r->index];
    left = str_printf("  %s", s->name);
    size_t n = saved_ws_tabs(s);
    right = str_printf("%zu tab%s", n, n == 1 ? "" : "s");
    break;
  }
  default:
    break;
  }
  wattron(win, base);
  mvwhline(win, y, ds->list_x, ' ', ds->list_w);
  draw_two(win, y, x, w, left ? left : "", right, right_attr);
  wattroff(win, base);
  free(left);
  free(right);
}

static void draw_list(DialogState *ds) {
  /* Keep the selection on screen, with its section's title when it is the
   * first of the section */
  size_t h = ds->list_h > 0 ? (size_t)ds->list_h : 1;
  if (ds->sel < ds->scroll)
    ds->scroll = ds->sel;
  if (ds->sel > 0 && ds->sel - 1 < ds->scroll && ds->rows[ds->sel - 1].kind == ROW_HEADER)
    ds->scroll = ds->sel - 1;
  if (ds->sel >= ds->scroll + h)
    ds->scroll = ds->sel - h + 1;
  if (ds->scroll + h > ds->num_rows)
    ds->scroll = ds->num_rows > h ? ds->num_rows - h : 0;
  for (size_t i = 0; i < h && ds->scroll + i < ds->num_rows; i++) {
    size_t idx = ds->scroll + i;
    draw_row(ds, &ds->rows[idx], ds->list_y + (int)i, idx == ds->sel);
  }
  /* More above or below: say so at the edge */
  int ax = ds->list_x + ds->list_w - 1;
  if (ds->scroll > 0)
    mvwaddch(ds->win, ds->list_y, ax, ACS_UARROW);
  if (ds->scroll + h < ds->num_rows)
    mvwaddch(ds->win, ds->list_y + (int)h - 1, ax, ACS_DARROW);
}

/* ============================================================================
 * Drawing: the details pane
 * ============================================================================
 */

typedef struct {
  WINDOW *win;
  int y, x, w, max_y;
} Pane;

static void pane_line(Pane *p, attr_t attr, const char *text) {
  if (p->y >= p->max_y)
    return;
  wattron(p->win, attr);
  tui_print_clip(p->win, p->y++, p->x, p->w, text ? text : "");
  wattroff(p->win, attr);
}

/* text over as many lines as it needs, broken after a space when one is
 * near the end of the line (paths and URLs break anywhere); lines after the
 * first start indent columns in */
static void pane_wrap_at(Pane *p, attr_t attr, const char *text, int indent) {
  const char *s = text ? text : "";
  bool first = true;
  while (*s && p->y < p->max_y) {
    int x = first ? 0 : indent, w = p->w - x;
    if (w < 8)
      break;
    int cols = 0;
    size_t len = strlen(s);
    size_t take = utf8_fit(s, len, w, &cols);
    if (take == 0)
      break;
    if (take < len) {
      for (size_t b = take; b > take * 2 / 3; b--) {
        if (s[b - 1] == ' ') {
          take = b;
          break;
        }
      }
    }
    char *part = str_ndup(s, take);
    wattron(p->win, attr);
    tui_print_clip(p->win, p->y++, p->x + x, w, part);
    wattroff(p->win, attr);
    free(part);
    s += take;
    while (*s == ' ')
      s++;
    first = false;
  }
}

static void pane_wrap(Pane *p, attr_t attr, const char *text) {
  pane_wrap_at(p, attr, text, 0);
}

static void pane_title(Pane *p, const char *title, const char *sub) {
  pane_line(p, A_BOLD, title);
  if (sub && sub[0])
    pane_wrap(p, A_DIM, sub);
  p->y++;
}


/* "Query: <first line>" */
static char *query_line(const char *q) {
  q = q ? q : "";
  while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')
    q++;
  size_t n = strcspn(q, "\r\n");
  return n ? str_printf("Query: %.*s", (int)n, q) : str_dup("Query (empty)");
}

/* One line about a tab: what it shows and how */
static char *tab_summary(const Tab *tab) {
  if (tab->type == TAB_TYPE_QUERY)
    return query_line(tab->query_text);
  if (tab->type != TAB_TYPE_TABLE)
    return str_dup("(no table open)");
  StringBuilder *sb = sb_new(64);
  sb_append(sb, tab->table_name ? tab->table_name : "");
  if (tab->num_sort_entries > 0 && tab->schema &&
      tab->sort_entries[0].column < tab->schema->num_columns) {
    sb_printf(sb, "  sorted by %s %s%s", tab->schema->columns[tab->sort_entries[0].column].name,
              tab->sort_entries[0].direction == SORT_DESC ? "\xE2\x96\xBC" : "\xE2\x96\xB2",
              tab->num_sort_entries > 1 ? " +" : "");
  }
  if (tab->filters.num_filters > 0)
    sb_printf(sb, "  %zu filter%s", tab->filters.num_filters,
              tab->filters.num_filters == 1 ? "" : "s");
  return sb_finish(sb);
}

static void pane_workspace(DialogState *ds, Pane *p, size_t i) {
  Workspace *ws = workspace_at(ds, i);
  AppState *app = app_of(ds);
  if (!ws || !app)
    return;
  char *title = ws_title(ds, i);
  char *sub = str_printf("Workspace %zu%s", i + 1,
                         i == app->current_workspace ? " (current)" : "");
  pane_title(p, title, sub);
  free(title);
  free(sub);
  /* Each connection once, with its transaction state */
  bool *seen = safe_calloc(app->num_connections + 1, sizeof(bool));
  for (size_t t = 0; t < ws->num_tabs; t++) {
    size_t ci = ws->tabs[t].connection_index;
    Connection *c = app_get_connection(app, ci);
    if (!c || ci >= app->num_connections || seen[ci])
      continue;
    seen[ci] = true;
    char *name = conn_name(ds, c);
    char *where = tui_connstr_display(c->connstr);
    pane_line(p, A_NORMAL, name);
    if (where && name && !str_eq(where, name))
      pane_wrap(p, A_DIM, where);
    const char *tx = tx_label(c);
    if (tx[0]) {
      char *l = str_printf("%s transaction open", tx);
      pane_line(p, COLOR_PAIR(COLOR_ERROR), l);
      free(l);
    }
    free(name);
    free(where);
  }
  free(seen);
  p->y++;
  pane_line(p, A_BOLD, "Tabs");
  for (size_t t = 0; t < ws->num_tabs; t++) {
    char *s = tab_summary(&ws->tabs[t]);
    char *l = str_printf("%s %s", t == ws->current_tab ? "\xE2\x96\xB8" : " ", s);
    pane_line(p, A_NORMAL, l);
    free(l);
    free(s);
  }
}

static void pane_saved_ws(DialogState *ds, Pane *p, size_t i) {
  SavedWorkspace *s = &ds->saved_ws.items[i];
  char *w = when(s->saved);
  char *sub = w[0] ? str_printf("Saved workspace (%s)", w) : str_dup("Saved workspace");
  pane_title(p, s->name, sub);
  free(w);
  free(sub);
  pane_line(p, A_BOLD, "Tabs");
  cJSON *tabs = json_get_array(s->workspace, "tabs");
  cJSON *t;
  cJSON_ArrayForEach(t, tabs) {
    const char *type = json_get_string(t, "type", "");
    const char *id = json_get_string(t, "connection_id", "");
    ConnectionItem *item = connmgr_find_by_id(ds->mgr, id);
    const char *cn = item && connmgr_is_connection(item) ? item->connection.name
                                                         : "(connection deleted)";
    char *what;
    if (str_eq(type, "TABLE")) {
      cJSON *filters = json_get_array(t, "filters");
      int nf = filters ? json_array_size(filters) : 0;
      what = nf ? str_printf("%s  %d filter%s", json_get_string(t, "table_name", ""), nf,
                             nf == 1 ? "" : "s")
                : str_dup(json_get_string(t, "table_name", ""));
    } else if (str_eq(type, "QUERY")) {
      what = query_line(json_get_string(t, "query_text", ""));
    } else {
      what = str_dup("(no table open)");
    }
    char *l = str_printf("  %s  \xC2\xB7 %s", what, cn ? cn : "");
    pane_line(p, A_NORMAL, l);
    free(l);
    free(what);
  }
  p->y++;
  pane_wrap(p, A_DIM, "Enter opens it as a new workspace (connections already open are "
                      "used as they are)");
}



static void pane_examples(Pane *p) {
  static const char *const lines[] = {
      "Every open workspace and the saved layouts.",
      "",
      "Connections open in a connection tab: Ctrl+T in any",
      "workspace, or n here for a new workspace with one.",
      NULL};
  for (int i = 0; lines[i]; i++)
    pane_line(p, i == 0 ? A_NORMAL : A_DIM, lines[i]);
}

static void draw_pane(DialogState *ds) {
  Pane p = {ds->win, ds->pane_y, ds->pane_x, ds->pane_w, ds->pane_y + ds->pane_h};
  ListRow *r = selected(ds);
  if (r && r->kind == ROW_WORKSPACE)
    pane_workspace(ds, &p, r->index);
  else if (r && r->kind == ROW_SAVED_WS)
    pane_saved_ws(ds, &p, r->index);
  else
    pane_examples(&p);
}

/* ============================================================================
 * Drawing: footer and frame
 * ============================================================================
 */



/* The actions that apply to the selected row, Enter's first */
static int row_actions(DialogState *ds, ActionId *out) {
  int n = 0;
  ListRow *r = selected(ds);
  if (r && r->kind == ROW_WORKSPACE) {
    out[n++] = ACT_ENTER;
    out[n++] = ACT_RENAME;
    out[n++] = ACT_SAVE;
    out[n++] = ACT_DELETE;
  } else if (r && r->kind == ROW_SAVED_WS) {
    out[n++] = ACT_ENTER;
    out[n++] = ACT_RENAME;
    out[n++] = ACT_DELETE;
  }
  /* Narrow: the details right after Enter, where they are seen */
  if (!ds->wide) {
    int at = n > 0 && out[0] == ACT_ENTER ? 1 : 0;
    memmove(&out[at + 1], &out[at], (size_t)(n - at) * sizeof(ActionId));
    out[at] = ACT_DETAILS;
    n++;
  }
  out[n++] = ACT_NEW;
  out[n++] = ACT_SEARCH;
  out[n++] = ACT_HELP;
  return n;
}

static bool action_applies(DialogState *ds, ActionId act) {
  ActionId acts[MAX_HINTS];
  int n = row_actions(ds, acts);
  for (int i = 0; i < n; i++)
    if (acts[i] == act)
      return true;
  return act == ACT_QUIT;
}

static const char *action_key(DialogState *ds, ActionId act) {
  const Config *c = ds->config;
  switch (act) {
  case ACT_ENTER: return "Enter";
  case ACT_SAVE: return "s";
  case ACT_RENAME: return connect_key_hint(c, HOTKEY_CONN_RENAME, "r");
  case ACT_DELETE: return connect_key_hint(c, HOTKEY_CONN_DELETE, "x");
  case ACT_NEW: return connect_key_hint(c, HOTKEY_CONN_NEW, "n");
  case ACT_SEARCH: return connect_key_hint(c, HOTKEY_CONN_FILTER, "/");
  case ACT_DETAILS: return "i";
  case ACT_HELP: return "?";
  case ACT_QUIT: return "q";
  default: return "";
  }
}

static const char *action_label(DialogState *ds, ActionId act) {
  ListRow *r = selected(ds);
  RowKind k = r ? r->kind : ROW_NOTE;
  switch (act) {
  case ACT_ENTER: return k == ROW_WORKSPACE ? "switch" : "open";
  case ACT_SAVE: return "save layout";
  case ACT_RENAME: return "rename";
  case ACT_DELETE: return k == ROW_WORKSPACE ? "close" : "delete";
  case ACT_NEW: return "new workspace";
  case ACT_SEARCH: return "search";
  case ACT_DETAILS: return ds->show_details ? "list" : "details";
  case ACT_HELP: return "keys";
  case ACT_QUIT: return "quit";
  default: return "";
  }
}


static void draw_message(DialogState *ds, int y) {
  WINDOW *win = ds->win;
  const char *msg = ds->error_msg ? ds->error_msg : ds->success_msg;
  attr_t attr = msg ? COLOR_PAIR(ds->error_msg ? COLOR_ERROR : COLOR_NUMBER) : A_DIM;
  if (!msg && ds->input_focused)
    msg = "Type to search. Up/Down select, Enter opens, Tab or Esc: the list";
  if (!msg)
    return;
  char *flat = connect_flatten_message(msg);
  wattron(win, attr);
  tui_print_clip(win, y, 2, ds->term_w - 4, flat);
  wattroff(win, attr);
  free(flat);
}

/* The keys for the selection ("Enter switch  r rename ..."), as many as
 * fit, and always the one that lists them all; clicks find them by
 * hint_x/hint_w */
static void draw_hints(DialogState *ds, int y) {
  ActionId acts[MAX_HINTS];
  int n = row_actions(ds, acts);
  int x = 2, end = ds->term_w - 2;
  const char *help_key = action_key(ds, ACT_HELP);
  const char *help_label = action_label(ds, ACT_HELP);
  int help_w = utf8_width(help_key, strlen(help_key)) + 1 +
               utf8_width(help_label, strlen(help_label));
  ds->num_hints = 0;
  bool full = false;
  for (int i = 0; i < n; i++) {
    bool help = acts[i] == ACT_HELP;
    if (full && !help)
      continue;
    const char *key = action_key(ds, acts[i]);
    const char *label = action_label(ds, acts[i]);
    int kw = utf8_width(key, strlen(key)), lw = utf8_width(label, strlen(label));
    /* Room is kept for the last hint, "? keys" */
    if (!help && x + kw + 1 + lw + 2 + help_w > end) {
      full = true;
      continue;
    }
    if (x + kw + 1 + lw > end)
      break;
    WITH_ATTR(ds->win, A_BOLD, tui_print_clip(ds->win, y, x, kw, key));
    tui_print_clip(ds->win, y, x + kw + 1, lw, label);
    ds->hint_act[ds->num_hints] = acts[i];
    ds->hint_x[ds->num_hints] = x;
    ds->hint_w[ds->num_hints] = kw + 1 + lw;
    ds->num_hints++;
    x += kw + 1 + lw + 2;
  }
}

static void draw_frame(DialogState *ds) {
  WINDOW *win = ds->win;
  int h = ds->term_h, w = ds->term_w;
  DRAW_BOX(win, COLOR_BORDER);
  int split = ds->wide ? ds->list_x + ds->list_w : -1;
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  int rules[2] = {2, h - 4};
  for (int i = 0; i < 2; i++) {
    mvwaddch(win, rules[i], 0, ACS_LTEE);
    mvwhline(win, rules[i], 1, ACS_HLINE, w - 2);
    mvwaddch(win, rules[i], w - 1, ACS_RTEE);
  }
  if (split > 0) {
    for (int row = 3; row < h - 4; row++)
      mvwaddch(win, row, split, ACS_VLINE);
    mvwaddch(win, 2, split, ACS_TTEE);
    mvwaddch(win, h - 4, split, ACS_BTEE);
  }
  wattroff(win, COLOR_PAIR(COLOR_BORDER));
  WITH_ATTR(win, A_BOLD, tui_print_clip(win, 0, 2, w - 4, " Workspaces "));
}

/* The input line; where its cursor goes in *cy, *cx (-1: hidden) */
static void draw_input(DialogState *ds, int *cy, int *cx) {
  WINDOW *win = ds->win;
  int w = ds->term_w - 6;
  WITH_ATTR(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD, mvwaddch(win, 1, 2, '>'));
  *cy = *cx = -1;
  if (ds->input.len == 0 && !ds->input_focused) {
    char *ph = str_printf("%s search workspaces",
                          connect_key_hint(ds->config, HOTKEY_CONN_FILTER, "/"));
    WITH_ATTR(win, A_DIM, tui_print_clip(win, 1, 4, w, ph));
    free(ph);
    return;
  }
  int c = text_input_draw(win, &ds->input, 1, 4, w, A_NORMAL, ds->input_focused);
  if (ds->input_focused && c >= 0) {
    *cy = 1;
    *cx = c;
  }
}

static void draw_dialog(DialogState *ds) {
  WINDOW *win = ds->win;
  if (!win)
    return;
  werase(win);
  if (ds->too_small) {
    char *m = str_printf("Needs %dx%d (Esc closes)",
                         MIN_DIALOG_WIDTH, MIN_DIALOG_HEIGHT);
    tui_print_clip(win, 0, 0, ds->term_w, m);
    free(m);
    curs_set(0);
    touchwin(win);
    wrefresh(win);
    return;
  }
  draw_frame(ds);
  int cy, cx;
  draw_input(ds, &cy, &cx);
  if (!ds->wide && ds->show_details)
    draw_pane(ds);
  else
    draw_list(ds);
  if (ds->wide)
    draw_pane(ds);
  draw_message(ds, ds->term_h - 3);
  draw_hints(ds, ds->term_h - 2);
  if (cy >= 0) {
    wmove(win, cy, cx);
    curs_set(1);
  } else {
    curs_set(0);
  }
  /* Other windows may have drawn over us (a restore, a progress dialog) */
  touchwin(win);
  wrefresh(win);
}

void connect_redraw(DialogState *ds) {
  layout_dialog(ds);
  rebuild(ds);
  draw_dialog(ds);
}

/* ============================================================================
 * Small dialogs
 * ============================================================================
 */

char *connect_prompt_text(WINDOW *parent, const char *title, const char *label,
                          const char *initial_value) {
  (void)parent; /* Centered on the screen, over the window */
  PromptOptions opts = {title, label, initial_value, NULL, false, false};
  return tui_prompt(&opts);
}

bool connect_confirm(WINDOW *parent, const char *title, const char *message) {
  (void)parent;
  int term_h, term_w;
  getmaxyx(stdscr, term_h, term_w);
  int msg_w = utf8_width(message, strlen(message));
  int w = msg_w + 6;
  if (w < 36)
    w = 36;
  if (w > term_w - 2)
    w = term_w - 2;
  int h = 7;
  WINDOW *dlg = dialog_create(h, w, term_h, term_w);
  if (!dlg)
    return false;
  int focus = 1; /* No */
  int bx_yes = 0;
  bool result = false, running = true;
  while (running) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);
    char *t = str_printf(" %s ", title);
    int tw = utf8_width(t, strlen(t));
    WITH_ATTR(dlg, A_BOLD, tui_print_clip(dlg, 0, (w - tw) / 2 > 1 ? (w - tw) / 2 : 1, w - 2, t));
    free(t);
    int mx = (w - msg_w) / 2;
    if (mx < 2)
      mx = 2;
    tui_print_clip(dlg, 2, mx, w - mx - 2, message);
    int bx = w / 2 - 8;
    bx_yes = bx;
    if (focus == 0)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 4, bx, "[ Yes ]");
    if (focus == 0)
      wattroff(dlg, A_REVERSE);
    if (focus == 1)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 4, bx + 9, "[ No ]");
    if (focus == 1)
      wattroff(dlg, A_REVERSE);
    curs_set(0);
    wrefresh(dlg);
    int ch = wgetch(dlg);
    if (ch == KEY_RESIZE)
      continue;
    UiEvent event;
    render_translate_key(ch, &event);
    if (event.type == UI_EVENT_MOUSE && event.mouse.button == UI_MOUSE_LEFT) {
      int by, bx;
      getbegyx(dlg, by, bx);
      int ry = event.mouse.y - by, rx = event.mouse.x - bx;
      if (ry == 4 && rx >= bx_yes && rx < bx_yes + 7) {
        result = true;
        running = false;
      } else if (ry == 4 && rx >= bx_yes + 9 && rx < bx_yes + 15) {
        running = false;
      }
      continue;
    }
    int key = render_event_get_char(&event);
    if (render_event_is_special(&event, UI_KEY_ESCAPE) || key == 'n' || key == 'N') {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_BTAB) ||
               render_event_is_special(&event, UI_KEY_LEFT) ||
               render_event_is_special(&event, UI_KEY_RIGHT) || key == 'h' ||
               key == 'l') {
      focus = 1 - focus;
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      result = focus == 0;
      running = false;
    } else if (key == 'y' || key == 'Y') {
      result = true;
      running = false;
    }
  }
  delwin(dlg);
  return result;
}

void connect_button_layout(const char *const *labels, int count, int w,
                           int gap, bool roomy, int *x, int *width) {
  int total = 0;
  for (int i = 0; i < count; i++) {
    width[i] = utf8_width(labels[i], strlen(labels[i])) + (roomy ? 4 : 2);
    total += width[i] + (i ? gap : 0);
  }
  int pos = (w - total) / 2;
  if (pos < 1)
    pos = 1;
  for (int i = 0; i < count; i++) {
    x[i] = pos;
    pos += width[i] + gap;
  }
}

int connect_button_at(const int *x, const int *width, int count, int cx) {
  for (int i = 0; i < count; i++)
    if (cx >= x[i] && cx < x[i] + width[i])
      return i;
  return -1;
}


/* ============================================================================
 * Actions
 * ============================================================================
 */


static void save_layout(DialogState *ds, size_t i) {
  Workspace *ws = workspace_at(ds, i);
  if (!ws || !ds->state)
    return;
  char *title = ws_title(ds, i);
  char *name = connect_prompt_text(ds->win, "Save Workspace", "Name:", title);
  free(title);
  connect_redraw(ds);
  if (!name)
    return;
  if (saved_workspaces_find(&ds->saved_ws, name) != SIZE_MAX) {
    char *q = str_printf("Replace the saved workspace \"%s\"?", name);
    bool yes = connect_confirm(ds->win, "Replace", q);
    free(q);
    connect_redraw(ds);
    if (!yes) {
      free(name);
      return;
    }
  }
  size_t skipped = 0;
  cJSON *json = session_workspace_to_json(ds->state, i, &skipped);
  if (!json) {
    set_error(ds, str_dup("Its tabs use connections that are not saved: save the "
                          "connection first (s on it under Recent)"));
    free(name);
    return;
  }
  saved_workspaces_put(&ds->saved_ws, name, json);
  if (save_layouts(ds)) {
    snprintf(ws->name, sizeof(ws->name), "%s", name);
    set_success(ds, skipped ? str_printf("Saved \"%s\" (%zu tab%s left out: not on a saved "
                                         "connection)",
                                         name, skipped, skipped == 1 ? "" : "s")
                            : str_printf("Saved \"%s\"", name));
  }
  free(name);
}

static void rename_workspace(DialogState *ds, size_t i) {
  Workspace *ws = workspace_at(ds, i);
  if (!ws)
    return;
  char *title = ws_title(ds, i);
  char *name = connect_prompt_text(ds->win, "Rename Workspace", "Name:", title);
  free(title);
  connect_redraw(ds);
  if (name)
    snprintf(ws->name, sizeof(ws->name), "%s", name);
  free(name);
}

static void rename_saved_ws(DialogState *ds, size_t i) {
  SavedWorkspace *s = &ds->saved_ws.items[i];
  char *name = connect_prompt_text(ds->win, "Rename Saved Workspace", "Name:", s->name);
  connect_redraw(ds);
  if (!name)
    return;
  if (!str_eq(name, s->name) && saved_workspaces_find(&ds->saved_ws, name) != SIZE_MAX) {
    set_error(ds, str_printf("A saved workspace is already called \"%s\"", name));
    free(name);
    return;
  }
  free(s->name);
  s->name = name;
  save_layouts(ds);
}

static void close_workspace(DialogState *ds, size_t i) {
  Workspace *ws = workspace_at(ds, i);
  if (!ws || !ds->state)
    return;
  char *title = ws_title(ds, i);
  char *q = tui_workspace_close_question(ds->state, i);
  if (!q)
    q = str_printf("Close workspace %zu \"%s\" (%zu tab%s)?", i + 1, title, ws->num_tabs,
                   ws->num_tabs == 1 ? "" : "s");
  bool yes = connect_confirm(ds->win, "Close Workspace", q);
  free(q);
  if (yes) {
    tui_workspace_close_at(ds->state, i);
    set_success(ds, str_printf("Closed \"%s\"", title));
  }
  free(title);
  connect_redraw(ds);
}

static void delete_saved_ws(DialogState *ds, size_t i) {
  char *q = str_printf("Delete the saved workspace \"%s\"?", ds->saved_ws.items[i].name);
  bool yes = connect_confirm(ds->win, "Confirm Delete", q);
  free(q);
  connect_redraw(ds);
  if (!yes)
    return;
  saved_workspaces_remove(&ds->saved_ws, i);
  if (save_layouts(ds))
    set_success(ds, str_dup("Deleted"));
}

static bool open_saved_ws(DialogState *ds, size_t i) {
  if (!ds->state)
    return false;
  SavedWorkspace *s = &ds->saved_ws.items[i];
  char *err = NULL;
  bool ok = session_open_workspace_json(ds->state, s->workspace, s->name, &err);
  if (!ok)
    set_error(ds, err ? str_printf("Could not open \"%s\": %s", s->name, err)
                      : str_dup("Could not open the workspace"));
  free(err);
  if (!ok)
    connect_redraw(ds);
  return ok;
}

/* Every key, in a box (any key closes it) */
static void show_help(DialogState *ds) {
  const Config *c = ds->config;
  /* connect_key_hint returns a static buffer: one call per line */
  char *lines[] = {
      str_printf("%-11s switch to a workspace, or open a saved one", "Enter"),
      str_printf("%-11s search", connect_key_hint(c, HOTKEY_CONN_FILTER, "/")),
      str_printf("%-11s from the input back to the list", "Tab, Esc"),
      str_printf("%-11s save a workspace's layout", "s"),
      str_printf("%-11s rename", connect_key_hint(c, HOTKEY_CONN_RENAME, "r")),
      str_printf("%-11s close a workspace, delete a saved one", connect_key_hint(c, HOTKEY_CONN_DELETE, "x")),
      str_printf("%-11s new workspace (with a connection tab)", connect_key_hint(c, HOTKEY_CONN_NEW, "n")),
      str_printf("%-11s details in place of the list (narrow terminals)", "i"),
      str_printf("%-11s close this window", "Esc"),
      str_printf("%-11s quit lace", "q"),
  };
  int n = (int)(sizeof(lines) / sizeof(lines[0]));
  int term_h, term_w;
  getmaxyx(stdscr, term_h, term_w);
  int w = 0;
  for (int i = 0; i < n; i++) {
    int lw = utf8_width(lines[i], strlen(lines[i]));
    if (lw > w)
      w = lw;
  }
  w += 4;
  if (w > term_w)
    w = term_w;
  int h = n + 2 < term_h ? n + 2 : term_h;
  WINDOW *dlg = dialog_create(h, w, term_h, term_w);
  if (dlg) {
    werase(dlg);
    DRAW_BOX(dlg, COLOR_BORDER);
    WITH_ATTR(dlg, A_BOLD, tui_print_clip(dlg, 0, 2, w - 4, " Keys "));
    for (int i = 0; i < n && i + 1 < h - 1; i++)
      tui_print_clip(dlg, i + 1, 2, w - 4, lines[i]);
    curs_set(0);
    wrefresh(dlg);
    wgetch(dlg);
    delwin(dlg);
  }
  for (int i = 0; i < n; i++)
    free(lines[i]);
  connect_redraw(ds);
}

/* Run act on the selected row; *running goes false when the window should
 * close */
static void run_action(DialogState *ds, ActionId act, bool *running,
                       ConnectResult *result) {
  if (act == ACT_NONE)
    return;
  if (!action_applies(ds, act)) {
    set_error(ds, str_printf("%s does not apply here: the keys below do",
                             action_key(ds, act)));
    return;
  }
  ListRow *sel = selected(ds);
  ListRow r = sel ? *sel : (ListRow){ROW_NOTE, 0, NULL, 0, NULL};

  switch (act) {
  case ACT_ENTER:
    if (r.kind == ROW_WORKSPACE) {
      tui_workspace_switch(ds->state, r.index);
      *running = false;
    } else if (r.kind == ROW_SAVED_WS) {
      if (open_saved_ws(ds, r.index))
        *running = false;
    }
    break;
  case ACT_SAVE:
    save_layout(ds, r.index);
    break;
  case ACT_RENAME:
    if (r.kind == ROW_WORKSPACE)
      rename_workspace(ds, r.index);
    else
      rename_saved_ws(ds, r.index);
    break;
  case ACT_DELETE:
    if (r.kind == ROW_WORKSPACE) {
      close_workspace(ds, r.index);
      /* The last one closed: lace quits, or a new workspace shows */
      if (!ds->state->running || !ds->state->app->running) {
        result->mode = CONNECT_MODE_QUIT;
        *running = false;
      }
    } else {
      delete_saved_ws(ds, r.index);
    }
    break;
  case ACT_NEW:
    tui_new_workspace_form(ds->state);
    *running = false;
    break;
  case ACT_SEARCH:
    ds->input_focused = true;
    ds->show_details = false;
    break;
  case ACT_DETAILS:
    ds->show_details = !ds->show_details;
    break;
  case ACT_HELP:
    show_help(ds);
    break;
  case ACT_QUIT:
    result->mode = CONNECT_MODE_QUIT;
    *running = false;
    break;
  default:
    break;
  }
}

/* What a key asks for (letters only from the list; in the input they are
 * text) */
static ActionId key_action(DialogState *ds, const UiEvent *ev) {
  const Config *c = ds->config;
  if (render_event_is_special(ev, UI_KEY_ENTER))
    return ACT_ENTER;
  if (ds->input_focused)
    return ACT_NONE;
  int ch = render_event_get_char(ev);
  if (ch == 's')
    return ACT_SAVE;
  if (ch == '?')
    return ACT_HELP;
  if (ch == 'q')
    return ACT_QUIT;
  if (ch == 'i')
    return ds->wide ? ACT_NONE : ACT_DETAILS;
  if (!c)
    return ACT_NONE;
  static const struct {
    HotkeyAction hk;
    ActionId act;
  } map[] = {{HOTKEY_CONN_RENAME, ACT_RENAME}, {HOTKEY_CONN_DELETE, ACT_DELETE},
             {HOTKEY_CONN_NEW, ACT_NEW},       {HOTKEY_CONN_FILTER, ACT_SEARCH}};
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (hotkey_matches(c, ev, map[i].hk))
      return map[i].act;
  return ACT_NONE;
}

/* ============================================================================
 * Mouse
 * ============================================================================
 */

/* Clicks: the input focuses it, a row selects (a double click opens), a
 * footer hint runs; the wheel moves the selection. */
static void handle_mouse(DialogState *ds, const UiEvent *event, bool *running,
                         ConnectResult *result) {
  if (!ds->win)
    return;
  int y = event->mouse.y, x = event->mouse.x;
  if (event->mouse.button == UI_MOUSE_SCROLL_UP ||
      event->mouse.button == UI_MOUSE_SCROLL_DOWN) {
    move_selection(ds, event->mouse.button == UI_MOUSE_SCROLL_UP ? -3 : 3);
    return;
  }
  if (event->mouse.button != UI_MOUSE_LEFT)
    return;
  if (y == 1) {
    ds->input_focused = true;
    return;
  }
  if (y == ds->term_h - 2) {
    for (int i = 0; i < ds->num_hints; i++) {
      if (x >= ds->hint_x[i] && x < ds->hint_x[i] + ds->hint_w[i]) {
        run_action(ds, ds->hint_act[i], running, result);
        return;
      }
    }
    return;
  }
  bool in_list = (ds->wide || !ds->show_details) && y >= ds->list_y &&
                 y < ds->list_y + ds->list_h && x >= ds->list_x &&
                 x < ds->list_x + ds->list_w;
  if (!in_list)
    return;
  size_t idx = ds->scroll + (size_t)(y - ds->list_y);
  if (idx >= ds->num_rows || !selectable(&ds->rows[idx]))
    return;
  ds->sel = idx;
  ds->input_focused = false;
  remember_selection(ds);
  if (event->mouse.action == UI_MOUSE_DOUBLE_CLICK)
    run_action(ds, ACT_ENTER, running, result);
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================
 */

ConnectResult connect_view_show(TuiState *state) {
  ConnectResult result = {CONNECT_MODE_CANCELLED};

  DialogState ds = {0};
  ds.state = state;
  ds.mgr = connmgr_load(NULL); /* Names of saved connections, read only */
  if (!ds.mgr)
    ds.mgr = connmgr_new();
  ds.mgr->read_only = true;
  ds.config = state && state->app ? state->app->config : NULL;
  saved_workspaces_load(&ds.saved_ws);
  text_input_init(&ds.input, NULL, false);
  if (ds.saved_ws.load_error)
    ds.error_msg = str_printf("%s (saved workspaces will not be changed)",
                              ds.saved_ws.load_error);

  /* Start on the current workspace */
  build_rows(&ds);
  ds.sel_kind = num_workspaces(&ds) > 0 ? ROW_WORKSPACE : ROW_NOTE;
  ds.sel_index = state && state->app ? state->app->current_workspace : 0;

  bool running = true;
  while (running) {
    layout_dialog(&ds);
    rebuild(&ds);
    draw_dialog(&ds);
    if (!ds.win)
      break;

    int ch = wgetch(ds.win);
    if (ch == KEY_RESIZE)
      continue; /* layout_dialog() sees the new size */
    UiEvent event;
    render_translate_key(ch, &event);
    free(ds.error_msg);
    ds.error_msg = NULL;
    free(ds.success_msg);
    ds.success_msg = NULL;

    if (ds.too_small) {
      if (render_event_is_special(&event, UI_KEY_ESCAPE))
        running = false;
      continue;
    }
    if (event.type == UI_EVENT_MOUSE) {
      handle_mouse(&ds, &event, &running, &result);
      continue;
    }

    bool esc = render_event_is_special(&event, UI_KEY_ESCAPE);
    bool tab = render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_BTAB);
    bool up = render_event_is_special(&event, UI_KEY_UP);
    bool down = render_event_is_special(&event, UI_KEY_DOWN);
    bool pgup = render_event_is_special(&event, UI_KEY_PAGEUP);
    bool pgdn = render_event_is_special(&event, UI_KEY_PAGEDOWN);
    int page = ds.list_h > 2 ? ds.list_h - 1 : 1;

    if (ds.input_focused) {
      ActionId act = key_action(&ds, &event);
      if (act != ACT_NONE) {
        run_action(&ds, act, &running, &result);
      } else if (esc) {
        /* Clear the text first, then back to the list */
        if (ds.input.len > 0) {
          text_input_set(&ds.input, "");
          select_first(&ds);
        } else {
          ds.input_focused = false;
        }
      } else if (tab) {
        ds.input_focused = false;
      } else if (up || down || pgup || pgdn) {
        move_selection(&ds, up ? -1 : down ? 1 : pgup ? -page : page);
      } else {
        char *old = str_dup(ds.input.buf ? ds.input.buf : "");
        text_input_handle_key(&ds.input, &event);
        if (!str_eq(old, ds.input.buf ? ds.input.buf : ""))
          select_first(&ds);
        free(old);
      }
      continue;
    }

    if (esc) {
      /* A search is cleared before the window closes */
      if (ds.input.len > 0) {
        text_input_set(&ds.input, "");
        select_first(&ds);
      } else if (ds.show_details) {
        ds.show_details = false;
      } else {
        running = false;
      }
    } else if (tab) {
      ds.input_focused = true;
      ds.show_details = false;
    } else if (up || (ds.config && hotkey_matches(ds.config, &event, HOTKEY_MOVE_UP))) {
      move_selection(&ds, -1);
    } else if (down || (ds.config && hotkey_matches(ds.config, &event, HOTKEY_MOVE_DOWN))) {
      move_selection(&ds, 1);
    } else if (pgup || pgdn) {
      move_selection(&ds, pgup ? -page : page);
    } else if (render_event_is_special(&event, UI_KEY_HOME) ||
               render_event_is_special(&event, UI_KEY_END)) {
      select_edge(&ds, render_event_is_special(&event, UI_KEY_END));
    } else {
      run_action(&ds, key_action(&ds, &event), &running, &result);
    }
  }

  curs_set(0);
  if (ds.win)
    delwin(ds.win);
  connmgr_free(ds.mgr);
  saved_workspaces_free(&ds.saved_ws);
  free(ds.rows);
  free(ds.error_msg);
  free(ds.success_msg);
  text_input_free(&ds.input);

  /* The window covered everything: the screen is drawn again in full */
  if (state) {
    tui_recreate_windows(state);
    tui_refresh(state);
  }
  return result;
}
