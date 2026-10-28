/*
 * Lace
 * The screen chrome's text, shared by both TUIs (no drawing): the header's
 * path and right side, the tabs strip's layout and the status line's
 * hints. lace-nc draws it with notcurses (ui/header.c, ui/tabstrip.c,
 * ui/status.c), lace with ncurses (chrome.c).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CORE_CHROME_H
#define LACE_CORE_CHROME_H

#include "../config/config.h"
#include "../../../liblace/include/sql_builder.h"
#include <stdbool.h>
#include <stddef.h>

/* The header's left part, the path to the current view:
 *   "<workspace> ▸ <connection> ▸ <database> ▸ <view> (<i> of <n>)"
 * names are the workspace's connection tabs (forms included), current the
 * one shown; a name another tab shares gets its position among the equal
 * names appended ("t.sqlite 2"). When it does not fit in width columns the
 * workspace name goes, then the line is clipped. database NULL or empty:
 * no database part (SQLite); view NULL: none; view_count 0: no "(i of n)";
 * workspace NULL: none. The offset and length pairs (each may be NULL)
 * get the byte ranges of the connection name, the database and the view's name in
 * the result (len 0 when absent or clipped away). Allocated. */
char *chrome_header_format(const char *workspace, const char *const *names, size_t num_names, size_t current,
                           const char *database, const char *view, size_t view_pos, size_t view_count, int width,
                           size_t *cur_off, size_t *cur_len, size_t *db_off, size_t *db_len, size_t *view_off,
                           size_t *view_len);

/* 12345 -> "12,345" (allocated) */
char *chrome_commas(size_t n);

/* "3 of 1,200": cursor is 0-based; "≈" before an estimate, "+" after a
 * lower bound (known false); NULL when nothing is known yet */
char *chrome_position(size_t cursor, size_t extent, bool known, bool approximate);

/* One filter as the header shows it: "v = 42", "name contains 'x'",
 * "v between 1 and 5", "deleted is NULL", "(raw SQL)" (op RAW or no
 * column); numeric values go unquoted */
char *chrome_filter_text(const char *column, LaceFilterOp op, const char *value, const char *value2, bool numeric);

/* The header's right side in at most max_cols columns: the sorts and
 * filters (cut first, with "…"), two spaces, the position. NULL when
 * there is neither. */
char *chrome_header_right(const char *sorts_filters, const char *position, int max_cols);

/* The filter panel's fields in a row cols wide: the column, the operator,
 * the value (two with " and " between for BETWEEN), the remove mark */
typedef struct {
  int col_x, col_w, op_x, op_w, val_x, val_w, val2_x, val2_w, del_x;
} ChromeFilterLayout;
void chrome_filter_layout(int cols, bool two_values, ChromeFilterLayout *L);

/* The tabs strip */
#define CHROME_TAB_LABEL_MAX 24  /* display columns of a label before it is cut */
#define CHROME_STRIP_MIN_ROWS 14 /* below this the strip gives its row to the content */

/* Lay out n tabs of the given label widths (cells are " label " plus one
 * separator column) in width columns, scrolled so that the current one
 * shows whole; x0/x1 (n entries) get each tab's cell [x0, x1), 0/0 when it
 * is off the strip. Returns the first tab shown. */
size_t chrome_strip_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1);

/* A label's width on the strip: its display width, at most the maximum */
int chrome_strip_label_width(const char *label);

/* The status line's hints: a key and a label; one with an action runs it
 * when clicked (CHROME_NOACT: none; the focus moves are no hotkeys) */
typedef struct {
  const char *text;
  int action;
} ChromeHint;

#define CHROME_NOACT HOTKEY_COUNT
#define CHROME_FOCUS_EDITOR (HOTKEY_COUNT + 1)
#define CHROME_FOCUS_RESULTS (HOTKEY_COUNT + 2)

typedef enum {
  HINTS_EDIT,        /* an inline cell edit */
  HINTS_EDITOR,      /* a query editor with the focus */
  HINTS_RESULTS,     /* query results */
  HINTS_RESULTS_FK,  /* ... the cursor on a foreign key's column */
  HINTS_TABLE,       /* a table's grid */
  HINTS_TABLE_FK,    /* ... the cursor on a foreign key's column */
  HINTS_FILTERS,     /* the filter panel */
  HINTS_SIDEBAR,     /* the sidebar */
  HINTS_FORM,        /* a connection form's fields */
  HINTS_FORM_LIST,   /* a connection form's saved connections */
  HINTS_NONE,        /* nothing open */
  HINTS_COUNT
} ChromeHintSet;

/* A sub-view list's hints: what Enter does and the list's own keys */
#define CHROME_SV_STRUCTURE_ENTER "Enter edit"
#define CHROME_SV_STRUCTURE_KEYS "+ add  x drop  R rename  P key  Space move"
#define CHROME_SV_INDEXES_ENTER "Enter rename"
#define CHROME_SV_INDEXES_KEYS "+ index  c check  x drop  R rename"
#define CHROME_SV_FKS_ENTER "Enter edit/open"
#define CHROME_SV_FKS_KEYS "+ add  x drop  R rename"
#define CHROME_SV_INFO_ENTER "Enter count/open/act"
#define CHROME_SV_DEFINITION_ENTER "Enter edit in a query view"

/* The hints of a place, ended by a NULL text */
const ChromeHint *chrome_hints(ChromeHintSet set);

/* The hints joined by two spaces (allocated) */
char *chrome_hints_join(const ChromeHint *hs);

/* "[TX] ", "[TX FAILED] ", "[TX LOST] " or "" */
const char *chrome_tx_label(bool open, bool failed, bool lost);

/* A query's title on the strip and in the header: its first line, cut at
 * 40 columns, or "New query" for an empty text; written into buf */
const char *chrome_query_title(const char *text, char *buf, size_t cap);

/* A message wrapped to width columns by words (a word wider than a line
 * is cut; newlines start lines): *out gets the lines, each allocated, and
 * the array; returns their count (at least one) */
size_t chrome_wrap(const char *text, int width, char ***out);
void chrome_wrap_free(char **lines, size_t n);

/* What a connection string is called in the header and on the strip when
 * no saved connection names it: a SQLite file's base name, else "db on
 * host" (allocated; "database" for a string that does not parse) */
char *chrome_connstr_short_name(const char *connstr);

#endif /* LACE_CORE_CHROME_H */
