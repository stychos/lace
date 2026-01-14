/*
 * Lace
 * Query History Dialog
 *
 * Popup dialog showing per-connection SQL query history.
 * Allows copying queries to clipboard and managing history entries.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../config/config.h"
#include "../../core/history.h"
#include "render_helpers.h"
#include "tui_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Dialog dimensions */
#define HISTORY_DIALOG_MIN_WIDTH 60
#define HISTORY_DIALOG_MIN_HEIGHT 15
#define HISTORY_DIALOG_MAX_WIDTH_RATIO 0.8
#define HISTORY_DIALOG_MAX_HEIGHT_RATIO 0.8

/* Maximum SQL display length before truncation */
#define MAX_SQL_DISPLAY_LEN 60

/* Format timestamp for display (HH:MM:SS) */
static void format_time(time_t ts, char *buf, size_t buf_size) {
  struct tm *tm_info = localtime(&ts);
  if (tm_info) {
    strftime(buf, buf_size, "%H:%M:%S", tm_info);
  } else {
    snprintf(buf, buf_size, "--:--:--");
  }
}

/* Truncate SQL for display, adding ellipsis if needed */
static void truncate_sql(const char *sql, char *buf, size_t buf_size,
                         size_t max_len) {
  if (!sql || !buf || buf_size == 0)
    return;

  /* Skip leading whitespace */
  while (*sql && (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r'))
    sql++;

  size_t sql_len = strlen(sql);
  if (sql_len <= max_len) {
    /* Replace newlines with spaces for display */
    size_t i = 0;
    while (i < sql_len && i < buf_size - 1) {
      buf[i] = (sql[i] == '\n' || sql[i] == '\r') ? ' ' : sql[i];
      i++;
    }
    buf[i] = '\0';
  } else {
    /* Truncate with ellipsis */
    size_t copy_len = max_len - 3;
    if (copy_len >= buf_size)
      copy_len = buf_size - 4;
    size_t i = 0;
    while (i < copy_len) {
      buf[i] = (sql[i] == '\n' || sql[i] == '\r') ? ' ' : sql[i];
      i++;
    }
    buf[i++] = '.';
    buf[i++] = '.';
    buf[i++] = '.';
    buf[i] = '\0';
  }
}

/* Get color pair for entry type */
static int type_color(HistoryEntryType type) {
  switch (type) {
  case HISTORY_TYPE_SELECT:
    return COLOR_PK; /* Primary key color (blue-ish) */
  case HISTORY_TYPE_UPDATE:
    return COLOR_NUMBER; /* Number color */
  case HISTORY_TYPE_DELETE:
    return COLOR_ERROR; /* Red */
  case HISTORY_TYPE_INSERT:
    return COLOR_NULL; /* NULL color (used for success) */
  case HISTORY_TYPE_DDL:
    return COLOR_HEADER; /* Header color */
  default:
    return COLOR_STATUS;
  }
}

/* Copy text to clipboard using pbcopy on macOS, wl-copy/xclip on Linux.
 * Always saves to internal buffer as fallback for pasting within the app. */
static bool copy_to_clipboard(TuiState *state, const char *text) {
  if (!text || !text[0])
    return false;

  /* Always save to internal buffer for in-app pasting */
  if (state) {
    char *dup = strdup(text);
    if (dup) {
      free(state->clipboard_buffer);
      state->clipboard_buffer = dup;
    }
    /* If strdup fails, keep old buffer (if any) */
  }

  /* Try external clipboard */
#ifdef __APPLE__
  FILE *p = popen("pbcopy", "w");
#else
  /* Use wl-copy on Wayland, xclip on X11 */
  const char *cmd =
      getenv("WAYLAND_DISPLAY") ? "wl-copy" : "xclip -selection clipboard";
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

/* Show the history dialog */
void tui_show_history_dialog(TuiState *state) {
  if (!state || !state->app)
    return;

  /* Get connection for current tab */
  Tab *tab = TUI_TAB(state);
  if (!tab)
    return;

  Connection *conn = app_get_connection(state->app, tab->connection_index);
  if (!conn) {
    tui_set_error(state, "No active connection");
    return;
  }

  /* Check if history tracking is enabled */
  QueryHistory *history = conn->history;
  if (!history) {
    tui_set_status(state, "History tracking is disabled");
    return;
  }

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  /* Calculate dialog dimensions */
  int width = (int)(term_cols * HISTORY_DIALOG_MAX_WIDTH_RATIO);
  if (width < HISTORY_DIALOG_MIN_WIDTH)
    width = HISTORY_DIALOG_MIN_WIDTH;
  if (width > term_cols - 4)
    width = term_cols - 4;

  int height = (int)(term_rows * HISTORY_DIALOG_MAX_HEIGHT_RATIO);
  if (height < HISTORY_DIALOG_MIN_HEIGHT)
    height = HISTORY_DIALOG_MIN_HEIGHT;
  if (height > term_rows - 2)
    height = term_rows - 2;

  WINDOW *dialog = dialog_create(height, width, term_rows, term_cols);
  if (!dialog)
    return;

  /* Display state */
  size_t selected = 0;
  size_t scroll_offset = 0;
  size_t visible_rows =
      (size_t)(height - 5); /* Account for border and footer */
  size_t num_entries = history->num_entries;

  /* Main loop */
  bool running = true;
  while (running) {
    werase(dialog);

    /* Draw border and title */
    DRAW_BOX(dialog, COLOR_BORDER);
    WITH_ATTR(dialog, A_BOLD,
              mvwprintw(dialog, 0, (width - 16) / 2, " Query History "));

    /* Entry count */
    char count_str[32];
    snprintf(count_str, sizeof(count_str), " %zu entries ", num_entries);
    mvwprintw(dialog, 0, width - (int)strlen(count_str) - 2, "%s", count_str);

    /* Content area */
    int content_start_y = 1;
    int content_width = width - 2;

    /* Show empty message if no entries */
    if (num_entries == 0) {
      wattron(dialog, A_DIM);
      mvwprintw(dialog, height / 2, (width - 20) / 2, "No history entries");
      wattroff(dialog, A_DIM);
    }

    /* Draw entries (newest first) */
    for (size_t i = 0; i < visible_rows && (scroll_offset + i) < num_entries;
         i++) {
      /* Display newest first */
      size_t entry_idx = num_entries - 1 - (scroll_offset + i);
      const HistoryEntry *entry = &history->entries[entry_idx];

      int row = content_start_y + (int)i + 1;
      int is_selected = (scroll_offset + i) == selected;

      /* Highlight selected row */
      if (is_selected) {
        wattron(dialog, A_REVERSE);
      }

      /* Clear the row first */
      mvwhline(dialog, row, 1, ' ', content_width);

      /* Time */
      char time_str[12];
      format_time(entry->timestamp, time_str, sizeof(time_str));
      mvwprintw(dialog, row, 2, "%s", time_str);

      /* Type tag with color */
      const char *tag = history_type_tag(entry->type);
      if (!is_selected) {
        wattron(dialog, COLOR_PAIR(type_color(entry->type)));
      }
      mvwprintw(dialog, row, 12, "[%s]", tag);
      if (!is_selected) {
        wattroff(dialog, COLOR_PAIR(type_color(entry->type)));
      }

      /* SQL (truncated) */
      char sql_buf[128];
      size_t max_sql_len =
          (size_t)(content_width - 22); /* Leave room for time, type, padding */
      if (max_sql_len > MAX_SQL_DISPLAY_LEN)
        max_sql_len = MAX_SQL_DISPLAY_LEN;
      truncate_sql(entry->sql, sql_buf, sizeof(sql_buf), max_sql_len);
      mvwprintw(dialog, row, 19, "%s", sql_buf);

      if (is_selected) {
        wattroff(dialog, A_REVERSE);
      }
    }

    /* Draw scrollbar if needed */
    if (num_entries > visible_rows && num_entries > 0) {
      int scrollbar_height = (int)visible_rows;
      int thumb_height = (int)((visible_rows * visible_rows) / num_entries);
      if (thumb_height < 1)
        thumb_height = 1;
      size_t scroll_range = num_entries - visible_rows;
      int thumb_pos = (scroll_range > 0)
                          ? (int)((scroll_offset *
                                   (size_t)(scrollbar_height - thumb_height)) /
                                  scroll_range)
                          : 0;

      for (int i = 0; i < scrollbar_height; i++) {
        if (i >= thumb_pos && i < thumb_pos + thumb_height) {
          mvwaddch(dialog, content_start_y + 1 + i, width - 1,
                   ACS_BLOCK | COLOR_PAIR(COLOR_BORDER));
        } else {
          mvwaddch(dialog, content_start_y + 1 + i, width - 1,
                   ACS_VLINE | COLOR_PAIR(COLOR_BORDER));
        }
      }
    }

    /* Footer with instructions - show configured keys */
    int footer_y = height - 2;
    wattron(dialog, A_DIM);
    char *copy_key =
        hotkey_get_display(state->app->config, HOTKEY_HISTORY_COPY);
    char *del_key =
        hotkey_get_display(state->app->config, HOTKEY_HISTORY_DELETE);
    char *clear_key =
        hotkey_get_display(state->app->config, HOTKEY_HISTORY_CLEAR);
    char *close_key =
        hotkey_get_display(state->app->config, HOTKEY_HISTORY_CLOSE);
    mvwprintw(dialog, footer_y, 2,
              "[%s] Copy  [%s] Delete  [%s] Clear All  [%s] Close",
              copy_key ? copy_key : "Enter", del_key ? del_key : "x",
              clear_key ? clear_key : "c", close_key ? close_key : "Esc");
    free(copy_key);
    free(del_key);
    free(clear_key);
    free(close_key);
    wattroff(dialog, A_DIM);

    wrefresh(dialog);

    /* Handle input - translate to UiEvent for hotkey matching */
    int ch = wgetch(dialog);
    UiEvent event;
    render_translate_key(ch, &event);

    /* Navigation keys (not configurable, vim-style) */
    if (ch == 'k' || ch == KEY_UP) {
      if (selected > 0) {
        selected--;
        if (selected < scroll_offset) {
          scroll_offset = selected;
        }
      }
    } else if (ch == 'j' || ch == KEY_DOWN) {
      if (num_entries > 0 && selected < num_entries - 1) {
        selected++;
        if (selected >= scroll_offset + visible_rows) {
          scroll_offset = selected - visible_rows + 1;
        }
      }
    } else if (ch == KEY_PPAGE) {
      if (num_entries > 0) {
        size_t page_size = visible_rows > 1 ? visible_rows - 1 : 1;
        if (selected >= page_size) {
          selected -= page_size;
        } else {
          selected = 0;
        }
        if (selected < scroll_offset) {
          scroll_offset = selected;
        }
      }
    } else if (ch == KEY_NPAGE) {
      if (num_entries > 0) {
        size_t page_size = visible_rows > 1 ? visible_rows - 1 : 1;
        selected += page_size;
        if (selected >= num_entries) {
          selected = num_entries - 1;
        }
        if (selected >= scroll_offset + visible_rows) {
          scroll_offset = selected - visible_rows + 1;
        }
      }
    } else if (ch == 'g' || ch == KEY_HOME) {
      if (num_entries > 0) {
        selected = 0;
        scroll_offset = 0;
      }
    } else if (ch == 'G' || ch == KEY_END) {
      if (num_entries > 0) {
        selected = num_entries - 1;
        if (num_entries > visible_rows) {
          scroll_offset = num_entries - visible_rows;
        }
      }
    }
    /* Configurable hotkeys */
    else if (hotkey_matches(state->app->config, &event, HOTKEY_HISTORY_COPY)) {
      /* Copy selected entry to clipboard */
      if (num_entries > 0) {
        size_t entry_idx = num_entries - 1 - selected;
        const HistoryEntry *entry = &history->entries[entry_idx];
        if (copy_to_clipboard(state, entry->sql)) {
          tui_set_status(state, "SQL copied to clipboard");
        } else {
          tui_set_status(state, "Failed to copy to clipboard");
        }
        running = false;
      }
    } else if (hotkey_matches(state->app->config, &event,
                              HOTKEY_HISTORY_DELETE)) {
      /* Delete selected entry */
      if (num_entries > 0) {
        size_t entry_idx = num_entries - 1 - selected;
        history_remove(history, entry_idx);
        num_entries = history->num_entries;

        /* Adjust selection if needed */
        if (num_entries == 0) {
          running = false;
          tui_set_status(state, "History cleared");
        } else if (selected >= num_entries) {
          selected = num_entries - 1;
        }

        /* Adjust scroll if needed */
        if (scroll_offset > 0 && scroll_offset + visible_rows > num_entries) {
          scroll_offset =
              num_entries > visible_rows ? num_entries - visible_rows : 0;
        }
      }
    } else if (hotkey_matches(state->app->config, &event,
                              HOTKEY_HISTORY_CLEAR)) {
      /* Clear all history (with confirmation) */
      if (num_entries > 0) {
        /* Simple confirmation - show message on same dialog */
        mvwprintw(dialog, height / 2, (width - 30) / 2,
                  "Clear all history? (y/n)");
        wrefresh(dialog);
        int confirm = wgetch(dialog);
        if (confirm == 'y' || confirm == 'Y') {
          history_clear(history);
          running = false;
          tui_set_status(state, "History cleared");
        }
      }
    } else if (hotkey_matches(state->app->config, &event,
                              HOTKEY_HISTORY_CLOSE)) {
      running = false;
    }
  }

  delwin(dialog);
  touchwin(stdscr);
  refresh();
}
