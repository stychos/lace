/*
 * Lace
 * Modal dialogs
 *
 * These are TUI-specific modal dialogs. VmTable is used for schema access
 * where applicable for future cross-platform consistency.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "async/async.h"
#include "config/config.h"
#include "core/history.h"
#include "../../liblace/include/util/mem.h"
#include "tui_internal.h"
#include "query_internal.h"
#include "views/prompt.h"
#include "views/config_view.h"
#include "views/connect_view.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* Show confirmation dialog - returns true if user confirms */
bool tui_show_confirm_dialog(TuiState *state, const char *message) {
  if (!state)
    return false;

  int msg_len = (int)strlen(message);
  int width = msg_len + 6;
  if (width < 30)
    width = 30;

  DialogContext ctx;
  if (!dialog_ctx_init(&ctx, 7, width, COLOR_BORDER, "Confirm"))
    return false;

  static const char *buttons[] = {"Yes", "No"};
  bool result = false;

  while (ctx.running) {
    dialog_ctx_draw_frame(&ctx);

    /* Message */
    mvwprintw(ctx.win, 2, (ctx.width - msg_len) / 2, "%s", message);

    /* Buttons */
    dialog_ctx_draw_buttons(&ctx, buttons, 2, ctx.selected);
    wrefresh(ctx.win);

    int ch = dialog_ctx_getch(&ctx);
    switch (ch) {
    case KEY_LEFT:
    case KEY_RIGHT:
    case '\t':
    case KEY_BTAB:
    case 'h':
    case 'l':
      dialog_ctx_cycle_button(&ctx, 2); /* Two buttons: either way toggles */
      break;

    case 'y':
    case 'Y':
      result = true;
      ctx.running = false;
      break;

    case 'n':
    case 'N':
    case 27: /* Escape */
      ctx.running = false;
      break;

    case '\n':
    case KEY_ENTER:
      result = (ctx.selected == 0);
      ctx.running = false;
      break;
    }
  }

  dialog_ctx_destroy(&ctx);
  return result;
}

/* Show password input dialog (masks input with asterisks)
 * Returns: malloc'd password string, or NULL if cancelled.
 * Caller must use str_secure_free() on result. */
char *tui_show_password_dialog(TuiState *state, const char *title,
                               const char *label, const char *error_msg) {
  (void)state;
  /* Also asked before the main screen exists (startup, session restore):
   * start from a clean screen */
  clear();
  refresh();
  PromptOptions opts = {title, label, NULL, error_msg, true, true};
  return tui_prompt(&opts);
}

