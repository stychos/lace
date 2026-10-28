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
  return chrome_tx_label(s == NC_TX_OPEN || s == NC_TX_FAILED, s == NC_TX_FAILED, s == NC_TX_LOST);
}

/* A hint is a key and a label; one with an action runs it when clicked
 * (core/chrome.h holds the sets, shared with the ncurses TUI) */
typedef ChromeHint Hint;
#define NOACT CHROME_NOACT
#define CTRL "\xe2\x8c\x83"
#define H_EDIT chrome_hints(HINTS_EDIT)
#define H_EDITOR chrome_hints(HINTS_EDITOR)
#define H_RESULTS chrome_hints(HINTS_RESULTS)
#define H_RESULTS_FK chrome_hints(HINTS_RESULTS_FK)
#define H_TABLE chrome_hints(HINTS_TABLE)
#define H_TABLE_FK chrome_hints(HINTS_TABLE_FK)
#define H_FILTERS chrome_hints(HINTS_FILTERS)
#define H_SIDEBAR chrome_hints(HINTS_SIDEBAR)
#define H_FORM chrome_hints(HINTS_FORM)
#define H_FORM_LIST chrome_hints(HINTS_FORM_LIST)
#define H_NONE chrome_hints(HINTS_NONE)

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
  Hint *hs = safe_calloc(SV_COUNT + 8, sizeof *hs);
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
  const char *keys = subview_keys_hint(v);
  if (keys) hs[n++] = (Hint){keys, NOACT};
  hs[n++] = (Hint){"m actions", HOTKEY_TABLE_MENU};
  hs[n++] = (Hint){"F5 refresh", HOTKEY_REFRESH};
  hs[n++] = (Hint){"? keys", HOTKEY_HELP};
  hs[n] = (Hint){NULL, 0};
  return hs;
}

static char *hints_join(const Hint *hs) { return chrome_hints_join(hs); }

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
