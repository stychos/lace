/*
 * Lace
 * Modal cell editor view implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "editor_view.h"
#include "../config/config.h"
#include "../util/mem.h"
#include "../util/str.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Editor buffer - simple gap buffer implementation */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} EditorBuffer;

/* Line info for display */
typedef struct {
  size_t start; /* Offset in buffer */
  size_t len;   /* Length of line (excluding newline) */
} LineInfo;

/* Editor state */
typedef struct {
  EditorBuffer buf;

  /* Cursor position */
  size_t cursor;      /* Byte offset in buffer */
  size_t cursor_line; /* Line number (0-based) */
  size_t cursor_col;  /* Column (0-based) */

  /* View scroll */
  size_t scroll_line; /* First visible line */
  size_t scroll_col;  /* Horizontal scroll */

  /* Line cache */
  LineInfo *lines;
  size_t num_lines;
  size_t lines_cap;

  /* Window dimensions (content area) */
  int view_rows;
  int view_cols;

  bool readonly;
  bool modified;
} EditorState;

static void editor_buffer_init(EditorBuffer *buf) {
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

static void editor_buffer_free(EditorBuffer *buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

static void editor_buffer_set(EditorBuffer *buf, const char *content) {
  editor_buffer_free(buf);

  if (!content) {
    buf->cap = 256;
    buf->data = safe_malloc(buf->cap);
    buf->data[0] = '\0';
    buf->len = 0;
    return;
  }

  buf->len = strlen(content);
  buf->cap = buf->len + 256;
  buf->data = safe_malloc(buf->cap);
  memcpy(buf->data, content, buf->len + 1);
}

static void editor_buffer_insert(EditorBuffer *buf, size_t pos, char ch) {
  if (buf->len + 2 > buf->cap) {
    size_t new_cap = buf->cap * 2;
    if (new_cap == 0)
      new_cap = 256; /* Handle zero capacity case */
    buf->data = safe_realloc(buf->data, new_cap);
    buf->cap = new_cap;
  }

  if (pos > buf->len)
    pos = buf->len;
  memmove(buf->data + pos + 1, buf->data + pos, buf->len - pos + 1);
  buf->data[pos] = ch;
  buf->len++;
}

static void editor_buffer_delete(EditorBuffer *buf, size_t pos) {
  if (pos >= buf->len)
    return;
  memmove(buf->data + pos, buf->data + pos + 1, buf->len - pos);
  buf->len--;
}

/* Rebuild line cache */
static void editor_rebuild_lines(EditorState *state) {
  state->num_lines = 0;

  const char *data = state->buf.data;
  size_t len = state->buf.len;
  size_t line_start = 0;

  for (size_t i = 0; i <= len; i++) {
    if (i == len || data[i] == '\n') {
      /* Add line */
      if (state->num_lines >= state->lines_cap) {
        size_t new_cap = state->lines_cap == 0 ? 64 : state->lines_cap * 2;
        state->lines =
            safe_reallocarray(state->lines, new_cap, sizeof(LineInfo));
        state->lines_cap = new_cap;
      }

      state->lines[state->num_lines].start = line_start;
      state->lines[state->num_lines].len = i - line_start;
      state->num_lines++;

      line_start = i + 1;
    }
  }

  /* Ensure at least one line */
  if (state->num_lines == 0) {
    if (state->lines_cap == 0) {
      state->lines = safe_malloc(sizeof(LineInfo));
      state->lines_cap = 1;
    }
    state->lines[0].start = 0;
    state->lines[0].len = 0;
    state->num_lines = 1;
  }
}

/* Update cursor line/col from byte offset */
static void editor_update_cursor_pos(EditorState *state) {
  state->cursor_line = 0;
  state->cursor_col = 0;

  for (size_t i = 0; i < state->num_lines; i++) {
    size_t line_end = state->lines[i].start + state->lines[i].len;
    if (state->cursor <= line_end) {
      state->cursor_line = i;
      state->cursor_col = state->cursor - state->lines[i].start;
      break;
    }
    if (i == state->num_lines - 1) {
      state->cursor_line = i;
      state->cursor_col = state->cursor - state->lines[i].start;
    }
  }
}

/* Update byte offset from cursor line/col */
static void editor_update_cursor_offset(EditorState *state) {
  if (state->cursor_line >= state->num_lines) {
    state->cursor_line = state->num_lines > 0 ? state->num_lines - 1 : 0;
  }

  if (state->num_lines == 0) {
    state->cursor = 0;
    state->cursor_col = 0;
    return;
  }

  LineInfo *line = &state->lines[state->cursor_line];
  if (state->cursor_col > line->len) {
    state->cursor_col = line->len;
  }
  state->cursor = line->start + state->cursor_col;
}

/* Ensure cursor is visible */
static void editor_ensure_visible(EditorState *state) {
  /* Vertical scroll */
  if (state->cursor_line < state->scroll_line) {
    state->scroll_line = state->cursor_line;
  }
  if (state->cursor_line >= state->scroll_line + (size_t)state->view_rows) {
    state->scroll_line = state->cursor_line - state->view_rows + 1;
  }

  /* Horizontal scroll */
  if (state->cursor_col < state->scroll_col) {
    state->scroll_col = state->cursor_col;
  }
  if (state->cursor_col >= state->scroll_col + (size_t)state->view_cols) {
    state->scroll_col = state->cursor_col - state->view_cols + 1;
  }
}

static void editor_move_left(EditorState *state) {
  if (state->cursor > 0) {
    state->cursor--;
    editor_update_cursor_pos(state);
    editor_ensure_visible(state);
  }
}

static void editor_move_right(EditorState *state) {
  if (state->cursor < state->buf.len) {
    state->cursor++;
    editor_update_cursor_pos(state);
    editor_ensure_visible(state);
  }
}

static void editor_move_up(EditorState *state) {
  if (state->cursor_line > 0) {
    state->cursor_line--;
    editor_update_cursor_offset(state);
    editor_ensure_visible(state);
  }
}

static void editor_move_down(EditorState *state) {
  if (state->cursor_line < state->num_lines - 1) {
    state->cursor_line++;
    editor_update_cursor_offset(state);
    editor_ensure_visible(state);
  }
}

static void editor_move_home(EditorState *state) {
  if (state->cursor_line < state->num_lines) {
    state->cursor = state->lines[state->cursor_line].start;
    state->cursor_col = 0;
    editor_ensure_visible(state);
  }
}

static void editor_move_end(EditorState *state) {
  if (state->cursor_line < state->num_lines) {
    LineInfo *line = &state->lines[state->cursor_line];
    state->cursor = line->start + line->len;
    state->cursor_col = line->len;
    editor_ensure_visible(state);
  }
}

static void editor_page_up(EditorState *state) {
  if (state->cursor_line > (size_t)state->view_rows) {
    state->cursor_line -= state->view_rows;
  } else {
    state->cursor_line = 0;
  }
  editor_update_cursor_offset(state);
  editor_ensure_visible(state);
}

static void editor_page_down(EditorState *state) {
  if (state->num_lines == 0) {
    state->cursor_line = 0;
    return;
  }
  state->cursor_line += state->view_rows;
  if (state->cursor_line >= state->num_lines) {
    state->cursor_line = state->num_lines - 1;
  }
  editor_update_cursor_offset(state);
  editor_ensure_visible(state);
}

static void editor_insert_char(EditorState *state, char ch) {
  if (state->readonly)
    return;

  editor_buffer_insert(&state->buf, state->cursor, ch);
  state->cursor++;
  state->modified = true;
  editor_rebuild_lines(state);
  editor_update_cursor_pos(state);
  editor_ensure_visible(state);
}

static void editor_delete_char(EditorState *state) {
  if (state->readonly)
    return;
  if (state->cursor >= state->buf.len)
    return;

  editor_buffer_delete(&state->buf, state->cursor);
  state->modified = true;
  editor_rebuild_lines(state);
  editor_update_cursor_pos(state);
}

static void editor_backspace(EditorState *state) {
  if (state->readonly)
    return;
  if (state->cursor == 0)
    return;

  state->cursor--;
  editor_buffer_delete(&state->buf, state->cursor);
  state->modified = true;
  editor_rebuild_lines(state);
  editor_update_cursor_pos(state);
  editor_ensure_visible(state);
}

static void draw_editor(WINDOW *win, EditorState *state, const char *title,
                        int height, int width, const Config *config) {
  werase(win);
  DRAW_BOX(win, COLOR_BORDER);

  /* Title */
  int title_len = (int)strlen(title);
  if (title_len > width - 4)
    title_len = width - 4;
  WITH_ATTR(win, A_BOLD, {
    mvwaddnstr(win, 0, (width - title_len - 2) / 2, " ", 1);
    waddnstr(win, title, title_len);
    waddnstr(win, " ", 1);
  });

  /* Modified indicator */
  if (state->modified) {
    mvwaddstr(win, 0, width - 12, " [modified] ");
  }

  /* Calculate line number width based on total lines */
  int lnum_width = 3;
  if (state->num_lines >= 1000)
    lnum_width = 4;
  if (state->num_lines >= 10000)
    lnum_width = 5;

  /* Content area */
  int content_y = 1;
  int content_x = 1 + lnum_width + 1; /* line numbers + separator space */
  int content_h = height - 4;         /* Leave room for status bar */
  int content_w = width - 2 - lnum_width - 1;

  state->view_rows = content_h;
  state->view_cols = content_w;

  /* Draw lines */
  for (int row = 0; row < content_h; row++) {
    size_t line_idx = state->scroll_line + row;

    /* Draw line number */
    if (line_idx < state->num_lines) {
      wattron(win, A_DIM);
      mvwprintw(win, content_y + row, 1, "%*zu", lnum_width, line_idx + 1);
      wattroff(win, A_DIM);
    } else {
      /* Clear line number area for empty lines */
      mvwhline(win, content_y + row, 1, ' ', lnum_width);
    }

    wmove(win, content_y + row, content_x);

    if (line_idx < state->num_lines) {
      LineInfo *line = &state->lines[line_idx];
      const char *line_data = state->buf.data + line->start;
      size_t line_len = line->len;

      /* Apply horizontal scroll */
      if (state->scroll_col < line_len) {
        size_t visible_start = state->scroll_col;
        size_t visible_len = line_len - visible_start;
        if (visible_len > (size_t)content_w)
          visible_len = content_w;

        waddnstr(win, line_data + visible_start, visible_len);
      }
    }

    /* Clear rest of line */
    wclrtoeol(win);
  }

  /* Redraw right border */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  for (int row = 1; row < height - 1; row++) {
    mvwaddch(win, row, width - 1, ACS_VLINE);
  }

  /* Status bar separator with T-junctions */
  int status_y = height - 2;
  mvwaddch(win, status_y - 1, 0, ACS_LTEE);
  mvwhline(win, status_y - 1, 1, ACS_HLINE, width - 2);
  mvwaddch(win, status_y - 1, width - 1, ACS_RTEE);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Status text */
  if (state->readonly) {
    mvwprintw(win, status_y, 2, "[Read-only] Line %zu/%zu  Col %zu",
              state->cursor_line + 1, state->num_lines, state->cursor_col + 1);
    char *cancel_key = hotkey_get_display(config, HOTKEY_EDITOR_CANCEL);
    char close_hint[32];
    snprintf(close_hint, sizeof(close_hint), "[%s] Close",
             cancel_key ? cancel_key : "Esc");
    free(cancel_key);
    mvwprintw(win, status_y, width - (int)strlen(close_hint) - 2, "%s",
              close_hint);
  } else {
    mvwprintw(win, status_y, 2, "L%zu/%zu C%zu", state->cursor_line + 1,
              state->num_lines, state->cursor_col + 1);
    /* Build status hint from configurable keys */
    char *save_key = hotkey_get_display(config, HOTKEY_EDITOR_SAVE);
    char *null_key = hotkey_get_display(config, HOTKEY_EDITOR_NULL);
    char *empty_key = hotkey_get_display(config, HOTKEY_EDITOR_EMPTY);
    char *cancel_key = hotkey_get_display(config, HOTKEY_EDITOR_CANCEL);
    char status_hint[128];
    snprintf(status_hint, sizeof(status_hint),
             "[%s] Save [%s] NULL [%s] Empty [%s] Cancel",
             save_key ? save_key : "F2", null_key ? null_key : "^N",
             empty_key ? empty_key : "^D", cancel_key ? cancel_key : "Esc");
    free(save_key);
    free(null_key);
    free(empty_key);
    free(cancel_key);
    int hint_len = (int)strlen(status_hint);
    mvwprintw(win, status_y, width - hint_len - 2, "%s", status_hint);
  }

  /* Position cursor - ensure no underflow from size_t subtraction */
  int cursor_y = content_y;
  int cursor_x = content_x; /* Already includes line number offset */
  if (state->cursor_line >= state->scroll_line) {
    cursor_y += (int)(state->cursor_line - state->scroll_line);
  }
  if (state->cursor_col >= state->scroll_col) {
    cursor_x += (int)(state->cursor_col - state->scroll_col);
  }
  wmove(win, cursor_y, cursor_x);

  wrefresh(win);
}

EditorResult editor_view_show(TuiState *state, const char *title,
                              const char *content, bool readonly) {
  EditorResult result = {false, false, NULL};

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  /* Size: 80% of terminal, min 40x15, max 120x40 */
  int height = term_rows * 80 / 100;
  int width = term_cols * 80 / 100;

  if (height < 15)
    height = 15;
  if (width < 50)
    width = 50;
  if (height > 40)
    height = 40;
  if (width > 120)
    width = 120;
  if (height > term_rows - 2)
    height = term_rows - 2;
  if (width > term_cols - 2)
    width = term_cols - 2;

  int starty, startx;
  dialog_center_position(&starty, &startx, height, width, term_rows, term_cols);

  WINDOW *win = newwin(height, width, starty, startx);
  if (!win)
    return result;

  keypad(win, TRUE);
  curs_set(1);

  /* Enable mouse for this window */
  mousemask(BUTTON1_CLICKED, NULL);

  /* Initialize editor state */
  EditorState editor = {0};
  editor_buffer_init(&editor.buf);
  editor.readonly = readonly;

  editor_buffer_set(&editor.buf, content);

  editor_rebuild_lines(&editor);
  editor_update_cursor_pos(&editor);

  const Config *config = state->app ? state->app->config : NULL;

  bool running = true;
  while (running) {
    draw_editor(win, &editor, title, height, width, config);

    int ch = wgetch(win);

    /* Handle mouse separately (not a key event) */
    if (ch == KEY_MOUSE) {
      MEVENT mouse_event;
      if (getmouse(&mouse_event) == OK) {
        /* Convert screen coords to window coords */
        int mouse_y = mouse_event.y - starty;
        int mouse_x = mouse_event.x - startx;
        int status_y = height - 2;

        /* Check if click is on status bar */
        if (mouse_y == status_y && (mouse_event.bstate & BUTTON1_CLICKED)) {
          if (readonly) {
            /* "[Esc] Close" at width - 13 */
            if (mouse_x >= width - 13 && mouse_x < width - 2) {
              running = false; /* Same as Escape */
            }
          } else {
            /* "[F2] Save [^N] NULL [^D] Empty [Esc] Cancel" at width - 45 */
            if (mouse_x >= width - 45 && mouse_x < width - 35) {
              /* Clicked [F2] Save */
              result.saved = true;
              result.content = str_dup(editor.buf.data);
              running = false;
            } else if (mouse_x >= width - 35 && mouse_x < width - 25) {
              /* Clicked [^N] NULL */
              result.saved = true;
              result.set_null = true;
              result.content = NULL;
              running = false;
            } else if (mouse_x >= width - 25 && mouse_x < width - 14) {
              /* Clicked [^D] Empty */
              result.saved = true;
              result.content = str_dup("");
              running = false;
            } else if (mouse_x >= width - 14 && mouse_x < width - 2) {
              /* Clicked [Esc] Cancel */
              running = false;
            }
          }
        }
      }
      continue;
    }

    /* Translate key to UiEvent */
    UiEvent event;
    render_translate_key(ch, &event);
    int key_char = render_event_get_char(&event);

    /* Cancel - configurable (default: Escape) */
    if (hotkey_matches(config, &event, HOTKEY_EDITOR_CANCEL)) {
      running = false;
    }
    /* Save - configurable (default: F2) */
    else if (hotkey_matches(config, &event, HOTKEY_EDITOR_SAVE) && !readonly) {
      result.saved = true;
      result.content = str_dup(editor.buf.data);
      running = false;
    }
    /* Left arrow */
    else if (render_event_is_special(&event, UI_KEY_LEFT)) {
      editor_move_left(&editor);
    }
    /* Right arrow */
    else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
      editor_move_right(&editor);
    }
    /* Up arrow */
    else if (render_event_is_special(&event, UI_KEY_UP)) {
      editor_move_up(&editor);
    }
    /* Down arrow */
    else if (render_event_is_special(&event, UI_KEY_DOWN)) {
      editor_move_down(&editor);
    }
    /* Home or Ctrl+A */
    else if (render_event_is_special(&event, UI_KEY_HOME) ||
             render_event_is_ctrl(&event, 'A')) {
      editor_move_home(&editor);
    }
    /* End or Ctrl+E */
    else if (render_event_is_special(&event, UI_KEY_END) ||
             render_event_is_ctrl(&event, 'E')) {
      editor_move_end(&editor);
    }
    /* Page Up */
    else if (render_event_is_special(&event, UI_KEY_PAGEUP)) {
      editor_page_up(&editor);
    }
    /* Page Down */
    else if (render_event_is_special(&event, UI_KEY_PAGEDOWN)) {
      editor_page_down(&editor);
    }
    /* Backspace */
    else if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
      editor_backspace(&editor);
    }
    /* Delete */
    else if (render_event_is_special(&event, UI_KEY_DELETE)) {
      editor_delete_char(&editor);
    }
    /* Set NULL - configurable (default: Ctrl+N) */
    else if (hotkey_matches(config, &event, HOTKEY_EDITOR_NULL) && !readonly) {
      result.saved = true;
      result.set_null = true;
      result.content = NULL;
      running = false;
    }
    /* Set empty - configurable (default: Ctrl+D) */
    else if (hotkey_matches(config, &event, HOTKEY_EDITOR_EMPTY) && !readonly) {
      result.saved = true;
      result.content = str_dup("");
      running = false;
    }
    /* Cut line - configurable (default: Ctrl+K) */
    else if (hotkey_matches(config, &event, HOTKEY_CUT_LINE) && !readonly) {
      if (editor.cursor_line < editor.num_lines) {
        LineInfo *line = &editor.lines[editor.cursor_line];
        size_t start = line->start;
        size_t end = start + line->len;
        /* Include newline if present */
        if (end < editor.buf.len && editor.buf.data[end] == '\n') {
          end++;
        }
        size_t count = end - start;

        if (count > 0 && state) {
          /* Check for consecutive cut */
          static size_t last_cut_line = SIZE_MAX;
          bool is_consecutive = (last_cut_line == editor.cursor_line) &&
                                (state->clipboard_buffer != NULL);

          if (is_consecutive) {
            /* Append to buffer */
            size_t old_len = strlen(state->clipboard_buffer);
            state->clipboard_buffer =
                safe_realloc(state->clipboard_buffer, old_len + count + 1);
            memcpy(state->clipboard_buffer + old_len, editor.buf.data + start,
                   count);
            state->clipboard_buffer[old_len + count] = '\0';
          } else {
            /* Replace buffer, add newline if needed */
            free(state->clipboard_buffer);
            bool needs_newline = (editor.buf.data[end - 1] != '\n');
            state->clipboard_buffer =
                safe_malloc(count + (needs_newline ? 1 : 0) + 1);
            memcpy(state->clipboard_buffer, editor.buf.data + start, count);
            if (needs_newline) {
              state->clipboard_buffer[count] = '\n';
              state->clipboard_buffer[count + 1] = '\0';
            } else {
              state->clipboard_buffer[count] = '\0';
            }
          }

          /* Copy to OS clipboard */
          if (state->clipboard_buffer) {
#ifdef __APPLE__
            FILE *p = popen("pbcopy", "w");
#else
            const char *cmd = getenv("WAYLAND_DISPLAY")
                                  ? "wl-copy"
                                  : "xclip -selection clipboard";
            FILE *p = popen(cmd, "w");
#endif
            if (p) {
              fwrite(state->clipboard_buffer, 1,
                     strlen(state->clipboard_buffer), p);
              pclose(p);
            }
          }

          /* Delete the line */
          memmove(editor.buf.data + start, editor.buf.data + end,
                  editor.buf.len - end + 1);
          editor.buf.len -= count;
          editor.cursor = start;
          editor.modified = true;
          editor_rebuild_lines(&editor);
          editor_update_cursor_pos(&editor);
          editor_ensure_visible(&editor);

          last_cut_line = editor.cursor_line;
        }
      }
    }
    /* Paste - configurable (default: Ctrl+U) */
    else if (hotkey_matches(config, &event, HOTKEY_PASTE) && !readonly) {
      char *paste_text = NULL;
      bool os_clipboard_accessible = false;

      /* Try system clipboard first */
#ifdef __APPLE__
      FILE *p = popen("pbpaste", "r");
#else
      const char *cmd = getenv("WAYLAND_DISPLAY")
                            ? "wl-paste -n 2>/dev/null"
                            : "xclip -selection clipboard -o 2>/dev/null";
      FILE *p = popen(cmd, "r");
#endif
      if (p) {
        size_t capacity = 4096;
        size_t len = 0;
        paste_text = safe_malloc(capacity);
        int c;
        while ((c = fgetc(p)) != EOF) {
          if (len + 1 >= capacity) {
            capacity *= 2;
            paste_text = safe_realloc(paste_text, capacity);
          }
          paste_text[len++] = (char)c;
        }
        paste_text[len] = '\0';
        int status = pclose(p);
        /* OS clipboard is accessible if command succeeded (status 0) */
        os_clipboard_accessible = (status == 0);
        if (!os_clipboard_accessible || paste_text[0] == '\0') {
          free(paste_text);
          paste_text = NULL;
        }
      }

      /* Only fall back to internal buffer if OS clipboard is inaccessible */
      if (!os_clipboard_accessible && state && state->clipboard_buffer) {
        paste_text = strdup(state->clipboard_buffer);
      }

      /* Perform paste */
      if (paste_text && paste_text[0]) {
        size_t paste_len = strlen(paste_text);
        /* Ensure capacity */
        size_t needed = editor.buf.len + paste_len + 1;
        if (needed > editor.buf.cap) {
          size_t new_cap = editor.buf.cap * 2;
          if (new_cap < needed)
            new_cap = needed + 256;
          editor.buf.data = safe_realloc(editor.buf.data, new_cap);
          editor.buf.cap = new_cap;
        }
        memmove(editor.buf.data + editor.cursor + paste_len,
                editor.buf.data + editor.cursor,
                editor.buf.len - editor.cursor + 1);
        memcpy(editor.buf.data + editor.cursor, paste_text, paste_len);
        editor.buf.len += paste_len;
        editor.cursor += paste_len;
        editor.modified = true;
        editor_rebuild_lines(&editor);
        editor_update_cursor_pos(&editor);
        editor_ensure_visible(&editor);
      }
      free(paste_text);
    }
    /* Enter - insert newline */
    else if (render_event_is_special(&event, UI_KEY_ENTER) && !readonly) {
      editor_insert_char(&editor, '\n');
    }
    /* Tab - insert spaces */
    else if (render_event_is_special(&event, UI_KEY_TAB) && !readonly) {
      for (int i = 0; i < 4; i++) {
        editor_insert_char(&editor, ' ');
      }
    }
    /* Printable character - insert */
    else if (render_event_is_char(&event) && key_char >= 32 && key_char < 127 &&
             !readonly) {
      editor_insert_char(&editor, (char)key_char);
    }
  }

  curs_set(0);
  delwin(win);

  /* Restore mouse mask for main TUI */
  mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED, NULL);

  /* Cleanup */
  editor_buffer_free(&editor.buf);
  free(editor.lines);

  /* Redraw main screen */
  touchwin(stdscr);
  if (state) {
    tui_refresh(state);
  }

  return result;
}
