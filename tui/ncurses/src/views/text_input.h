/*
 * Lace
 * Single-line text input: the one implementation of line editing keys
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TEXT_INPUT_H
#define LACE_TEXT_INPUT_H

#include "../core/ui_types.h"
#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The text is UTF-8; cursor is a byte offset kept on a character boundary.
 * Either the input owns a growable buffer (text_input_init), or it edits a
 * caller's fixed array in place (text_input_bind): then the text never grows
 * past that array, and is cut on a character boundary.
 *
 * Keys (text_input_handle_key): Left/Right, Home/Ctrl+A, End/Ctrl+E,
 * Backspace, Delete/Ctrl+D, Ctrl+U (delete to start), Ctrl+K (delete to
 * end), and text. Callers check their own hotkeys first.
 */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;      /* Bytes available in buf, the terminating NUL included */
  size_t cursor;
  bool owned;      /* buf is ours (grows); else the caller's fixed array */
  bool secret;     /* Drawn as '*' (an owned buffer is always wiped) */
} TextInput;

/* Own a buffer holding initial (NULL = empty). */
void text_input_init(TextInput *in, const char *initial, bool secret);

/* Edit the caller's array buf[cap] in place (it must hold a string). */
void text_input_bind(TextInput *in, char *buf, size_t cap, bool secret);

/* Take ownership of a malloc'd string (cursor clamped to it). */
void text_input_adopt(TextInput *in, char *heap_text, size_t cursor);

/* Give the owned text back to the caller (caller frees); *cursor gets the
 * cursor. The input is left empty. */
char *text_input_release(TextInput *in, size_t *cursor);

/* Wipe and free an owned buffer; a bound one is left to the caller. */
void text_input_free(TextInput *in);

/* Replace the text; the cursor goes to the end. */
void text_input_set(TextInput *in, const char *text);

/* Insert text at the cursor (a paste). Line breaks become spaces; text
 * beyond a bound buffer is cut on a character boundary. */
void text_input_insert(TextInput *in, const char *text);

/* Handle an editing key. Returns true if it was one (consumed). */
bool text_input_handle_key(TextInput *in, const UiEvent *event);

/* Draw width columns at (y, x) with attr. Focused: scrolled so the cursor
 * shows, which is drawn in reverse video; returns its screen column (-1 if
 * not visible). Unfocused: the start of the text, no cursor; returns -1. */
int text_input_draw(WINDOW *win, const TextInput *in, int y, int x, int width,
                    attr_t attr, bool focused);

#endif /* LACE_TEXT_INPUT_H */
