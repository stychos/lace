/*
 * Lace GTK frontend
 * LaceCellEditor: the popover that edits one cell
 *
 * Opens over a cell with its full, exact text (hex for a BLOB). Enter saves,
 * Shift+Enter inserts a line break, Esc cancels; Set NULL and Set empty are
 * explicit actions, so an empty text is '' and only Set NULL writes NULL.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_CELL_EDITOR_H
#define LACE_GTK_CELL_EDITOR_H

#include <adwaita.h>
#include <lace.h>

G_BEGIN_DECLS

#define LACE_TYPE_CELL_EDITOR (lace_cell_editor_get_type())
G_DECLARE_FINAL_TYPE(LaceCellEditor, lace_cell_editor, LACE, CELL_EDITOR, GtkPopover)

LaceCellEditor *lace_cell_editor_new(void);

/* Show the editor pointing at rect (in the popover parent's coordinates)
 * for a value of a column. Emits "committed" (const char *text, gboolean
 * set_null) or "cancelled". */
void lace_cell_editor_open(LaceCellEditor *ed, const GdkRectangle *rect,
                           const LaceValue *value, const char *column_name,
                           LaceValueType type, const char *type_name,
                           gboolean editable);

/* Replace the text (after open): typing into a cell starts with that key */
void lace_cell_editor_set_text(LaceCellEditor *ed, const char *text);

G_END_DECLS

#endif /* LACE_GTK_CELL_EDITOR_H */
