/*
 * Lace
 * The screen chrome, drawn as the notcurses TUI draws it: the header (the
 * path to the current tab, the sorts and filters and the cursor position
 * on the right), the tabs strip (one flat row of the workspace's tabs, the
 * current one with a close mark) and the status line (the focused thing's
 * key hints, the transaction badge, the message). The text comes from the
 * shared core/chrome.c; clicks on a hint run its action.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "core/chrome.h"
#include "config/workspace_json.h"
#include "ddl_edit.h"
#include "tui_internal.h"
#include "views/form_tab.h"
#include "views/palette.h"
#include "views/theme.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Where the last draw put what a click can reach: [x0, x1) and a value */
typedef struct {
  int x0, x1, value;
} Span;

typedef struct {
  Span *s;
  size_t n, cap;
} Spans;

static Spans g_header, g_strip, g_strip_close, g_status;

static void span_add(Spans *sp, int x0, int x1, int value) {
  if (sp->n == sp->cap) {
    sp->cap = sp->cap ? sp->cap * 2 : 16;
    sp->s = safe_realloc(sp->s, sp->cap * sizeof *sp->s);
  }
  sp->s[sp->n++] = (Span){x0, x1, value};
}

static int span_find(const Spans *sp, int x) {
  for (size_t i = 0; i < sp->n; i++)
    if (x >= sp->s[i].x0 && x < sp->s[i].x1)
      return sp->s[i].value;
  return -1;
}

static int text_width(const char *s) { return s ? utf8_width(s, strlen(s)) : 0; }

/* Fill [x, x + w) of row y with text, clipped and padded by width */
static void put(WINDOW *w, int y, int x, int width, const char *text) {
  if (width > 0)
    tui_print_fit(w, y, x, width, text ? text : "");
}

static void put_right(WINDOW *w, int y, int x, int width, const char *text) {
  int tw = text_width(text);
  if (tw >= width) {
    put(w, y, x, width, text);
    return;
  }
  put(w, y, x, width - tw, "");
  put(w, y, x + width - tw, tw, text);
}

/* ==========================================================================
 * Names
 * ========================================================================== */

/* What a connection is called: its saved name, else the file or "db on host" */
static char *connection_name(TuiState *state, const Connection *c) {
  if (!c)
    return str_dup("?");
  ConnectionManager *mgr = c->saved_conn_id ? tui_connmgr(state) : NULL;
  ConnectionItem *it = mgr ? connmgr_find_by_id(mgr, c->saved_conn_id) : NULL;
  if (it && connmgr_is_connection(it) && it->connection.name && *it->connection.name)
    return str_dup(it->connection.name);
  return chrome_connstr_short_name(c->connstr);
}

/* A tab's label on the strip: a form's, a connection tab's connection, else
 * its table or query name */
static char *tab_label(TuiState *state, const Tab *tab) {
  if (tab->type == TAB_TYPE_FORM)
    return form_tab_label(tab);
  if (tab->type == TAB_TYPE_CONNECTION)
    return connection_name(state, app_get_connection(state->app, tab->connection_index));
  if (tab->type == TAB_TYPE_QUERY) {
    char buf[256];
    return str_dup(chrome_query_title(tab->query_text, buf, sizeof buf));
  }
  return str_dup(tab->table_name ? tab->table_name : "?");
}

static const char *workspace_name(const Workspace *ws) { return ws && ws->name[0] ? ws->name : "Workspace"; }

/* The workspace's connection tabs as the notcurses TUI has them: a group per
 * connection (in the order its first tab comes) and one per form. *current
 * gets the current tab's group. names[] and the strings are allocated. */
