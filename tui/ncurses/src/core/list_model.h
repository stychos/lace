/*
 * Lace
 * A filterable, scrollable list with optional header rows, shared by both
 * TUIs (the body of their palettes, pick lists, preferences, history and
 * workspaces dialogs): rows, the filter, the cursor, keys and mouse. Each
 * TUI draws it (notcurses ui/list.c, ncurses views/list_draw.c).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_LIST_MODEL_H
#define LACE_LIST_MODEL_H

#include "../config/config.h"
#include "ui_types.h"
#include "../views/text_input.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *text, *detail, *keys;
  int kind;
  void *data;   /* not owned */
  bool header;
} ListRow;

typedef struct {
  TextInput input;
  bool input_focused, has_input;
  ListRow *rows;
  size_t num_rows;
  size_t *shown;       /* indexes into rows, in display order */
  size_t num_shown;
  size_t cursor, scroll; /* positions in shown */
  int (*score)(const char *needle, const ListRow *row); /* -1: hidden; NULL: case-insensitive substring */
  int detail_cols;     /* width reserved for the detail column, 0 = none */
  int page;            /* rows visible at the last draw (PageUp/PageDown step) */
  bool hide_cursor;    /* draw no cursor row (nothing picked yet); the cursor stays where it is */
} ListWidget;

typedef enum { LIST_NONE, LIST_CHANGED, LIST_ACTIVATE, LIST_CANCEL } ListAction;

/* The config whose navigation hotkeys (move_up/down, page_up/down,
 * first/last_row: j, k, g, G, ...) every list takes besides the arrows while
 * its input is not focused; set once at startup */
void list_use_hotkeys(const Config *config);
void list_init(ListWidget *l, bool has_input);
void list_free(ListWidget *l);
void list_set_rows(ListWidget *l, ListRow *rows, size_t n); /* adopts */
void list_filter(ListWidget *l);
ListAction list_key(ListWidget *l, const UiEvent *ev);
/* Wheel scrolls, a click selects (and activates) a row; y is the content row
 * where the list starts, rows its height */
ListAction list_mouse(ListWidget *l, const UiEvent *ev, int y, int rows);
ListRow *list_selected(ListWidget *l);
/* Width of the detail column: detail_cols if set, else the widest detail
 * among the shown rows; at most a third of cols */
int list_detail_width(const ListWidget *l, int cols);
/* The shown row at position pos may hold the cursor (not a header) */
bool list_selectable(const ListWidget *l, size_t pos);

#endif /* LACE_LIST_MODEL_H */
