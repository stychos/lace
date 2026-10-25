#include "app.h"
#include "conn/connection.h"
#include "model/workspace.h"
#include "ui/form_tab.h"
#include "ui/overlay.h"
#include "ui/sidebar.h"
#include "ui/subviews.h"
#include <notcurses/notcurses.h>
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *tx_label(NcTxState s) {
  switch (s) {
  case NC_TX_OPEN: return "[TX] ";
  case NC_TX_FAILED: return "[TX FAILED] ";
  case NC_TX_LOST: return "[TX LOST] ";
  default: return "";
  }
}

/* A hint is a key and a label; one with an action runs it when clicked */
typedef struct { const char *text; int action; } Hint; /* action HOTKEY_COUNT: none */
#define NOACT HOTKEY_COUNT
#define CTRL "\xe2\x8c\x83"
static const Hint H_EDIT[] = {{"Enter save", NOACT}, {"Esc cancel", NOACT}, {CTRL "N NULL", NOACT},
                              {CTRL "D empty", NOACT}, {CTRL "K copy", NOACT}, {CTRL "U paste", NOACT}, {NULL, 0}};
static const Hint H_EDITOR[] = {{CTRL "R run", HOTKEY_EXECUTE_QUERY}, {CTRL "A run all", HOTKEY_EXECUTE_ALL},
                                {CTRL "T transaction", HOTKEY_EXECUTE_TRANSACTION},
                                {"Esc results", HINT_FOCUS_RESULTS}, {CTRL "\xe2\x86\x91/" CTRL "\xe2\x86\x93 split", NOACT}, {NULL, 0}};
static const Hint H_RESULTS[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"Enter editor", HINT_FOCUS_EDITOR},
                                 {"e edit", HOTKEY_EDIT_MODAL}, {"x delete", HOTKEY_DELETE_ROW},
                                 {"r refresh", HOTKEY_REFRESH}, {"p new query", HOTKEY_OPEN_QUERY},
                                 {"? keys", HOTKEY_HELP}, {NULL, 0}};
static const Hint H_TABLE[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"/ filter", HOTKEY_TOGGLE_FILTERS},
                               {"o sort", HOTKEY_CYCLE_SORT}, {"Enter edit", HOTKEY_EDIT_INLINE},
                               {"e editor", HOTKEY_EDIT_MODAL}, {"+ add", HOTKEY_ROW_ADD},
                               {"x delete", HOTKEY_DELETE_ROW}, {"t tables", HOTKEY_TOGGLE_SIDEBAR},
                               {"? keys", HOTKEY_HELP}, {NULL, 0}};
/* the same with the cursor on a foreign key column */
static const Hint H_TABLE_FK[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"/ filter", HOTKEY_TOGGLE_FILTERS},
                                  {"o sort", HOTKEY_CYCLE_SORT}, {"Enter edit", HOTKEY_EDIT_INLINE},
                                  {CTRL "Enter follow", HOTKEY_FOLLOW_FK}, {"e editor", HOTKEY_EDIT_MODAL},
                                  {"+ add", HOTKEY_ROW_ADD}, {"x delete", HOTKEY_DELETE_ROW},
                                  {"t tables", HOTKEY_TOGGLE_SIDEBAR}, {"? keys", HOTKEY_HELP}, {NULL, 0}};
static const Hint H_RESULTS_FK[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"Enter editor", HINT_FOCUS_EDITOR},
                                    {CTRL "Enter follow", HOTKEY_FOLLOW_FK}, {"e edit", HOTKEY_EDIT_MODAL},
                                    {"x delete", HOTKEY_DELETE_ROW}, {"r refresh", HOTKEY_REFRESH},
                                    {"p new query", HOTKEY_OPEN_QUERY}, {"? keys", HOTKEY_HELP}, {NULL, 0}};
static const Hint H_FILTERS[] = {{"+ add", HOTKEY_ADD_FILTER}, {"- remove", HOTKEY_REMOVE_FILTER},
                                 {"X clear", HOTKEY_CLEAR_FILTERS}, {"Enter edit", NOACT}, {"Tab next", NOACT},
                                 {"Esc table", NOACT}, {"/ close", HOTKEY_TOGGLE_FILTERS}, {NULL, 0}};