static size_t connection_groups(TuiState *state, const Workspace *ws, char ***names_out, size_t *current) {
  size_t n = 0;
  char **names = safe_calloc(ws->num_tabs + 1, sizeof *names);
  size_t *conn_of = safe_calloc(ws->num_tabs + 1, sizeof *conn_of); /* the group's connection, SIZE_MAX: a form */
  *current = 0;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    const Tab *t = &ws->tabs[i];
    if (!t->active)
      continue;
    size_t g = SIZE_MAX;
    if (t->type != TAB_TYPE_FORM)
      for (size_t k = 0; k < n; k++)
        if (conn_of[k] == t->connection_index)
          g = k;
    if (g == SIZE_MAX) {
      g = n++;
      conn_of[g] = t->type == TAB_TYPE_FORM ? SIZE_MAX : t->connection_index;
      names[g] = t->type == TAB_TYPE_FORM ? form_tab_label(t)
                                          : connection_name(state, app_get_connection(state->app, t->connection_index));
    }
    if (i == ws->current_tab)
      *current = g;
  }
  free(conn_of);
  *names_out = names;
  return n;
}

/* ==========================================================================
 * Header
 * ========================================================================== */

/* "↑id ↓v  v = 42  name contains 'x'" */
static char *sorts_filters(const Tab *tab) {
  StringBuilder *sb = sb_new(64);
  const TableSchema *sc = tab->schema;
  size_t parts = 0;
  for (size_t i = 0; sc && i < tab->num_sort_entries; i++) {
    size_t col = tab->sort_entries[i].column;
    if (col >= sc->num_columns)
      continue;
    sb_printf(sb, "%s%s%s", parts++ ? " " : "", tab->sort_entries[i].direction == SORT_DESC ? "\xe2\x86\x93" : "\xe2\x86\x91",
              sc->columns[col].name ? sc->columns[col].name : "");
  }
  for (size_t i = 0; i < tab->filters.num_filters; i++) {
    const ColumnFilter *f = &tab->filters.filters[i];
    if (!f->value[0] && filter_op_needs_value(f->op))
      continue; /* not applied */
    bool raw = f->op == FILTER_OP_RAW;
    const LaceColumn *c = !raw && sc && f->column_index < sc->num_columns ? &sc->columns[f->column_index] : NULL;
    char *text = chrome_filter_text(c ? c->name : NULL, raw ? LACE_FILTER_RAW : (LaceFilterOp)workspace_json_op_lace(f->op),
                                    f->value, f->value2, c && lace_type_is_numeric(c->type));
    sb_printf(sb, "%s%s", parts++ ? "  " : "", text);
    free(text);
  }
  return sb_finish(sb);
}

/* The cursor's position: "3 of 1,200" on a table, "3 of 10 rows" or "N rows
 * affected" in a query tab; NULL when there is nothing */
static char *position(const Tab *tab) {
  if (tab->type == TAB_TYPE_TABLE && tab->data && tab_has_subview(tab, SUBVIEW_DATA)) {
    size_t total = tab->total_rows > 0 ? tab->total_rows : tab->data->num_rows;
    size_t cur = tab->data->num_rows ? tab->loaded_offset + tab->cursor_row : 0;
    return chrome_position(cur, total, true, tab->row_count_approximate);
  }
  if (tab->type != TAB_TYPE_QUERY)
    return NULL;
  if (!tab->query_results && tab->query_exec_success) /* a statement that returns no rows */
    return str_printf("%lld %s affected", (long long)tab->query_affected, tab->query_affected == 1 ? "row" : "rows");
  if (!tab->query_results || !tab->query_results->num_columns)
    return NULL;
  size_t cur, extent;
  bool known = true;
  if (tab->query_paginated) {
    cur = tab->query_loaded_offset + tab->query_result_row;
    known = tab->query_total_known;
    extent = known ? tab->query_total_rows : tab->query_loaded_offset + tab->query_loaded_count;
  } else {
    cur = tab->query_result_row;
    extent = tab->query_results->num_rows;
  }
  char *p = chrome_position(cur, extent, known, false);
  if (!p)
    return NULL;
  char *out = str_printf("%s %s", p, known && extent == 1 ? "row" : "rows");
  free(p);
  return out;
}

