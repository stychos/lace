/*
 * Lace
 * Clipboard: the system clipboard (pbcopy, wl-copy, xclip) with an internal
 * buffer as fallback
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "config/config.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "views/editor_view.h"
#include "views/text_input.h"
#include "row_key.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Copy text to clipboard using pbcopy on macOS, wl-copy/xclip on Linux.
 * Always saves to internal buffer as fallback for pasting within the app. */
bool tui_clipboard_copy(TuiState *state, const char *text) {
  if (!text)
    return false;

  /* Always save to internal buffer for in-app pasting */
  if (state) {
    free(state->clipboard_buffer);
    state->clipboard_buffer = str_dup(text);
  }

  /* Try external clipboard (redirect stderr to avoid error messages) */
#ifdef __APPLE__
  FILE *p = popen("pbcopy 2>/dev/null", "w");
#else
  /* Use wl-copy on Wayland, xclip on X11 */
  const char *cmd = getenv("WAYLAND_DISPLAY")
                        ? "wl-copy 2>/dev/null"
                        : "xclip -selection clipboard 2>/dev/null";
  FILE *p = popen(cmd, "w");
#endif
  if (!p) {
    /* External clipboard failed, but internal buffer is set */
    return state && state->clipboard_buffer;
  }

  size_t len = strlen(text);
  size_t written = fwrite(text, 1, len, p);
  int status = pclose(p);

  /* Success if external worked OR we have internal buffer */
  return (written == len && status == 0) || (state && state->clipboard_buffer);
}

/* Read text from clipboard. Returns malloc'd string or NULL. */
char *tui_clipboard_read(TuiState *state) {
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
    /* Read clipboard content with size limit to prevent OOM */
#define MAX_CLIPBOARD_SIZE (16 * 1024 * 1024) /* 16MB limit */
    size_t capacity = 4096;
    size_t len = 0;
    paste_text = safe_malloc(capacity);
    int c;
    bool size_exceeded = false;
    while ((c = fgetc(p)) != EOF) {
      if (len + 1 >= capacity) {
        if (capacity > SIZE_MAX / 2 || capacity * 2 > MAX_CLIPBOARD_SIZE) {
          size_exceeded = true;
          break;
        }
        capacity *= 2;
        paste_text = safe_realloc(paste_text, capacity);
      }
      paste_text[len++] = (char)c;
    }
    if (size_exceeded) {
      free(paste_text);
      paste_text = NULL;
    } else {
      paste_text[len] = '\0';
    }
#undef MAX_CLIPBOARD_SIZE
    int status = pclose(p);
    /* OS clipboard is accessible if command succeeded (status 0) */
    os_clipboard_accessible = (status == 0);
    if (!os_clipboard_accessible || (paste_text && paste_text[0] == '\0')) {
      free(paste_text);
      paste_text = NULL;
    }
  }

  /* Only fall back to internal buffer if OS clipboard is inaccessible */
  if (!os_clipboard_accessible && state && state->clipboard_buffer) {
    paste_text = str_dup(state->clipboard_buffer);
  }

  return paste_text;
}
