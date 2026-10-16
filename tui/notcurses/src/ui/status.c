#include "app.h"
#include "conn/connection.h"
#include "model/workspace.h"
#include "ui/overlay.h"
#include "ui/sidebar.h"
#include <notcurses/notcurses.h>
#include "util/str.h"
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
static const Hint H_TABLE[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"f filter", HOTKEY_TOGGLE_FILTERS},
                               {"o sort", HOTKEY_CYCLE_SORT}, {"Enter edit", HOTKEY_EDIT_INLINE},
                               {"e editor", HOTKEY_EDIT_MODAL}, {"+ add", HOTKEY_ROW_ADD},
                               {"x delete", HOTKEY_DELETE_ROW}, {"t tables", HOTKEY_TOGGLE_SIDEBAR},
                               {"? keys", HOTKEY_HELP}, {NULL, 0}};
static const Hint H_FILTERS[] = {{"+ add", HOTKEY_ADD_FILTER}, {"- remove", HOTKEY_REMOVE_FILTER},
                                 {"X clear", HOTKEY_CLEAR_FILTERS}, {"Enter edit", NOACT}, {"Tab next", NOACT},
                                 {"Esc table", NOACT}, {"f close", HOTKEY_TOGGLE_FILTERS}, {NULL, 0}};
static const Hint H_SIDEBAR[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"/ search", HOTKEY_SIDEBAR_FILTER},
                                 {"Enter open", NOACT}, {"+ new tab", HOTKEY_NEW_TAB}, {"Tab content", NOACT}, {"Esc back", NOACT},
                                 {"t hide", HOTKEY_TOGGLE_SIDEBAR}, {"? keys", HOTKEY_HELP}, {NULL, 0}};
static const Hint H_NONE[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"? keys", HOTKEY_HELP},
                              {"q quit", HOTKEY_QUIT}, {NULL, 0}};

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
  const Hint *hs = view && view->editing ? H_EDIT
                   : tab && tab->sidebar_focused && sidebar_width(app) ? H_SIDEBAR
                   : view && view->kind == VIEW_TABLE && view->filters_visible && view->fp.focused ? H_FILTERS
                   : view && view->kind == VIEW_QUERY && !view->focus_results ? H_EDITOR
                   : view && view->kind == VIEW_QUERY ? H_RESULTS
                   : view && view->kind == VIEW_TABLE ? H_TABLE
                                                      : H_NONE;
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
}