void tui_draw_header(TuiState *state) {
  g_header.n = 0;
  if (!state || !state->header_win || !state->header_visible)
    return;
  WINDOW *w = state->header_win;
  int cols = getmaxx(w);
  wbkgd(w, tui_theme_attr(TC_HEADER_FG, TC_HEADER_BG) | ' ');
  werase(w);
  tui_style(w, TC_HEADER_FG, TC_HEADER_BG, A_BOLD);
  put(w, 0, 0, cols, "");
  Workspace *ws = TUI_WORKSPACE(state);
  const char *wname = workspace_name(ws);
  Tab *t = TUI_TAB(state);
  if (!ws || !ws->num_tabs || !t) {
    put(w, 0, 1, cols - 1, wname);
    wrefresh(w);
    return;
  }
  char **names;
  size_t current;
  size_t n = connection_groups(state, ws, &names, &current);
  Connection *conn = t->type == TAB_TYPE_FORM ? NULL : app_get_connection(state->app, t->connection_index);
  char qbuf[256];
  const char *view = t->type == TAB_TYPE_TABLE   ? t->table_name
                     : t->type == TAB_TYPE_QUERY ? chrome_query_title(t->query_text, qbuf, sizeof qbuf)
                                                 : NULL;

  char *pos = position(t);
  char *sf = t->type == TAB_TYPE_TABLE ? sorts_filters(t) : NULL;
  char *right = chrome_header_right(sf, pos, cols / 2);
  int right_w = text_width(right);
  int left_w = cols - 1 - (right_w ? right_w + 2 : 0);
  size_t off = 0, len = 0, db_off = 0, db_len = 0, v_off = 0, v_len = 0;
  char *line = chrome_header_format(wname, (const char *const *)names, n, current, conn ? conn->database : NULL, view,
                                    0, 0, left_w, &off, &len, &db_off, &db_len, &v_off, &v_len);
  if (db_len) { /* the database segment (a click there will switch databases) */
    char *head = str_ndup(line, db_off), *seg = str_ndup(line + db_off, db_len);
    int x0 = 1 + text_width(head);
    span_add(&g_header, x0, x0 + text_width(seg), 0);
    free(head);
    free(seg);
  }
  if (v_len && t->type == TAB_TYPE_TABLE) { /* a click on the view's name opens the table's actions */
    char *head = str_ndup(line, v_off), *seg = str_ndup(line + v_off, v_len);
    int x0 = 1 + text_width(head);
    span_add(&g_header, x0, x0 + text_width(seg), 1);
    free(head);
    free(seg);
  }
  tui_style(w, TC_HEADER_FG, TC_HEADER_BG, A_BOLD);
  put(w, 0, 1, left_w, line);
  if (right_w) {
    tui_style(w, TC_HEADER_FG, TC_HEADER_BG, 0);
    put_right(w, 0, cols - 1 - right_w, right_w, right);
  }
  if (len) { /* the connection's name in the accent colour */
    char *head = str_ndup(line, off), *cur = str_ndup(line + off, len);
    tui_style(w, TC_ACCENT, TC_HEADER_BG, A_BOLD);
    put(w, 0, 1 + text_width(head), text_width(cur), cur);
    free(head);
    free(cur);
  }
  tui_style(w, TC_HEADER_FG, TC_HEADER_BG, 0);
  free(line);
  free(right);
  free(pos);
  free(sf);
  for (size_t i = 0; i < n; i++)
    free(names[i]);
  free(names);
  wrefresh(w);
}

/* ==========================================================================
 * Tabs strip
 * ========================================================================== */

/* The active tabs' indexes, in order; *current the current one's position */
static size_t strip_entries(const Workspace *ws, size_t **out, size_t *current) {
  size_t *e = safe_calloc(ws->num_tabs + 1, sizeof *e);
  size_t n = 0;
  *current = SIZE_MAX;
  for (size_t i = 0; i < ws->num_tabs; i++) {
    if (!ws->tabs[i].active)
      continue;
    if (i == ws->current_tab)
      *current = n;
    e[n++] = i;
  }
  *out = e;
  return n;
}

