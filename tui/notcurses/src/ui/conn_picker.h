/* The saved connections / recent URLs picker with its input line: the body
 * of the welcome screen and of the connect overlay (dialogs/connect.c). */
#ifndef LACE_NC_CONN_PICKER_H
#define LACE_NC_CONN_PICKER_H
#include "app.h"
#include "ui/list.h"
#include "views/text_input.h"

typedef enum { CPK_CONN = 1, CPK_FOLDER, CPK_RECENT } CpKind;
typedef enum {
  CP_NONE, CP_CONNECT_TEXT, CP_ROW, CP_ROW_TAB, CP_ROW_WORKSPACE, CP_EDIT, CP_NEW, CP_DELETE,
  CP_FORGET, CP_SAVE
} CpAction;
typedef struct { bool consumed; CpAction act; } CpResult;

typedef struct {
  TextInput in;
  ListWidget list;
  bool input_focused;
  unsigned seen; /* cp_changed() counter the rows were built for */
} ConnPicker;

void cp_init(NcApp *app, ConnPicker *p, bool input_focused);
void cp_free(ConnPicker *p);
void cp_rebuild(NcApp *app, ConnPicker *p);
void cp_changed(void); /* the tree or the recent list changed: pickers rebuild when drawn */
/* picker_mode: Esc is left to the overlay (closes it); otherwise Esc clears
 * the text and moves to the list */
CpResult cp_key(NcApp *app, ConnPicker *p, const UiEvent *ev, bool picker_mode);
/* Does what act says. A connect pops overlay o (if not NULL) first, which
 * frees p: do not touch p afterwards. */
void cp_perform(NcApp *app, ConnPicker *p, CpAction act, struct Overlay *o);
void cp_draw(NcApp *app, ConnPicker *p, struct ncplane *pl, int y, int x, int rows, int cols,
             int *cursor_y, int *cursor_x);
/* A text field: scrolled so the cursor shows; cursor_x gets its column */
void nc_draw_input(struct ncplane *pl, const Theme *t, int y, int x, int w, const TextInput *in,
                   ThemeColor fg, ThemeColor bg, int *cursor_x);
#endif