static const Hint H_SIDEBAR[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"/ search", HOTKEY_SIDEBAR_FILTER},
                                 {"Enter open", NOACT}, {"+ new tab", HOTKEY_NEW_TAB}, {"Tab content", NOACT}, {"Esc back", NOACT},
                                 {"t hide", HOTKEY_TOGGLE_SIDEBAR}, {"? keys", HOTKEY_HELP}, {NULL, 0}};
static const Hint H_FORM[] = {{"Enter open", HOTKEY_CONN_OPEN}, {CTRL "B background", HOTKEY_CONN_OPEN_BACKGROUND},
                              {CTRL "N workspace", HOTKEY_CONN_OPEN_NEW_WORKSPACE},
                              {CTRL "O to\xe2\x80\xa6", HOTKEY_CONN_OPEN_TO_WORKSPACE}, {CTRL "S save", HOTKEY_CONN_SAVE},
                              {CTRL "T test", HOTKEY_CONN_TEST}, {CTRL "L clear", HOTKEY_CONN_CLEAR},
                              {CTRL "P palette", HOTKEY_PALETTE}, {NULL, 0}};
static const Hint H_FORM_LIST[] = {{"Enter open", HOTKEY_CONN_OPEN}, {"N folder", HOTKEY_CONN_NEW_FOLDER},
                                   {"r rename", HOTKEY_CONN_RENAME}, {"d duplicate", HOTKEY_CONN_DUPLICATE},
                                   {"Space move", HOTKEY_CONN_MOVE}, {"x delete", HOTKEY_CONN_DELETE},
                                   {"/ filter", HOTKEY_CONN_FILTER}, {CTRL "S save", HOTKEY_CONN_SAVE},
                                   {CTRL "P palette", HOTKEY_PALETTE}, {NULL, 0}};
static const Hint H_NONE[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"? keys", HOTKEY_HELP},
                              {"q quit", HOTKEY_QUIT}, {NULL, 0}};

/* The cursor column of the grid is a foreign key's first column */
static bool on_fk_column(const View *v) {
  if (!v || !v->schema || !page_cache_num_columns(v->cache)) return false;
  const LaceColumn *c = page_cache_column(v->cache, v->cursor_col);
  return c && c->name && view_column_is_fk(v, c->name);
}

/* The hints of a sub-view list: the switches by their keys, then Enter and refresh */
static Hint *subview_hints(NcApp *app, const View *v, char **texts) {
  static const HotkeyAction acts[SV_COUNT] = {HOTKEY_SUBVIEW_DATA, HOTKEY_SUBVIEW_STRUCTURE, HOTKEY_SUBVIEW_INDEXES,
                                              HOTKEY_SUBVIEW_FOREIGN_KEYS, HOTKEY_SUBVIEW_INFORMATION};
  Hint *hs = safe_calloc(SV_COUNT + 6, sizeof *hs);
  size_t n = 0, nt = 0;
  hs[n++] = (Hint){CTRL "P palette", HOTKEY_PALETTE};
  for (int i = 0; i < SV_COUNT; i++) {
    if (!view_has_subview(v, (SubView)i) || (SubView)i == v->subview) continue;
    const HotkeyBinding *b = &app->config->hotkeys[acts[i]];
    if (!b->num_keys || strlen(b->keys[0]) > 3) continue;
    char *label = str_dup(subview_label(v, (SubView)i));
    for (char *c = label; *c; c++) *c = (char)tolower((unsigned char)*c);
    texts[nt] = str_printf("%s %s", b->keys[0], label);
    free(label);
    hs[n++] = (Hint){texts[nt++], acts[i]};
  }
  const char *enter = subview_enter_hint(v);
  if (enter) hs[n++] = (Hint){enter, NOACT};
  hs[n++] = (Hint){"r refresh", HOTKEY_REFRESH};
  hs[n++] = (Hint){"? keys", HOTKEY_HELP};
  hs[n] = (Hint){NULL, 0};
  return hs;
}

static char *hints_join(const Hint *hs) {
  char *out = str_dup("");
  for (; hs->text; hs++) {
    char *n = str_printf("%s%s%s", out, *out ? "  " : "", hs->text);
    free(out);
    out = n;
  }
  return out;
}