void tui_draw_tabs(TuiState *state) {
  g_strip.n = g_strip_close.n = 0;
  if (!state || !state->tab_win)
    return;
  WINDOW *w = state->tab_win;
  int cols = getmaxx(w);
  wbkgd(w, tui_theme_attr(TC_DIM, TC_STRIPE_BG) | ' ');
  werase(w);
  tui_style(w, TC_DIM, TC_STRIPE_BG, 0);
  put(w, 0, 0, cols, "");
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws) {
    wrefresh(w);
    return;
  }
  size_t *e, current;
  size_t n = strip_entries(ws, &e, &current);
  if (!n) {
    put(w, 0, 1, cols - 1, "No views open: pick a table or press p for a query");
    free(e);
    wrefresh(w);
    return;
  }
  char **labels = safe_calloc(n, sizeof *labels);
  int *wd = safe_calloc(n, sizeof *wd), *x0 = safe_calloc(n, sizeof *x0), *x1 = safe_calloc(n, sizeof *x1);
  for (size_t i = 0; i < n; i++) {
    labels[i] = tab_label(state, &ws->tabs[e[i]]);
    wd[i] = chrome_strip_label_width(labels[i]) + (i == current ? 2 : 0);
  }
  size_t first = chrome_strip_layout(wd, n, current == SIZE_MAX ? 0 : current, cols, x0, x1);
  if (first)
    put(w, 0, 0, 1, "\xe2\x80\xb9"); /* ‹ */
  size_t last_shown = first;
  for (size_t i = first; i < n; i++) {
    if (x1[i] == x0[i])
      break;
    last_shown = i;
    bool cur = i == current;
    const Tab *t = &ws->tabs[e[i]];
    int lw = chrome_strip_label_width(labels[i]);
    /* a tab without a view of its own (a connection's, a form) in italics */
    attr_t italic = t->type == TAB_TYPE_CONNECTION || t->type == TAB_TYPE_FORM || t->preview ? A_ITALIC : 0;
    if (cur)
      tui_style(w, TC_FG, TC_BG, A_BOLD | italic);
    else
      tui_style(w, TC_DIM, TC_STRIPE_BG, italic);
    put(w, 0, x0[i], x1[i] - x0[i], "");
    put(w, 0, x0[i] + 1, lw, labels[i]);
    if (cur) {
      tui_style(w, TC_DIM, TC_BG, 0);
      put(w, 0, x0[i] + 1 + lw + 1, 1, "\xc3\x97"); /* × */
      span_add(&g_strip_close, x0[i] + 1 + lw + 1, x1[i], (int)e[i]);
      span_add(&g_strip, x0[i], x0[i] + 1 + lw + 1, (int)e[i]);
    } else {
      span_add(&g_strip, x0[i], x1[i], (int)e[i]);
    }
    if (i + 1 < n && x1[i + 1] > x1[i]) {
      tui_style(w, TC_DIM, TC_STRIPE_BG, 0);
      put(w, 0, x1[i], 1, "\xe2\x94\x82");
    }
  }
  if (last_shown + 1 < n) {
    tui_style(w, TC_DIM, TC_STRIPE_BG, 0);
    put(w, 0, cols - 1, 1, "\xe2\x80\xba"); /* › */
  }
  tui_style(w, TC_DIM, TC_STRIPE_BG, 0);
  for (size_t i = 0; i < n; i++)
    free(labels[i]);
  free(labels);
  free(wd);
  free(x0);
  free(x1);
  free(e);
  wrefresh(w);
}

/* ==========================================================================
 * Status line
 * ========================================================================== */

#define CTRL "\xe2\x8c\x83"

/* This TUI's query editor: Esc leaves it (the tab unfocused, where the
 * global keys work) */
static const ChromeHint H_EDITOR_LEAVE[] = {{CTRL "R run", HOTKEY_EXECUTE_QUERY},
                                            {CTRL "A run all", HOTKEY_EXECUTE_ALL},
                                            {CTRL "T transaction", HOTKEY_EXECUTE_TRANSACTION},
                                            {"Esc leave", CHROME_NOACT},
                                            {CTRL "P palette", HOTKEY_PALETTE},
                                            {NULL, 0}};
/* The new row under the grid */
static const ChromeHint H_ADD_ROW[] = {{"Enter edit", CHROME_NOACT}, {"F2 save row", CHROME_NOACT},
                                       {"Esc cancel", CHROME_NOACT}, {CTRL "N NULL", CHROME_NOACT},
                                       {CTRL "D empty", CHROME_NOACT}, {NULL, 0}};

