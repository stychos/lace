/*
 * lace - Database Viewer and Manager
 * Modal cell editor view implementation
 */

#include "editor_view.h"
#include "../../util/str.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Editor buffer - simple gap buffer implementation */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} EditorBuffer;

/* Line info for display */
typedef struct {
    size_t start;  /* Offset in buffer */
    size_t len;    /* Length of line (excluding newline) */
} LineInfo;

/* Editor state */
typedef struct {
    EditorBuffer buf;

    /* Cursor position */
    size_t cursor;       /* Byte offset in buffer */
    size_t cursor_line;  /* Line number (0-based) */
    size_t cursor_col;   /* Column (0-based) */

    /* View scroll */
    size_t scroll_line;  /* First visible line */
    size_t scroll_col;   /* Horizontal scroll */

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

static bool editor_buffer_set(EditorBuffer *buf, const char *content) {
    editor_buffer_free(buf);

    if (!content) {
        buf->cap = 256;
        buf->data = malloc(buf->cap);
        if (!buf->data) return false;
        buf->data[0] = '\0';
        buf->len = 0;
        return true;
    }

    buf->len = strlen(content);
    buf->cap = buf->len + 256;
    buf->data = malloc(buf->cap);
    if (!buf->data) return false;
    memcpy(buf->data, content, buf->len + 1);
    return true;
}

static bool editor_buffer_insert(EditorBuffer *buf, size_t pos, char ch) {
    if (buf->len + 2 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return false;
        buf->data = new_data;
        buf->cap = new_cap;
    }

    if (pos > buf->len) pos = buf->len;
    memmove(buf->data + pos + 1, buf->data + pos, buf->len - pos + 1);
    buf->data[pos] = ch;
    buf->len++;
    return true;
}

static void editor_buffer_delete(EditorBuffer *buf, size_t pos) {
    if (pos >= buf->len) return;
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
                LineInfo *new_lines = realloc(state->lines, new_cap * sizeof(LineInfo));
                if (!new_lines) return;
                state->lines = new_lines;
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
            state->lines = malloc(sizeof(LineInfo));
            if (state->lines) state->lines_cap = 1;
        }
        if (state->lines) {
            state->lines[0].start = 0;
            state->lines[0].len = 0;
            state->num_lines = 1;
        }
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
    state->cursor_line += state->view_rows;
    if (state->cursor_line >= state->num_lines) {
        state->cursor_line = state->num_lines - 1;
    }
    editor_update_cursor_offset(state);
    editor_ensure_visible(state);
}

static void editor_insert_char(EditorState *state, char ch) {
    if (state->readonly) return;

    if (editor_buffer_insert(&state->buf, state->cursor, ch)) {
        state->cursor++;
        state->modified = true;
        editor_rebuild_lines(state);
        editor_update_cursor_pos(state);
        editor_ensure_visible(state);
    }
}

static void editor_delete_char(EditorState *state) {
    if (state->readonly) return;
    if (state->cursor >= state->buf.len) return;

    editor_buffer_delete(&state->buf, state->cursor);
    state->modified = true;
    editor_rebuild_lines(state);
    editor_update_cursor_pos(state);
}

static void editor_backspace(EditorState *state) {
    if (state->readonly) return;
    if (state->cursor == 0) return;

    state->cursor--;
    editor_buffer_delete(&state->buf, state->cursor);
    state->modified = true;
    editor_rebuild_lines(state);
    editor_update_cursor_pos(state);
    editor_ensure_visible(state);
}

static void draw_editor(WINDOW *win, EditorState *state, const char *title,
                        int height, int width) {
    werase(win);
    box(win, 0, 0);

    /* Title */
    wattron(win, A_BOLD);
    int title_len = strlen(title);
    if (title_len > width - 4) title_len = width - 4;
    mvwaddnstr(win, 0, (width - title_len - 2) / 2, " ", 1);
    waddnstr(win, title, title_len);
    waddnstr(win, " ", 1);
    wattroff(win, A_BOLD);

    /* Modified indicator */
    if (state->modified) {
        mvwaddstr(win, 0, width - 12, " [modified] ");
    }

    /* Content area */
    int content_y = 1;
    int content_x = 1;
    int content_h = height - 4;  /* Leave room for status bar */
    int content_w = width - 2;

    state->view_rows = content_h;
    state->view_cols = content_w;

    /* Draw lines */
    for (int row = 0; row < content_h; row++) {
        size_t line_idx = state->scroll_line + row;

        wmove(win, content_y + row, content_x);

        if (line_idx < state->num_lines) {
            LineInfo *line = &state->lines[line_idx];
            const char *line_data = state->buf.data + line->start;
            size_t line_len = line->len;

            /* Apply horizontal scroll */
            if (state->scroll_col < line_len) {
                size_t visible_start = state->scroll_col;
                size_t visible_len = line_len - visible_start;
                if (visible_len > (size_t)content_w) visible_len = content_w;

                waddnstr(win, line_data + visible_start, visible_len);
            }
        }

        /* Clear rest of line */
        wclrtoeol(win);
    }

    /* Redraw right border */
    for (int row = 1; row < height - 1; row++) {
        mvwaddch(win, row, width - 1, ACS_VLINE);
    }

    /* Status bar */
    int status_y = height - 2;
    mvwhline(win, status_y - 1, 1, ACS_HLINE, width - 2);

    /* Status text */
    if (state->readonly) {
        mvwprintw(win, status_y, 2, "[Read-only] Line %zu/%zu  Col %zu",
                  state->cursor_line + 1, state->num_lines, state->cursor_col + 1);
        mvwprintw(win, status_y, width - 13, "[Esc] Close");
    } else {
        mvwprintw(win, status_y, 2, "L%zu/%zu C%zu",
                  state->cursor_line + 1, state->num_lines, state->cursor_col + 1);
        /* "[F2] Save [^N] NULL [^D] Empty [Esc]" = 36 chars + 1 for padding */
        mvwprintw(win, status_y, width - 37, "[F2] Save [^N] NULL [^D] Empty [Esc]");
    }

    /* Position cursor */
    int cursor_y = content_y + (state->cursor_line - state->scroll_line);
    int cursor_x = content_x + (state->cursor_col - state->scroll_col);
    wmove(win, cursor_y, cursor_x);

    wrefresh(win);
}

