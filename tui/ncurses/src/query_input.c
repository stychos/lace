/*
 * Lace
 * Query tab keys: editor, results, and the dispatch between them
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include "query_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Keys while the results pane has focus. Returns true if handled. */
static bool query_results_handle_key(TuiState *state, Tab *tab, UITabState *ui,
                                     const UiEvent *event, const Config *cfg) {
  if (!tab->query_results) {
    /* No results yet - up arrow switches to editor */
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
      ui->query_focus_results = false;
      return true;
    }
    return false;
  }

  /* Enter - start editing */
  if (hotkey_matches(cfg, event, HOTKEY_EDIT_INLINE)) {
    query_result_start_edit(state, tab);
    return true;
  }

  /* e or F4 - start modal editing */
  if (hotkey_matches(cfg, event, HOTKEY_EDIT_MODAL)) {
    query_result_start_modal_edit(state, tab);
    return true;
  }

  /* u, Ctrl+Z - revert the last write */
  if (hotkey_matches(cfg, event, HOTKEY_UNDO)) {
    tui_undo(state);
    return true;
  }

  /* Ctrl+Enter / Alt+Enter - the referenced table; Alt+R - the referencing ones */
  if (hotkey_matches(cfg, event, HOTKEY_FOLLOW_FK)) {
    tui_follow_fk(state);
    return true;
  }
  if (hotkey_matches(cfg, event, HOTKEY_REFERENCED_BY)) {
    tui_referenced_by(state);
    return true;
  }

  /* Ctrl+N or n - set cell to NULL */
  if (hotkey_matches(cfg, event, HOTKEY_SET_NULL)) {
    query_result_set_cell_direct(state, tab, true);
    return true;
  }

  /* Ctrl+D or d - set cell to empty string */
  if (hotkey_matches(cfg, event, HOTKEY_SET_EMPTY)) {
    query_result_set_cell_direct(state, tab, false);
    return true;
  }

  /* d - cut: copy the cell, then set it empty */
  if (hotkey_matches(cfg, event, HOTKEY_CELL_CUT)) {
    query_result_cell_copy(state, tab);
    query_result_set_cell_direct(state, tab, false);
    return true;
  }

  /* c or Ctrl+K - copy cell to clipboard */
  if (hotkey_matches(cfg, event, HOTKEY_CELL_COPY)) {
    query_result_cell_copy(state, tab);
    return true;
  }

  /* v or Ctrl+U - paste from clipboard to cell */
  if (hotkey_matches(cfg, event, HOTKEY_CELL_PASTE)) {
    query_result_cell_paste(state, tab);
    return true;
  }

  /* x or Delete - delete row(s) */
  if (hotkey_matches(cfg, event, HOTKEY_DELETE_ROW)) {
    query_result_delete_row(state, tab);
    return true;
  }

  /* Space - toggle row selection */
  if (hotkey_matches(cfg, event, HOTKEY_TOGGLE_SELECTION)) {
    /* Overflow check for global row calculation */
    if (tab->query_result_row > SIZE_MAX - tab->query_loaded_offset)
      return true; /* Silently ignore on overflow */
    size_t global_row = tab->query_loaded_offset + tab->query_result_row;
    tab_toggle_selection(tab, global_row);
    return true;
  }

  /* Escape - clear selections (only if there are selections) */
  if (hotkey_matches(cfg, event, HOTKEY_CLEAR_SELECTIONS)) {
    if (tab->num_selected > 0) {
      tab_clear_selections(tab);
      return true;
    }
    /* If no selections, let Escape switch focus to editor */
  }

  /* r/R or Ctrl+R - refresh query results */
  if (hotkey_matches(cfg, event, HOTKEY_REFRESH) ||
      hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY)) {
    /* Re-execute the base SQL if available, otherwise find query at cursor.
     * IMPORTANT: Must copy the SQL before calling query_execute because
     * query_execute frees tab->query_base_sql before re-using it. */
    char *sql_copy = NULL;
    if (tab->query_base_sql && *tab->query_base_sql) {
      sql_copy = str_dup(tab->query_base_sql);
    } else if (tab->query_text && *tab->query_text) {
      /* Fall back to finding query at cursor */
      sql_copy = query_find_at_cursor(tab->query_text, tab->query_cursor,
                           db_conn_driver_name(TUI_CONN(state)));
    }
    if (sql_copy && *sql_copy) {
      query_execute(state, sql_copy);
    }
    free(sql_copy);
    return true;
  }

  /* Up / k - move cursor up */
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_UP)) {
    if (tab->query_result_row > 0) {
      tab->query_result_row--;
      /* Adjust scroll */
      if (tab->query_result_row < tab->query_result_scroll_row) {
        tab->query_result_scroll_row = tab->query_result_row;
      }
      query_check_load_more(state, tab);
    } else {
      /* At top row - switch focus to editor */
      ui->query_focus_results = false;
    }
    return true;
  }

  /* Down / j - move cursor down */
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN)) {
    if (tab->query_results->num_rows > 0 &&
        tab->query_result_row < tab->query_results->num_rows - 1) {
      tab->query_result_row++;
      int visible = query_visible_result_rows(state);
      if (tab->query_result_row >=
          tab->query_result_scroll_row + (size_t)visible) {
        tab->query_result_scroll_row = tab->query_result_row - visible + 1;
      }
      query_check_load_more(state, tab);
    }
    return true;
  }

  /* Left / h - move cursor left */
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_LEFT)) {
    if (tab->query_result_col > 0) {
      tab->query_result_col--;
      /* Adjust horizontal scroll */
      if (tab->query_result_col < tab->query_result_scroll_col) {
        tab->query_result_scroll_col = tab->query_result_col;
      }
    } else if (state->sidebar_visible) {
      /* At left-most column - move focus to sidebar */
      state->sidebar_focused = true;
      /* Restore last sidebar position */
      state->sidebar_highlight = state->sidebar_last_position;
    }
    return true;
  }

  /* Right / l - move cursor right */
  if (hotkey_matches(cfg, event, HOTKEY_MOVE_RIGHT)) {
    if (tab->query_result_col < tab->query_results->num_columns - 1) {
      tab->query_result_col++;
      /* Adjust horizontal scroll to keep cursor visible using actual main
       * window */
      int right_rows, right_cols;
      getmaxyx(state->main_win, right_rows, right_cols);
      (void)right_rows;
      int avail_width = right_cols;
      int x = 1;
      size_t last_visible = tab->query_result_scroll_col;
      for (size_t col = tab->query_result_scroll_col;
           col < tab->query_results->num_columns; col++) {
        int w = tab->query_result_col_widths
                    ? tab->query_result_col_widths[col]
                    : 15;
        if (x + w + 3 > avail_width)
          break;
        x += w + 1;
        last_visible = col;
      }
      if (tab->query_result_col > last_visible) {
        tab->query_result_scroll_col = tab->query_result_col;
        /* Adjust to show as many columns as possible */
        x = 1;
        while (tab->query_result_scroll_col > 0) {
          int w =
              tab->query_result_col_widths
                  ? tab->query_result_col_widths[tab->query_result_scroll_col]
                  : 15;
          if (x + w + 3 > avail_width)
            break;
          x += w + 1;
          if (tab->query_result_scroll_col == tab->query_result_col)
            break;
          tab->query_result_scroll_col--;
        }
      }
    }
    return true;
  }

  /* Home */
  if (hotkey_matches(cfg, event, HOTKEY_FIRST_COL)) {
    tab->query_result_col = 0;
    tab->query_result_scroll_col = 0;
    return true;
  }

  /* End */
  if (hotkey_matches(cfg, event, HOTKEY_LAST_COL)) {
    if (tab->query_results->num_columns > 0) {
      tab->query_result_col = tab->query_results->num_columns - 1;
      /* Adjust horizontal scroll to show last column using actual main window
       */
      int end_rows, end_cols;
      getmaxyx(state->main_win, end_rows, end_cols);
      (void)end_rows;
      int avail_width = end_cols;
      tab->query_result_scroll_col = tab->query_result_col;
      /* Adjust to show as many columns as possible */
      int x = 1;
      while (tab->query_result_scroll_col > 0) {
        int w =
            tab->query_result_col_widths
                ? tab->query_result_col_widths[tab->query_result_scroll_col]
                : 15;
        if (x + w + 3 > avail_width)
          break;
        x += w + 1;
        if (tab->query_result_scroll_col == tab->query_result_col)
          break;
        tab->query_result_scroll_col--;
      }
    }
    return true;
  }

  /* Page Up */
  if (hotkey_matches(cfg, event, HOTKEY_PAGE_UP)) {
    /* Calculate visible rows in results area using actual main window */
    int visible = query_visible_result_rows(state);

    if (tab->query_result_row > (size_t)visible) {
      tab->query_result_row -= visible;
    } else {
      tab->query_result_row = 0;
    }
    /* Adjust scroll to keep cursor visible */
    if (tab->query_result_row < tab->query_result_scroll_row) {
      tab->query_result_scroll_row = tab->query_result_row;
    }
    query_check_load_more(state, tab);
    return true;
  }

  /* Page Down */
  if (hotkey_matches(cfg, event, HOTKEY_PAGE_DOWN)) {
    /* Calculate visible rows in results area using actual main window */
    int visible = query_visible_result_rows(state);

    tab->query_result_row += visible;
    if (tab->query_result_row >= tab->query_results->num_rows) {
      tab->query_result_row = tab->query_results->num_rows > 0
                                  ? tab->query_results->num_rows - 1
                                  : 0;
    }
    /* Adjust scroll to keep cursor visible */
    if (tab->query_result_row >=
        tab->query_result_scroll_row + (size_t)visible) {
      tab->query_result_scroll_row = tab->query_result_row - visible + 1;
    }
    query_check_load_more(state, tab);
    return true;
  }

  /* g or a - go to first row */
  if (hotkey_matches(cfg, event, HOTKEY_FIRST_ROW)) {
    tab->query_result_row = 0;
    tab->query_result_scroll_row = 0;
    query_check_load_more(state, tab);
    return true;
  }

  /* G or z - go to last row */
  if (hotkey_matches(cfg, event, HOTKEY_LAST_ROW)) {
    /* Jump to the real last row of a paginated result */
    if (tab->query_paginated && query_ensure_total(state, tab) &&
        tab->query_total_rows > 0 &&
        tab->query_loaded_offset + tab->query_loaded_count <
            tab->query_total_rows) {
      query_load_rows_at(state, tab, tab->query_total_rows);
    }
    if (tab->query_results && tab->query_results->num_rows > 0) {
      tab->query_result_row = tab->query_results->num_rows - 1;
      /* Scroll so the last row is on screen */
      int visible = query_visible_result_rows(state);
      tab->query_result_scroll_row =
          tab->query_result_row >= (size_t)visible
              ? tab->query_result_row - (size_t)visible + 1
              : 0;
    }
    query_check_load_more(state, tab);
    return true;
  }

  return false;
}