/* The cursor column of a table's grid, or of query results that are one
 * table's rows, is a foreign key's first column */
static bool on_fk_column(TuiState *state, const Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (tab->type == TAB_TYPE_TABLE && tab->data && tab->schema && tab->cursor_col < tab->data->num_columns)
    return catalog_fk_first_column(tab->schema, tab->data->columns[tab->cursor_col].name);
  if (tab->type == TAB_TYPE_QUERY && ui && ui->query_focus_results && tab->query_results &&
      tab->query_source_schema && tab->query_result_col < tab->query_results->num_columns)
    return catalog_fk_first_column(tab->query_source_schema, tab->query_results->columns[tab->query_result_col].name);
  return false;
}

/* The hints of a sub-view list: the other sub-views by their keys, what
 * Enter does, the list's keys, the table's actions, refresh, help */
static ChromeHint *subview_hints(TuiState *state, const Tab *tab, char **texts) {
  static const HotkeyAction acts[SUBVIEW_COUNT] = {HOTKEY_SUBVIEW_DATA, HOTKEY_SUBVIEW_STRUCTURE,
                                                   HOTKEY_SUBVIEW_INDEXES, HOTKEY_SUBVIEW_FOREIGN_KEYS,
                                                   HOTKEY_SUBVIEW_INFORMATION};
  ChromeHint *hs = safe_calloc(SUBVIEW_COUNT + 8, sizeof *hs);
  size_t n = 0, nt = 0;
  hs[n++] = (ChromeHint){CTRL "P palette", HOTKEY_PALETTE};
  for (int i = 0; i < SUBVIEW_COUNT; i++) {
    if (!tab_has_subview(tab, (TabSubView)i) || (TabSubView)i == tab->subview)
      continue;
    const HotkeyBinding *b = &state->app->config->hotkeys[acts[i]];
    if (!b->num_keys || strlen(b->keys[0]) > 3)
      continue;
    char *label = str_dup(tui_subview_label(tab, (TabSubView)i));
    for (char *c = label; *c; c++)
      *c = (char)tolower((unsigned char)*c);
    texts[nt] = str_printf("%s %s", b->keys[0], label);
    free(label);
    hs[n++] = (ChromeHint){texts[nt++], acts[i]};
  }
  const char *enter = NULL, *keys = NULL;
  switch (tab->subview) {
  case SUBVIEW_STRUCTURE:
    if (tab->object == LACE_OBJ_TABLE) {
      enter = CHROME_SV_STRUCTURE_ENTER;
      keys = CHROME_SV_STRUCTURE_KEYS;
    } else {
      enter = CHROME_SV_DEFINITION_ENTER;
    }
    break;
  case SUBVIEW_INDEXES:
    enter = CHROME_SV_INDEXES_ENTER;
    keys = CHROME_SV_INDEXES_KEYS;
    break;
  case SUBVIEW_FOREIGN_KEYS:
    enter = CHROME_SV_FKS_ENTER;
    keys = CHROME_SV_FKS_KEYS;
    break;
  case SUBVIEW_INFORMATION: enter = CHROME_SV_INFO_ENTER; break;
  default: break;
  }
  if (enter)
    hs[n++] = (ChromeHint){enter, CHROME_NOACT};
  if (keys)
    hs[n++] = (ChromeHint){keys, CHROME_NOACT};
  hs[n++] = (ChromeHint){"m actions", HOTKEY_TABLE_MENU};
  hs[n++] = (ChromeHint){"F5 refresh", HOTKEY_REFRESH};
  hs[n++] = (ChromeHint){"? keys", HOTKEY_HELP};
  hs[n] = (ChromeHint){NULL, 0};
  return hs;
}

