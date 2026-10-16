/* A filterable, scrollable list with optional header rows: the body of the
 * palette, the help overlay and (later) pickers. */
#ifndef NC_LIST_H
#define NC_LIST_H
#include "config/config.h"
#include "core/ui_types.h"
#include "ui/theme.h"
#include "views/text_input.h"
#include <stdbool.h>
#include <stddef.h>

struct ncplane;

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
void list_draw(ListWidget *l, struct ncplane *p, const Theme *t, int y, int x, int rows, int cols,
               int *cursor_y, int *cursor_x);
/* Width of the detail column: detail_cols if set, else the widest detail
 * among the shown rows; at most a third of cols */
int list_detail_width(const ListWidget *l, int cols);
ListRow *list_selected(ListWidget *l);
#endif