/* Keys while the editor has focus: cursor movement and text editing.
 * Returns true if handled. */
static bool query_editor_handle_key(TuiState *state, Tab *tab, UITabState *ui,
                                    const UiEvent *event, const Config *cfg) {
  int key_char = render_event_get_char(event);

  /* Up arrow - move cursor up one line */
  if (render_event_is_special(event, UI_KEY_UP)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);
    size_t dcol = query_cursor_display_col(tab);

    if (line > 0) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line - 1, dcol, lines, num_lines);
    }

    free(lines);
    return true;
  }

  /* Down arrow - move cursor down one line */
  if (render_event_is_special(event, UI_KEY_DOWN)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);
    size_t dcol = query_cursor_display_col(tab);

    if (line < num_lines - 1) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line + 1, dcol, lines, num_lines);
    } else if (tab->query_results && tab->query_results->num_rows > 0) {
      /* On last line - move focus to results panel */
      ui->query_focus_results = true;
      ui->query_unfocused = false;
    }

    free(lines);
    return true;
  }

  /* Left arrow */
  if (render_event_is_special(event, UI_KEY_LEFT)) {
    if (tab->query_cursor > 0) {
      tab->query_cursor = utf8_prev(tab->query_text, tab->query_cursor);
    } else if (state->sidebar_visible) {
      /* At top-left position - move focus to sidebar */
      state->sidebar_focused = true;
      /* Restore last sidebar position */
      state->sidebar_highlight = state->sidebar_last_position;
    }
    return true;
  }

  /* Right arrow */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    if (tab->query_cursor < tab->query_len) {
      tab->query_cursor =
          utf8_next(tab->query_text, tab->query_len, tab->query_cursor);
    } else if (tab->query_results && tab->query_results->num_rows > 0) {
      /* At end of text - move focus to results panel */
      ui->query_focus_results = true;
      ui->query_unfocused = false;
    }
    return true;
  }

  /* Home - move to start of line */
  if (render_event_is_special(event, UI_KEY_HOME)) {
    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    if (lines && line < num_lines) {
      tab->query_cursor = lines[line].start;
    }
    free(lines);
    return true;
  }

  /* End or Ctrl+E - move to end of line */
  if (render_event_is_special(event, UI_KEY_END) ||
      render_event_is_ctrl(event, 'E')) {
    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);

    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    if (lines && line < num_lines) {
      tab->query_cursor = lines[line].start + lines[line].len;
    }
    free(lines);
    return true;
  }

  /* Page Up */
  if (render_event_is_special(event, UI_KEY_PAGEUP)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);
    size_t dcol = query_cursor_display_col(tab);

    if (line > 10) {
      tab->query_cursor =
          query_line_col_to_cursor(tab, line - 10, dcol, lines, num_lines);
    } else {
      tab->query_cursor =
          query_line_col_to_cursor(tab, 0, dcol, lines, num_lines);
    }

    free(lines);
    return true;
  }

  /* Page Down */
  if (render_event_is_special(event, UI_KEY_PAGEDOWN)) {
    QueryLineInfo *lines = NULL;
    size_t num_lines = 0;
    query_rebuild_line_cache(tab, &lines, &num_lines);

    size_t line, col;
    query_cursor_to_line_col(tab, &line, &col);
    size_t dcol = query_cursor_display_col(tab);

    tab->query_cursor =
        query_line_col_to_cursor(tab, line + 10, dcol, lines, num_lines);

    free(lines);
    return true;
  }

  /* Backspace */
  if (render_event_is_special(event, UI_KEY_BACKSPACE)) {
    query_backspace(tab);
    return true;
  }

  /* Delete */
  if (render_event_is_special(event, UI_KEY_DELETE)) {
    query_delete_char(tab);
    return true;
  }

  /* Enter - insert newline */
  if (render_event_is_special(event, UI_KEY_ENTER)) {
    query_insert_char(tab, '\n');
    return true;
  }

  /* Ctrl+K - cut full line (including newline) to buffer.
   * Consecutive cuts append to buffer so multiple lines can be cut together. */
  if (hotkey_matches(cfg, event, HOTKEY_CUT_LINE)) {
    /* Find start of current line */
    size_t start = tab->query_cursor;
    while (start > 0 && tab->query_text[start - 1] != '\n') {
      start--;
    }

    /* Find end of current line (including newline if present) */
    size_t end = tab->query_cursor;
    while (end < tab->query_len && tab->query_text[end] != '\n') {
      end++;
    }
    /* Include the newline character if present */
    if (end < tab->query_len && tab->query_text[end] == '\n') {
      end++;
    }

    size_t count = end - start;
    if (count > 0) {
      /* Check if this is a consecutive cut (append) or new cut (replace).
       * Consecutive = cursor at start of line (where last cut left it).
       * Any other action (typing, navigation, paste) moves cursor. */
      static size_t last_cut_cursor = SIZE_MAX;
      bool is_consecutive =
          (last_cut_cursor == start) && (state->clipboard_buffer != NULL);

      if (is_consecutive) {
        /* Append to existing buffer */
        size_t old_len = strlen(state->clipboard_buffer);
        /* Check for overflow before allocation */
        if (old_len > SIZE_MAX - count - 1) {
          /* Overflow - skip append */
        } else {
          state->clipboard_buffer =
              safe_realloc(state->clipboard_buffer, old_len + count + 1);
          memcpy(state->clipboard_buffer + old_len, tab->query_text + start, count);
          state->clipboard_buffer[old_len + count] = '\0';
        }
      } else {
        /* New cut - replace buffer, add newline if line doesn't end with one */
        free(state->clipboard_buffer);
        bool needs_newline = (tab->query_text[end - 1] != '\n');
        /* Check for overflow before allocation */
        size_t alloc_size = count + (needs_newline ? 1 : 0) + 1;
        if (count > SIZE_MAX - 2) {
          state->clipboard_buffer = NULL; /* Overflow */
        } else {
          state->clipboard_buffer = safe_malloc(alloc_size);
          memcpy(state->clipboard_buffer, tab->query_text + start, count);
          if (needs_newline) {
            state->clipboard_buffer[count] = '\n';
            state->clipboard_buffer[count + 1] = '\0';
          } else {
            state->clipboard_buffer[count] = '\0';
          }
        }
      }

      /* Copy to OS clipboard (redirect stderr to avoid error messages) */
      if (state->clipboard_buffer) {
#ifdef __APPLE__
        FILE *p = popen("pbcopy 2>/dev/null", "w");
#else
        const char *cmd = getenv("WAYLAND_DISPLAY")
                              ? "wl-copy 2>/dev/null"
                              : "xclip -selection clipboard 2>/dev/null";
        FILE *p = popen(cmd, "w");
#endif
        if (p) {
          fwrite(state->clipboard_buffer, 1, strlen(state->clipboard_buffer),
                 p);
          pclose(p);
        }
      }

      /* Delete the text and move cursor to line start */
      memmove(tab->query_text + start, tab->query_text + end,
              tab->query_len - end + 1);
      tab->query_len -= count;
      tab->query_cursor = start;

      /* Track cursor position for consecutive cut detection. */
      last_cut_cursor = start;
    }
    return true;
  }

  /* Ctrl+U - paste from system clipboard first, then internal buffer if OS
   * inaccessible */
  if (hotkey_matches(cfg, event, HOTKEY_PASTE)) {
    char *paste_text = NULL;
    bool os_clipboard_accessible = false;

    /* Try to read from system clipboard first */
#ifdef __APPLE__
    FILE *p = popen("pbpaste", "r");
#else
    const char *cmd = getenv("WAYLAND_DISPLAY")
                          ? "wl-paste -n 2>/dev/null"
                          : "xclip -selection clipboard -o 2>/dev/null";
    FILE *p = popen(cmd, "r");
#endif
    if (p) {
      /* Read clipboard content */
      size_t capacity = 4096;
      size_t len = 0;
      paste_text = safe_malloc(capacity);
      int c;
      while ((c = fgetc(p)) != EOF) {
        if (len + 1 >= capacity) {
          /* Check for overflow before doubling */
          if (capacity > SIZE_MAX / 2)
            break;
          capacity *= 2;
          paste_text = safe_realloc(paste_text, capacity);
        }
        paste_text[len++] = (char)c;
      }
      paste_text[len] = '\0';
      int status = pclose(p);
      /* OS clipboard is accessible if command succeeded (status 0) */
      os_clipboard_accessible = (status == 0);
      if (!os_clipboard_accessible || (paste_text && paste_text[0] == '\0')) {
        free(paste_text);
        paste_text = NULL;
      }
    }

    /* Only fall back to internal buffer if OS clipboard is inaccessible */
    if (!os_clipboard_accessible && state->clipboard_buffer) {
      paste_text = str_dup(state->clipboard_buffer);
    }

    /* Perform the paste */
    if (paste_text && paste_text[0]) {
      size_t paste_len = strlen(paste_text);
      if (query_ensure_capacity(tab, tab->query_len + paste_len + 1)) {
        /* Make room for pasted text */
        memmove(tab->query_text + tab->query_cursor + paste_len,
                tab->query_text + tab->query_cursor,
                tab->query_len - tab->query_cursor + 1);
        /* Insert pasted text */
        memcpy(tab->query_text + tab->query_cursor, paste_text, paste_len);
        tab->query_len += paste_len;
        tab->query_cursor += paste_len;
      }
    }
    free(paste_text);
    return true;
  }

  /* Tab - insert a tab character (Shift+Tab leaves the editor) */
  if (render_event_is_special(event, UI_KEY_TAB)) {
    query_insert_char(tab, '\t');
    return true;
  }

  /* Printable character - insert */
  if (render_event_is_char(event) && utf8_is_text_key(key_char)) {
    query_insert_char(tab, (char)key_char);
    return true;
  }

  return false;
}