/* Show go-to row dialog */
void tui_show_goto_dialog(TuiState *state) {
  if (!state)
    return;

  /* Determine if we're in a query tab with results */
  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  bool is_query = false;
  size_t total_rows = 0;

  if (tab) {
    if (tab->type == TAB_TYPE_QUERY && tab->query_results &&
        tab->query_results->num_rows > 0) {
      is_query = true;
      if (tab->query_paginated) {
        query_ensure_total(state, tab); /* Cancellable count if unknown */
      }
      total_rows = tab->query_paginated && tab->query_total_known
                       ? tab->query_total_rows
                       : tab->query_loaded_offset + tab->query_results->num_rows;
    }
  }

  /* Fall back to regular table data if not in query tab */
  if (!is_query) {
    /* Use Tab as authoritative source */
    if (!tab || !tab->data)
      return;
    total_rows = tab->total_rows;
  }

  if (total_rows == 0)
    return;

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  int height = 7;
  int width = 50;
  int starty, startx;
  dialog_center_position(&starty, &startx, height, width, term_rows, term_cols);

  WINDOW *win = newwin(height, width, starty, startx);
  if (!win)
    return;

  keypad(win, TRUE);
  curs_set(1);

  char input[32] = {0};
  size_t input_len = 0;
  int selected = 0; /* 0 = Go, 1 = Cancel */

  bool running = true;
  while (running) {
    werase(win);
    DRAW_BOX(win, COLOR_BORDER);
    WITH_ATTR(win, A_BOLD, mvwprintw(win, 0, (width - 14) / 2, " Go to Row "));

    mvwprintw(win, 2, 2, "Enter row number (1-%zu):", total_rows);

    /* Draw input field */
    mvwprintw(win, 3, 2, "%s", input);
    mvwhline(win, 3, 2 + (int)input_len, '_', width - 4 - (int)input_len);

    /* Buttons */
    int btn_y = height - 2;
    int go_x = width / 2 - 12;
    int cancel_x = width / 2 + 2;

    if (selected == 0) {
      wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, go_x, "[ Go ]");
      wattroff(win, A_REVERSE);
      mvwprintw(win, btn_y, cancel_x, "[ Cancel ]");
    } else {
      mvwprintw(win, btn_y, go_x, "[ Go ]");
      wattron(win, A_REVERSE);
      mvwprintw(win, btn_y, cancel_x, "[ Cancel ]");
      wattroff(win, A_REVERSE);
    }

    wmove(win, 3, 2 + (int)input_len);
    wrefresh(win);

    int ch = wgetch(win);

    switch (ch) {
    case '\t':
    case KEY_BTAB:
      selected = 1 - selected; /* Two buttons: either way toggles */
      break;

    case 27: /* Escape */
      running = false;
      break;

    case '\n':
    case KEY_ENTER:
      if (selected == 1) {
        /* Cancel button selected */
        running = false;
        break;
      }
      if (input_len > 0) {
        errno = 0;
        char *endptr;
        long long parsed = strtoll(input, &endptr, 10);
        if (errno != 0 || endptr == input || parsed <= 0 ||
            (unsigned long long)parsed > total_rows) {
          flash();
          break;
        }
        size_t row_num = (size_t)parsed;
        if (row_num > 0 && row_num <= total_rows) {
          size_t target_row = row_num - 1; /* 0-indexed */

          /* Close dialog immediately before loading data */
          curs_set(0);
          delwin(win);
          win = NULL;
          touchwin(stdscr);
          tui_refresh(state);

          if (is_query) {
            /* Handle query results navigation */
            if (tab->query_paginated) {
              /* Check if target is in currently loaded range */
              if (target_row >= tab->query_loaded_offset &&
                  target_row <
                      tab->query_loaded_offset + tab->query_loaded_count) {
                /* Already loaded, just move cursor */
                tab->query_result_row = target_row - tab->query_loaded_offset;
              } else {
                /* Need to load new data - use async with progress */
                size_t load_offset =
                    target_row > TUI_PAGE_SIZE / 2 ? target_row - TUI_PAGE_SIZE / 2 : 0;
                bool loaded = query_load_rows_at(state, tab, load_offset);
                tab = TUI_TAB(state); /* The load dialog ran the event loop */
                if (!loaded || !tab || target_row < tab->query_loaded_offset ||
                    target_row >= tab->query_loaded_offset + tab->query_loaded_count) {
                  /* Cancelled or failed: the cursor stays where it was */
                  tui_refresh(state);
                  return;
                }
                tab->query_result_row = target_row - tab->query_loaded_offset;
              }
            } else {
              /* Non-paginated - just move cursor */
              tab->query_result_row = target_row;
            }

            /* Adjust scroll */
            int visible = query_visible_result_rows(state);

            if (tab->query_result_row < tab->query_result_scroll_row) {
              tab->query_result_scroll_row = tab->query_result_row;
            } else if (tab->query_result_row >=
                       tab->query_result_scroll_row + (size_t)visible) {
              tab->query_result_scroll_row =
                  tab->query_result_row - (size_t)visible + 1;
            }

            /* Ensure focus is on results */
            if (ui) {
              ui->query_focus_results = true;
              ui->query_unfocused = false;
            }
          } else {
            /* Handle regular table navigation */
            /* Data rows on screen (header, sort row and filters excluded) */
            size_t visible = (size_t)tui_get_layout_info(state).visible_rows;
            /* Check if target is in currently loaded range */
            if (target_row >= tab->loaded_offset &&
                target_row < tab->loaded_offset + tab->loaded_count) {
              /* Already loaded, just move cursor */
              tab->cursor_row = target_row - tab->loaded_offset;

              /* Adjust scroll */
              if (tab->cursor_row < tab->scroll_row) {
                tab->scroll_row = tab->cursor_row;
              } else if (tab->cursor_row >= tab->scroll_row + visible) {
                tab->scroll_row = tab->cursor_row - visible + 1;
              }
            } else {
              /* Need to load new data. tui_load_rows_at applies the tab's
               * filters and sort, like every other page load. */
              size_t load_offset =
                  target_row > TUI_PAGE_SIZE / 2 ? target_row - TUI_PAGE_SIZE / 2 : 0;
              bool loaded = tui_load_rows_at_with_dialog(state, load_offset);
              Tab *cur_tab = TUI_TAB(state);

              /* An approximate total can promise rows that do not exist:
               * count exactly, clamp the target and load again */
              if (loaded && cur_tab && cur_tab->data &&
                  cur_tab->data->num_rows == 0 &&
                  cur_tab->row_count_approximate && load_offset > 0) {
                AsyncOperation count_op;
                async_init(&count_op);
                count_op.op_type = ASYNC_OP_COUNT_ROWS;
                count_op.conn = TUI_CONN(state);
                count_op.table_name = str_dup(cur_tab->table_name);
                count_op.use_approximate = false; /* Force exact */
                loaded = false;
                if (count_op.table_name && async_start(&count_op)) {
                  bool count_done = tui_show_processing_dialog(
                      state, &count_op, "Counting rows (exact)...");
                  cur_tab = TUI_TAB(state);
                  if (count_done && cur_tab &&
                      count_op.state == ASYNC_STATE_COMPLETED &&
                      count_op.count > 0) {
                    cur_tab->total_rows = (size_t)count_op.count;
                    cur_tab->row_count_approximate = false;
                    if (target_row >= cur_tab->total_rows)
                      target_row = cur_tab->total_rows - 1;
                    load_offset = target_row > TUI_PAGE_SIZE / 2
                                      ? target_row - TUI_PAGE_SIZE / 2
                                      : 0;
                    loaded = tui_load_rows_at_with_dialog(state, load_offset);
                    cur_tab = TUI_TAB(state);
                  }
                }
                async_free(&count_op);
              }

              /* Move the cursor to the target, clamped to the loaded rows.
               * If cancelled/failed, keep current view unchanged. */
              if (loaded && cur_tab && cur_tab->data) {
                size_t actual_target =
                    target_row >= cur_tab->loaded_offset
                        ? target_row - cur_tab->loaded_offset
                        : 0;
                if (actual_target >= cur_tab->data->num_rows) {
                  actual_target = cur_tab->data->num_rows > 0
                                      ? cur_tab->data->num_rows - 1
                                      : 0;
                }
                cur_tab->cursor_row = actual_target;

                /* Adjust scroll */
                if (cur_tab->cursor_row < cur_tab->scroll_row) {
                  cur_tab->scroll_row = cur_tab->cursor_row;
                } else if (cur_tab->cursor_row >= cur_tab->scroll_row + visible) {
                  cur_tab->scroll_row = cur_tab->cursor_row - visible + 1;
                }
              }
            }
          }

          /* Dialog already closed, exit loop */
          tui_refresh(state);
          return;
        } else {
          /* Invalid row number - flash or beep */
          flash();
          continue;
        }
      }
      running = false;
      break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
      if (input_len > 0) {
        input[--input_len] = '\0';
      }
      break;

    default:
      if (ch >= '0' && ch <= '9' && input_len < sizeof(input) - 1) {
        input[input_len++] = (char)ch;
        input[input_len] = '\0';
      }
      break;
    }
  }

  /* Clean up if dialog wasn't already closed */
  if (win) {
    curs_set(0);
    delwin(win);
    touchwin(stdscr);
    tui_refresh(state);
  }
}

