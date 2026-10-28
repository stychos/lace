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

#include "core/list_model.h" /* the rows, filter, cursor and keys (shared with the ncurses TUI) */

void list_draw(ListWidget *l, struct ncplane *p, const Theme *t, int y, int x, int rows, int cols,
               int *cursor_y, int *cursor_x);
#endif
