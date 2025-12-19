/*
 * lace - Database Viewer and Manager
 * Modal cell editor view
 */

#ifndef LACE_EDITOR_VIEW_H
#define LACE_EDITOR_VIEW_H

#include "../tui.h"

/* Result of editor modal */
typedef struct {
  bool saved;    /* True if user saved, false if cancelled */
  bool set_null; /* True if user requested NULL (Ctrl+N) */
  char *content; /* New content (only if saved, caller must free) */
} EditorResult;

/*
 * Show modal editor for cell content
 * @param state     TUI state
 * @param title     Title for the editor window (e.g., "Edit: column_name")
 * @param content   Initial content (can be NULL)
 * @param readonly  If true, editor is read-only (just viewing)
 * @return          EditorResult with saved flag and new content
 */
EditorResult editor_view_show(TuiState *state, const char *title,
                              const char *content, bool readonly);

#endif /* LACE_EDITOR_VIEW_H */