/* Handle query tab input */
bool tui_handle_query_input(TuiState *state, const UiEvent *event) {
  if (!state)
    return false;
  if (!event || event->type != UI_EVENT_KEY)
    return false;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return false;

  const Config *cfg = state->app ? state->app->config : NULL;

  /* Handle edit mode first if active */
  if (ui->query_result_editing) {
    return query_result_handle_edit_input(state, tab, event);
  }

  /* Unfocused (just arrived at this tab): only keys that pick a focus are
   * handled here; everything else goes to global hotkeys such as [ and ] */
  if (ui->query_unfocused) {
    if (hotkey_matches(cfg, event, HOTKEY_QUERY_FOCUS_EDITOR)) {
      ui->query_unfocused = false;
      ui->query_focus_results = false;
      return true;
    }
    if (hotkey_matches(cfg, event, HOTKEY_QUERY_SWITCH_FOCUS) ||
        (tab->query_results && hotkey_matches(cfg, event, HOTKEY_MOVE_DOWN))) {
      ui->query_unfocused = false;
      ui->query_focus_results = tab->query_results != NULL;
      return true;
    }
    if (hotkey_matches(cfg, event, HOTKEY_MOVE_LEFT) && state->sidebar_visible) {
      state->sidebar_focused = true;
      return true;
    }
    /* Running needs no focus: run from the editor's cursor like the editor
     * does; a successful run moves focus to the results */
    if (!hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY) &&
        !hotkey_matches(cfg, event, HOTKEY_EXECUTE_ALL) &&
        !hotkey_matches(cfg, event, HOTKEY_EXECUTE_TRANSACTION)) {
      return false;
    }
    ui->query_focus_results = false;
  } else if (hotkey_matches(cfg, event, HOTKEY_QUERY_UNFOCUS) &&
             !(ui->query_focus_results && tab->num_selected > 0)) {
    /* Esc leaves the editor or results (with selections it clears them) */
    ui->query_unfocused = true;
    return true;
  }

  /* Toggle focus between editor and results (unbound by default) */
  if (hotkey_matches(cfg, event, HOTKEY_QUERY_SWITCH_FOCUS)) {
    ui->query_focus_results = !ui->query_focus_results;
    return true;
  }

  /* Handle results navigation when focused on results */
  if (ui->query_focus_results) {
    return query_results_handle_key(state, tab, ui, event, cfg);
  }

  /* Running statements works from the editor (and unfocused, see above) */
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_QUERY)) {
    query_run_script(state, QUERY_RUN_AT_CURSOR);
    return true;
  }
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_ALL)) {
    query_run_script(state, QUERY_RUN_ALL);
    return true;
  }
  if (hotkey_matches(cfg, event, HOTKEY_EXECUTE_TRANSACTION)) {
    query_run_script(state, QUERY_RUN_TRANSACTION);
    return true;
  }

  return query_editor_handle_key(state, tab, ui, event, cfg);
}