/* The hints of where the focus is; NULL for a sub-view list (built) */
static const ChromeHint *focused_hints(TuiState *state, Tab *tab) {
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab)
    return chrome_hints(HINTS_NONE);
  if (tab->type == TAB_TYPE_FORM)
    return chrome_hints(form_tab_list_focused(state, tab) ? HINTS_FORM_LIST : HINTS_FORM);
  if (state->editing || (ui && ui->query_result_editing))
    return chrome_hints(HINTS_EDIT);
  if (state->adding_row)
    return H_ADD_ROW;
  if (tui_sidebar_focused(state) && tui_sidebar_shown(state))
    return chrome_hints(HINTS_SIDEBAR);
  if (tab->type == TAB_TYPE_TABLE && tui_filters_visible(state) && tui_filters_focused(state))
    return chrome_hints(HINTS_FILTERS);
  if (tab->type == TAB_TYPE_QUERY && ui && !ui->query_unfocused && !ui->query_focus_results)
    return H_EDITOR_LEAVE;
  if (tab->type == TAB_TYPE_QUERY)
    return chrome_hints(on_fk_column(state, tab) ? HINTS_RESULTS_FK : HINTS_RESULTS);
  if (tab->type == TAB_TYPE_TABLE && tab->subview != SUBVIEW_DATA)
    return NULL;
  if (tab->type == TAB_TYPE_TABLE)
    return chrome_hints(on_fk_column(state, tab) ? HINTS_TABLE_FK : HINTS_TABLE);
  return chrome_hints(HINTS_NONE);
}

/* Record where each hint with an action sits when the hints start at x0;
 * a hint is cut at limit */
static void hints_record(const ChromeHint *hs, int x0, int limit) {
  int x = x0;
  for (; hs->text; hs++) {
    int w = text_width(hs->text);
    if (hs->action != CHROME_NOACT && x < limit)
      span_add(&g_status, x, x + w < limit ? x + w : limit, hs->action);
    x += w + 2;
  }
}