/* The Workspaces window (the workspaces manager) */
void tui_show_connect_dialog(TuiState *state) {
  ConnectResult result = connect_view_show(state);
  if (result.mode == CONNECT_MODE_QUIT) {
    state->running = false;
    state->app->running = false;
    return;
  }
  tui_refresh(state);
}

/* Show configuration dialog */
void tui_show_config(TuiState *state) {
  config_view_show(state);
  tui_refresh(state);
}

/* Spinner characters for processing dialog */
static const char SPINNER_CHARS[] = {'|', '/', '-', '\\'};
#define SPINNER_COUNT 4

/*
 * Show a processing dialog while an async operation runs.
 * Returns true if operation completed, false if cancelled.
 *
 * The dialog:
 * - Only appears if operation takes longer than delay_ms (0 = show immediately)
 * - Shows a message with spinner animation
 * - Polls operation state every 50ms
 * - Allows ESC to cancel
 * - Auto-closes when operation completes
 */
bool tui_show_processing_dialog_ex(TuiState *state, AsyncOperation *op,
                                   const char *message, int delay_ms) {
  if (!state || !op)
    return false;

#define POLL_INTERVAL_MS 50
  int delay_iterations = delay_ms / POLL_INTERVAL_MS;

  WINDOW *dialog = NULL;
  int spinner_frame = 0;
  int iterations = 0;
  int width = 0, height = 7;

  while (1) {
    /* Check operation state */
    AsyncState op_state = async_poll(op);
    if (op_state == ASYNC_STATE_COMPLETED || op_state == ASYNC_STATE_ERROR) {
      if (dialog) {
        delwin(dialog);
        touchwin(stdscr);
      }
      return true; /* Completed (check op->error for errors) */
    }
    if (op_state == ASYNC_STATE_CANCELLED) {
      if (dialog) {
        delwin(dialog);
        touchwin(stdscr);
      }
      return false; /* Cancelled */
    }

    /* Create dialog after delay threshold */
    if (!dialog && iterations >= delay_iterations) {
      int term_rows, term_cols;
      getmaxyx(stdscr, term_rows, term_cols);

      int msg_len = (int)strlen(message);
      width = msg_len + 10; /* Room for spinner and padding */
      if (width < 30)
        width = 30;
      if (width > term_cols - 4)
        width = term_cols - 4;

      int start_y, start_x;
      dialog_center_position(&start_y, &start_x, height, width, term_rows, term_cols);

      dialog = newwin(height, width, start_y, start_x);
      if (dialog) {
        keypad(dialog, TRUE);
        wtimeout(dialog, POLL_INTERVAL_MS);
      }
    }

    if (dialog) {
      /* Draw dialog */
      werase(dialog);
      DRAW_BOX(dialog, COLOR_BORDER);
      WITH_ATTR(dialog, A_BOLD,
                mvwprintw(dialog, 0, (width - 14) / 2, " Processing "));

      /* Spinner and message */
      char spinner = SPINNER_CHARS[spinner_frame];
      mvwprintw(dialog, 2, 2, "%c %s", spinner, message);

      /* Cancel button - centered, 1 line gap before it */
      const char *btn_text = "[ Cancel ]";
      int btn_len = (int)strlen(btn_text);
      wattron(dialog, A_REVERSE);
      mvwprintw(dialog, height - 2, (width - btn_len) / 2, "%s", btn_text);
      wattroff(dialog, A_REVERSE);

      wrefresh(dialog);

      /* Advance spinner */
      spinner_frame = (spinner_frame + 1) % SPINNER_COUNT;

      /* Check for input */
      int ch = wgetch(dialog);
      if (ch == 27 || ch == '\n' || ch == KEY_ENTER) {
        async_cancel(op);
        /* Continue looping until state changes to CANCELLED */
      }
    } else {
      /* No dialog yet - just sleep and poll */
      struct timespec ts = {0, POLL_INTERVAL_MS * 1000000L};
      nanosleep(&ts, NULL);
      iterations++;
    }
  }

#undef POLL_INTERVAL_MS
}

/*
 * Convenience wrapper with default 250ms delay.
 */
bool tui_show_processing_dialog(TuiState *state, AsyncOperation *op,
                                const char *message) {
  return tui_show_processing_dialog_ex(state, op, message, 250);
}

/*
 * Connect to database with progress dialog.
 * Returns connection on success, NULL on failure/cancel.
 */
DbConnection *tui_connect_with_progress(TuiState *state, const char *connstr) {
  if (!state || !connstr || !state->app || !state->app->client)
    return NULL;

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_CONNECT;
  op.client = state->app->client;
  op.connstr = str_dup(connstr);

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start connection");
    free(op.connstr);
    return NULL;
  }

  /* Connection dialog shows immediately (no delay) */
  bool completed =
      tui_show_processing_dialog_ex(state, &op, "Connecting...", 0);

  DbConnection *result = NULL;
  if (completed && op.state == ASYNC_STATE_COMPLETED) {
    result = (DbConnection *)op.result;
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Connection failed: %s",
                  op.error ? op.error : "Unknown error");
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Connection cancelled");
  }

  async_free(&op);
  return result;
}

