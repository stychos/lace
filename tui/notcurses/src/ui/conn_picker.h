/* The left panel of the connection form tab: the saved connections tree
 * (folders) and Recent, as list rows, with an optional filter (in). */
#ifndef LACE_NC_CONN_PICKER_H
#define LACE_NC_CONN_PICKER_H
#include "app.h"
#include "ui/list.h"
#include "views/text_input.h"

/* Row kinds; a row's data is its index: the filtered tree index for
 * CPK_CONN/CPK_FOLDER (connmgr_get_filtered_item with the filter), the
 * recent list index for CPK_RECENT */
typedef enum { CPK_CONN = 1, CPK_FOLDER, CPK_RECENT } CpKind;

typedef struct {
  TextInput in;     /* the filter */
  ListWidget list;
  bool input_focused;
  unsigned seen;    /* cp_changed() counter the rows were built for */
} ConnPicker;

void cp_init(NcApp *app, ConnPicker *p, bool input_focused);
void cp_free(ConnPicker *p);
void cp_rebuild(NcApp *app, ConnPicker *p); /* keeps the cursor on the same kind and index */
void cp_changed(void); /* the tree or the recent list changed */
/* The list alone, rebuilt first */
void cp_draw_list(NcApp *app, ConnPicker *p, struct ncplane *pl, int y, int x, int rows, int cols);
/* A text field: scrolled so the cursor shows; cursor_x gets its column */
void nc_draw_input(struct ncplane *pl, const Theme *t, int y, int x, int w, const TextInput *in,
                   ThemeColor fg, ThemeColor bg, int *cursor_x);
#endif