void tui_draw_status(TuiState *state) {
  g_status.n = 0;
  if (!state || !state->status_win || !state->status_visible)
    return;
  WINDOW *w = state->status_win;
  int cols = getmaxx(w);
  wbkgd(w, tui_theme_attr(TC_STATUS_FG, TC_STATUS_BG) | ' ');
  werase(w);
  tui_style(w, TC_STATUS_FG, TC_STATUS_BG, 0);
  put(w, 0, 0, cols, "");

  Tab *tab = TUI_TAB(state);
  DbConnection *db = TUI_CONN(state);
  bool lost = db && db->transaction_lost;
  bool open = db && (db->in_transaction || lost);
  bool failed = db && db->in_transaction && db->transaction_failed;
  const char *tx = tab && tab->type != TAB_TYPE_FORM ? chrome_tx_label(open, failed, lost) : "";

  char *sv_texts[SUBVIEW_COUNT] = {0};
  const ChromeHint *hs = focused_hints(state, tab);
  ChromeHint *sv_hints = hs ? NULL : subview_hints(state, tab, sv_texts);
  if (!hs)
    hs = sv_hints;
  char *hints = chrome_hints_join(hs);
  char *left = str_printf("%s%s", tx, hints);
  bool tx_bad = failed || lost;
  int badge_w = tx_bad ? text_width(tx) : 0;

  /* the message, one line, on the right */
  static unsigned spin;
  const char *msg = state->status_msg;
  char *right = str_printf("%c %s", state->bg_loading_active ? "|/-\\"[spin++ % 4] : ' ', msg ? msg : "");
  for (char *c = right; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t')
      *c = ' ';
  int rw = text_width(right);
  int left_w = text_width(left);
  int avail = cols - 1 - (1 + left_w + 2);
  bool show_hints = true;
  if (rw > avail && msg) { /* the message matters more than the hints, the badge stays */
    show_hints = false;
    left[strlen(tx)] = '\0';
    left_w = text_width(left);
    avail = cols - 1 - (1 + left_w);
  }
  if (show_hints)
    hints_record(hs, 1 + text_width(tx), rw > 0 ? cols - 2 - rw : cols - 1);
  put(w, 0, 1, cols - 1, left);
  if (badge_w) { /* [TX FAILED] / [TX LOST] stand out */
    tui_style(w, TC_ERROR_FG, TC_STATUS_BG, A_BOLD);
    put(w, 0, 1, badge_w < cols - 1 ? badge_w : cols - 1, tx);
  }
  if (msg && state->status_is_error)
    tui_style(w, TC_ERROR_FG, TC_STATUS_BG, A_BOLD);
  else
    tui_style(w, TC_STATUS_FG, TC_STATUS_BG, 0);
  if (rw > avail)
    rw = avail;
  if (rw > 0)
    put_right(w, 0, cols - 1 - rw, rw, right);
  tui_style(w, TC_STATUS_FG, TC_STATUS_BG, 0);
  free(left);
  free(hints);
  free(right);
  free(sv_hints);
  for (int i = 0; i < SUBVIEW_COUNT; i++)
    free(sv_texts[i]);
  wrefresh(w);
}

/* ==========================================================================
 * Mouse
 * ========================================================================== */

/* Close tab i of the current workspace (as - would, there) */
static void close_tab_at(TuiState *state, size_t i) {
  Workspace *ws = TUI_WORKSPACE(state);
  if (!ws || i >= ws->num_tabs)
    return;
  if (i != ws->current_tab)
    tab_switch(state, i);
  tab_close(state);
}

static void run_hint(TuiState *state, int action) {
  if (action == CHROME_FOCUS_EDITOR || action == CHROME_FOCUS_RESULTS) {
    UITabState *ui = TUI_TAB_UI(state);
    Tab *t = TUI_TAB(state);
    if (!ui || !t || t->type != TAB_TYPE_QUERY)
      return;
    tui_set_sidebar_focused(state, false);
    ui->query_unfocused = false;
    ui->query_focus_results = action == CHROME_FOCUS_RESULTS && t->query_results;
    return;
  }
  if (action >= 0 && action < HOTKEY_COUNT)
    tui_run_action(state, (HotkeyAction)action);
}

bool tui_chrome_mouse(TuiState *state, const UiEvent *ev) {
  if (!state || ev->type != UI_EVENT_MOUSE)
    return false;
  bool click = ev->mouse.action == UI_MOUSE_CLICK || ev->mouse.action == UI_MOUSE_DOUBLE_CLICK;
  int y = ev->mouse.y, x = ev->mouse.x;
  bool header = state->header_win && state->header_visible && y == getbegy(state->header_win);
  bool strip = state->tab_win && y == getbegy(state->tab_win);
  bool status = state->status_win && state->status_visible && y == getbegy(state->status_win);
  if (!header && !strip && !status)
    return false;
  if (!click) /* the wheel over the chrome does nothing */
    return true;
  if (state->editing)
    tui_confirm_edit(state);
  if (header && ev->mouse.button == UI_MOUSE_LEFT) {
    int what = span_find(&g_header, x);
    if (what == 0)
      tui_switch_database(state); /* the database segment */
    else if (what == 1)
      tui_table_menu(state);      /* the table's name */
    return true;
  }
  if (strip) {
    int close = span_find(&g_strip_close, x), cell = span_find(&g_strip, x);
    Workspace *ws = TUI_WORKSPACE(state);
    size_t n = ws ? ws->num_tabs : 0;
    if (close >= 0 && (size_t)close >= n)
      close = -1; /* the strip changed since the draw */
    if (cell >= 0 && (size_t)cell >= n)
      cell = -1;
    if (ev->mouse.button == UI_MOUSE_LEFT && close >= 0) {
      close_tab_at(state, (size_t)close);
    } else if (ev->mouse.button == UI_MOUSE_LEFT && cell >= 0 && ev->mouse.action == UI_MOUSE_CLICK) {
      if ((size_t)cell != ws->current_tab)
        tab_switch(state, (size_t)cell);
      tui_set_sidebar_focused(state, false);
      tui_set_filters_focused(state, false);
    } else if (ev->mouse.button == UI_MOUSE_MIDDLE && (cell >= 0 || close >= 0)) {
      close_tab_at(state, (size_t)(cell >= 0 ? cell : close));
    }
    return true;
  }
  if (status && ev->mouse.button == UI_MOUSE_LEFT && ev->mouse.action == UI_MOUSE_CLICK) {
    int action = span_find(&g_status, x);
    if (action >= 0)
      run_hint(state, action);
  }
  return true;
}