/* Record where each hint with an action sits when the hints start at column x0;
 * a hint is cut at limit */
static void hints_record(NcApp *app, const Hint *hs, int x0, int limit) {
  int x = x0;
  for (; hs->text; hs++) {
    int w = nc_text_width(hs->text);
    if (hs->action != NOACT && x < limit) hit_add(&app->status_spans, x, x + w < limit ? x + w : limit, hs->action);
    x += w + 2;
  }
}

void status_draw(NcApp *app) {
  app->status_spans.n = 0;
  int y = app->rows - 1;
  nc_style(app->std, &app->theme, TC_STATUS_FG, TC_STATUS_BG, 0);
  nc_put(app->std, y, 0, app->cols, "");
  Overlay *top = overlay_top(app);
  ConnTab *tab = app_current_tab(app);
  NcTxState tx = tab && tab->conn ? nc_connection_tx_state(tab->conn) : NC_TX_NONE;
  const View *view = tab ? conntab_current_view(tab) : NULL;
  const Hint *hs = tab && tab->form ? (form_tab_list_focused(tab) ? H_FORM_LIST : H_FORM)
                   : view && view->editing ? H_EDIT
                   : tab && tab->sidebar_focused && sidebar_width(app) ? H_SIDEBAR
                   : view && view->kind == VIEW_TABLE && view->filters_visible && view->fp.focused ? H_FILTERS
                   : view && view->kind == VIEW_QUERY && !view->focus_results ? H_EDITOR
                   : view && view->kind == VIEW_QUERY ? (on_fk_column(view) ? H_RESULTS_FK : H_RESULTS)
                   : view && view->kind == VIEW_TABLE && view->subview != SV_DATA ? NULL
                   : view && view->kind == VIEW_TABLE ? (on_fk_column(view) ? H_TABLE_FK : H_TABLE)
                                                      : H_NONE;
  char *sv_texts[SV_COUNT] = {0};
  Hint *sv_hints = hs ? NULL : subview_hints(app, view, sv_texts);
  if (!hs) hs = sv_hints;
  char *hints = hints_join(hs);
  bool tx_bad = !top && (tx == NC_TX_FAILED || tx == NC_TX_LOST);
  char *left_buf = top ? str_dup(top->footer ? top->footer : "") : str_printf("%s%s", tx_label(tx), hints);
  int badge_w = tx_bad ? nc_text_width(tx_label(tx)) : 0;
  /* the status text, one line */
  char *right = str_printf("%c %s", app_busy(app) ? "|/-\\"[app->spinner % 4] : ' ',
                           app->status ? app->status : "");
  for (char *c = right; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  int w = nc_text_width(right);
  int left_w = nc_text_width(left_buf);
  int avail = app->cols - 1 - (1 + left_w + 2);
  bool show_hints = true;
  if (w > avail && app->status && !top) { /* the message matters more than the hints */
    show_hints = false;                    /* but the transaction badge stays */
    left_buf[strlen(tx_label(tx))] = '\0';
    left_w = nc_text_width(left_buf);
    avail = app->cols - 1 - (1 + left_w);
  }
  if (show_hints && !top)
    hints_record(app, hs, 1 + nc_text_width(tx_label(tx)), w > 0 ? app->cols - 2 - w : app->cols - 1);
  nc_put(app->std, y, 1, app->cols - 1, left_buf);
  if (badge_w) { /* [TX FAILED] / [TX LOST] stand out */
    char *badge = str_ndup(left_buf, strlen(tx_label(tx)));
    nc_style(app->std, &app->theme, TC_ERROR_FG, TC_STATUS_BG, NCSTYLE_BOLD);
    nc_put(app->std, y, 1, badge_w < app->cols - 1 ? badge_w : app->cols - 1, badge);
    free(badge);
  }
  if (app->status && app->status_error) nc_style(app->std, &app->theme, TC_ERROR_FG, TC_STATUS_BG, 0);
  if (w > avail) w = avail;
  if (w > 0) nc_put_right(app->std, y, app->cols - 1 - w, w, right);
  free(left_buf);
  free(hints);
  free(right);
  free(sv_hints);
  for (int i = 0; i < SV_COUNT; i++) free(sv_texts[i]);
}