EditorResult editor_view_show(TuiState *state, const char *title,
                              const char *content, bool readonly) {
    EditorResult result = { false, false, NULL };

    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);

    /* Size: 80% of terminal, min 40x15, max 120x40 */
    int height = term_rows * 80 / 100;
    int width = term_cols * 80 / 100;

    if (height < 15) height = 15;
    if (width < 40) width = 40;
    if (height > 40) height = 40;
    if (width > 120) width = 120;
    if (height > term_rows - 2) height = term_rows - 2;
    if (width > term_cols - 2) width = term_cols - 2;

    int starty = (term_rows - height) / 2;
    int startx = (term_cols - width) / 2;

    WINDOW *win = newwin(height, width, starty, startx);
    if (!win) return result;

    keypad(win, TRUE);
    curs_set(1);

    /* Enable mouse for this window */
    mousemask(BUTTON1_CLICKED, NULL);

    /* Initialize editor state */
    EditorState editor = {0};
    editor_buffer_init(&editor.buf);
    editor.readonly = readonly;

    if (!editor_buffer_set(&editor.buf, content)) {
        delwin(win);
        return result;
    }

    editor_rebuild_lines(&editor);
    editor_update_cursor_pos(&editor);

    bool running = true;
    while (running) {
        draw_editor(win, &editor, title, height, width);

        int ch = wgetch(win);

        switch (ch) {
            case KEY_MOUSE: {
                MEVENT event;
                if (getmouse(&event) == OK) {
                    /* Convert screen coords to window coords */
                    int mouse_y = event.y - starty;
                    int mouse_x = event.x - startx;
                    int status_y = height - 2;

                    /* Check if click is on status bar */
                    if (mouse_y == status_y && (event.bstate & BUTTON1_CLICKED)) {
                        if (readonly) {
                            /* "[Esc] Close" at width - 13 */
                            if (mouse_x >= width - 13 && mouse_x < width - 2) {
                                running = false;  /* Same as Escape */
                            }
                        } else {
                            /* "[F2] Save [^N] NULL [^D] Empty [Esc]" at width - 37 */
                            /* [F2] Save: w-37 to w-28, [^N] NULL: w-27 to w-18, [^D] Empty: w-17 to w-7, [Esc]: w-6 to w-2 */
                            if (mouse_x >= width - 37 && mouse_x < width - 28) {
                                /* Clicked [F2] Save */
                                result.saved = true;
                                result.content = str_dup(editor.buf.data);
                                running = false;
                            } else if (mouse_x >= width - 27 && mouse_x < width - 18) {
                                /* Clicked [^N] NULL */
                                result.saved = true;
                                result.set_null = true;
                                result.content = NULL;
                                running = false;
                            } else if (mouse_x >= width - 17 && mouse_x < width - 7) {
                                /* Clicked [^D] Empty */
                                result.saved = true;
                                result.content = str_dup("");
                                running = false;
                            } else if (mouse_x >= width - 6 && mouse_x < width - 2) {
                                /* Clicked [Esc] */
                                running = false;
                            }
                        }
                    }
                }
                break;
            }

            case 27:  /* Escape */
                running = false;
                break;

            case KEY_F(2):   /* F2 - save */
                if (!readonly) {
                    result.saved = true;
                    result.content = str_dup(editor.buf.data);
                    running = false;
                }
                break;

            case KEY_LEFT:
                editor_move_left(&editor);
                break;

            case KEY_RIGHT:
                editor_move_right(&editor);
                break;

            case KEY_UP:
                editor_move_up(&editor);
                break;

            case KEY_DOWN:
                editor_move_down(&editor);
                break;

            case KEY_HOME:
            case 1:  /* Ctrl+A */
                editor_move_home(&editor);
                break;

            case KEY_END:
            case 5:  /* Ctrl+E */
                editor_move_end(&editor);
                break;

            case KEY_PPAGE:
                editor_page_up(&editor);
                break;

            case KEY_NPAGE:
                editor_page_down(&editor);
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                editor_backspace(&editor);
                break;

            case KEY_DC:
                editor_delete_char(&editor);
                break;

            case 14:  /* Ctrl+N - set to NULL */
                if (!readonly) {
                    result.saved = true;
                    result.set_null = true;
                    result.content = NULL;
                    running = false;
                }
                break;

            case 4:  /* Ctrl+D - set to empty string */
                if (!readonly) {
                    result.saved = true;
                    result.content = str_dup("");
                    running = false;
                }
                break;

            case '\n':
            case KEY_ENTER:
                if (!readonly) {
                    editor_insert_char(&editor, '\n');
                }
                break;

            case '\t':
                if (!readonly) {
                    /* Insert spaces for tab */
                    for (int i = 0; i < 4; i++) {
                        editor_insert_char(&editor, ' ');
                    }
                }
                break;

            default:
                if (ch >= 32 && ch < 127 && !readonly) {
                    editor_insert_char(&editor, (char)ch);
                }
                break;
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
