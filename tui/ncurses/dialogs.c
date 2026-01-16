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
#include "util/mem.h"
#include "tui_internal.h"
#include "views/config_view.h"
#include "views/connect_view.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Load history for a connection if in persistent mode */
static void load_connection_history(TuiState *state, Connection *conn) {
  if (!state || !state->app || !conn || !conn->saved_conn_id)
    return;

  /* Only load from file if persistent mode is enabled */
  Config *config = state->app->config;
  if (!config || config->general.history_mode != HISTORY_MODE_PERSISTENT)
    return;

  /* If history already exists (created in app_add_connection), just set the ID
   */
  if (conn->history) {
    if (!conn->history->connection_id) {
      conn->history->connection_id = str_dup(conn->saved_conn_id);
    }
  } else {
    /* Create history object with connection ID */
    conn->history = history_create(conn->saved_conn_id);
    if (!conn->history)
      return;
  }

  /* Load from file (ignore errors - empty history is fine) */
  char *err = NULL;
  if (!history_load(conn->history, &err)) {
    free(err);
    /* History file may not exist yet - that's fine */
  }
}

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
    case 'h':
    case 'l':
      dialog_ctx_cycle_button(&ctx, 2);
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

  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);

  /* Ensure minimum usable size */
  if (screen_h < 12 || screen_w < 40)
    return NULL;

  /* Clear screen for clean dialog display */
  clear();
  refresh();

  int dlg_height = 9;
  int dlg_width = 55;
  if (dlg_width > screen_w - 4)
    dlg_width = screen_w - 4;

  int dlg_y = (screen_h - dlg_height) / 2;
  int dlg_x = (screen_w - dlg_width) / 2;
  if (dlg_y < 0)
    dlg_y = 0;
  if (dlg_x < 0)
    dlg_x = 0;

  /* Create dialog window */
  WINDOW *dlg = newwin(dlg_height, dlg_width, dlg_y, dlg_x);
  if (!dlg)
    return NULL;

  keypad(dlg, TRUE);

  char buf[128];
  size_t len = 0;
  size_t cursor = 0;
  buf[0] = '\0';

  /* Focus: 0 = input, 1 = OK button, 2 = Cancel button */
  int focus = 0;

  char *result = NULL;
  bool running = true;

  while (running) {
    werase(dlg);
    box(dlg, 0, 0);

    /* Title */
    int title_len = (int)strlen(title) + 2;
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 0, (dlg_width - title_len) / 2, " %s ", title);
    wattroff(dlg, A_BOLD);

    /* Label */
    mvwprintw(dlg, 2, 2, "%s", label);

    /* Password input field */
    if (focus == 0) {
      wattron(dlg, COLOR_PAIR(COLOR_SELECTED));
    }
    mvwhline(dlg, 3, 2, ' ', dlg_width - 4);
    /* Show asterisks instead of actual password */
    for (size_t i = 0; i < len && (int)i < dlg_width - 5; i++) {
      mvwaddch(dlg, 3, 2 + (int)i, '*');
    }
    if (focus == 0) {
      wattroff(dlg, COLOR_PAIR(COLOR_SELECTED));
    }

    /* Error message */
    if (error_msg && error_msg[0]) {
      wattron(dlg, COLOR_PAIR(COLOR_ERROR_TEXT));
      int max_err_len = dlg_width - 4;
      mvwprintw(dlg, 4, 2, "%.*s", max_err_len, error_msg);
      wattroff(dlg, COLOR_PAIR(COLOR_ERROR_TEXT));
    }

    /* Separator line */
    wattron(dlg, A_DIM);
    mvwaddch(dlg, 5, 0, ACS_LTEE);
    mvwhline(dlg, 5, 1, ACS_HLINE, dlg_width - 2);
    mvwaddch(dlg, 5, dlg_width - 1, ACS_RTEE);
    wattroff(dlg, A_DIM);

    /* Buttons */
    int btn_x = dlg_width / 2 - 10;
    if (focus == 1)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 7, btn_x, "[ OK ]");
    if (focus == 1)
      wattroff(dlg, A_REVERSE);
    if (focus == 2)
      wattron(dlg, A_REVERSE);
    mvwprintw(dlg, 7, btn_x + 8, "[ Cancel ]");
    if (focus == 2)
      wattroff(dlg, A_REVERSE);

    if (focus == 0) {
      wmove(dlg, 3, 2 + (int)cursor);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    int ch = wgetch(dlg);
    UiEvent event;
    render_translate_key(ch, &event);

    if (render_event_is_special(&event, UI_KEY_ESCAPE)) {
      running = false;
    } else if (render_event_is_special(&event, UI_KEY_TAB) ||
               render_event_is_special(&event, UI_KEY_DOWN)) {
      focus = FOCUS_NEXT(focus, 3);
    } else if (render_event_is_special(&event, UI_KEY_UP)) {
      focus = FOCUS_PREV(focus, 3);
    } else if (render_event_is_special(&event, UI_KEY_ENTER)) {
      if (focus == 2) {
        /* Cancel */
        running = false;
      } else {
        /* OK - return even if empty (user might want empty password) */
        result = str_dup(buf);
        running = false;
      }
    } else if (focus == 0) {
      /* Input field handling */
      if (render_event_is_special(&event, UI_KEY_BACKSPACE)) {
        if (cursor > 0 && cursor <= len) {
          memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
          cursor--;
          len--;
        }
      } else if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (cursor > 0)
          cursor--;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (cursor < len)
          cursor++;
      } else if (render_event_is_special(&event, UI_KEY_HOME)) {
        cursor = 0;
      } else if (render_event_is_special(&event, UI_KEY_END)) {
        cursor = len;
      } else {
        int key_char = render_event_get_char(&event);
        if (render_event_is_char(&event) && key_char >= 32 && key_char < 127 &&
            len < sizeof(buf) - 1) {
          memmove(buf + cursor + 1, buf + cursor, len - cursor + 1);
          buf[cursor] = (char)key_char;
          cursor++;
          len++;
        }
      }
    } else {
      /* Button navigation with left/right */
      if (render_event_is_special(&event, UI_KEY_LEFT)) {
        if (focus == 2)
          focus = 1;
      } else if (render_event_is_special(&event, UI_KEY_RIGHT)) {
        if (focus == 1)
          focus = 2;
      }
    }
  }

  curs_set(0);
  delwin(dlg);

  /* Securely zero password buffer before returning to prevent stack leakage */
  volatile char *vbuf = buf;
  for (size_t i = 0; i < sizeof(buf); i++) {
    vbuf[i] = 0;
  }

  /* Force screen refresh to clear dialog remnants */
  touchwin(stdscr);
  refresh();

  return result;
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
      total_rows = tab->query_paginated ? tab->query_total_rows
                                        : tab->query_results->num_rows;
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
      selected = 1 - selected;
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
                    target_row > PAGE_SIZE / 2 ? target_row - PAGE_SIZE / 2 : 0;
                query_load_rows_at(state, tab, load_offset);
                tab->query_result_row = target_row - tab->query_loaded_offset;
              }
            } else {
              /* Non-paginated - just move cursor */
              tab->query_result_row = target_row;
            }

            /* Adjust scroll */
            int win_rows = state->term_rows - 4;
            int editor_height = (win_rows - 1) * 3 / 10;
            if (editor_height < 3)
              editor_height = 3;
            int visible = win_rows - editor_height - 4;
            if (visible < 1)
              visible = 1;

            if (tab->query_result_row < tab->query_result_scroll_row) {
              tab->query_result_scroll_row = tab->query_result_row;
            } else if (tab->query_result_row >=
                       tab->query_result_scroll_row + (size_t)visible) {
              tab->query_result_scroll_row =
                  tab->query_result_row - (size_t)visible + 1;
            }

            /* Ensure focus is on results */
            if (ui)
              ui->query_focus_results = true;
          } else {
            /* Handle regular table navigation */
            /* Check if target is in currently loaded range */
            if (target_row >= tab->loaded_offset &&
                target_row < tab->loaded_offset + tab->loaded_count) {
              /* Already loaded, just move cursor */
              tab->cursor_row = target_row - tab->loaded_offset;

              /* Adjust scroll */
              if (tab->cursor_row < tab->scroll_row) {
                tab->scroll_row = tab->cursor_row;
              } else if (tab->cursor_row >=
                         tab->scroll_row + (size_t)state->content_rows) {
                tab->scroll_row = tab->cursor_row - state->content_rows + 1;
              }
            } else {
              /* Need to load new data - use async with progress */
              size_t load_offset =
                  target_row > PAGE_SIZE / 2 ? target_row - PAGE_SIZE / 2 : 0;

              /* Get table name from Tab */
              Tab *curr_tab = TUI_TAB(state);
              DbConnection *conn = TUI_CONN(state);
              const char *table = curr_tab ? curr_tab->table_name : NULL;
              char *where_clause = NULL;

              /* Build WHERE clause from filters if applicable */
              if (curr_tab && curr_tab->filters.num_filters > 0 &&
                  curr_tab->schema && conn) {
                char *err = NULL;
                where_clause =
                    filters_build_where(&curr_tab->filters, curr_tab->schema,
                                        conn->driver->name, &err);
                free(err);
              }

              /* Use async operation with progress dialog */
              AsyncOperation op;
              async_init(&op);
              op.conn = state->conn;
              op.table_name = str_dup(table);
              op.offset = load_offset;
              op.limit = PAGE_SIZE;
              op.order_by = NULL;
              op.desc = false;

              if (where_clause) {
                op.op_type = ASYNC_OP_QUERY_PAGE_WHERE;
                op.where_clause = where_clause;
              } else {
                op.op_type = ASYNC_OP_QUERY_PAGE;
              }

              bool load_succeeded = false;
              if (async_start(&op)) {
                bool completed =
                    tui_show_processing_dialog(state, &op, "Loading data...");

                if (completed && op.state == ASYNC_STATE_COMPLETED &&
                    op.result) {
                  ResultSet *new_data = (ResultSet *)op.result;

                  /* Check if we got 0 rows with approximate count */
                  bool was_approximate = false;
                  Tab *check_tab = TUI_TAB(state);
                  if (check_tab) {
                    was_approximate = check_tab->row_count_approximate;
                  }

                  if (new_data->num_rows == 0 && was_approximate &&
                      load_offset > 0) {
                    /* Approximate count was wrong - get exact count */
                    db_result_free(new_data);
                    async_free(&op);

                    AsyncOperation count_op;
                    async_init(&count_op);
                    count_op.op_type = ASYNC_OP_COUNT_ROWS;
                    count_op.conn = state->conn;
                    count_op.table_name = str_dup(table);
                    count_op.use_approximate = false; /* Force exact */

                    if (count_op.table_name && async_start(&count_op)) {
                      bool count_done = tui_show_processing_dialog(
                          state, &count_op, "Counting rows (exact)...");
                      if (count_done &&
                          count_op.state == ASYNC_STATE_COMPLETED &&
                          count_op.count > 0) {
                        /* Update total rows with exact count */
                        Tab *update_tab = TUI_TAB(state);
                        if (update_tab) {
                          update_tab->total_rows = (size_t)count_op.count;
                          update_tab->row_count_approximate = false;
                        }

                        /* Clamp target row to actual data */
                        size_t total = update_tab ? update_tab->total_rows : 0;
                        if (target_row >= total) {
                          target_row = total > 0 ? total - 1 : 0;
                        }

                        /* Recalculate load offset and reload */
                        load_offset = target_row > PAGE_SIZE / 2
                                          ? target_row - PAGE_SIZE / 2
                                          : 0;

                        /* Clean up count dialog and refresh before next dialog
                         */
                        async_free(&count_op);
                        touchwin(stdscr);
                        tui_refresh(state);

                        /* Use tui_load_rows_at_with_dialog which handles edge
                         * cases */
                        if (tui_load_rows_at_with_dialog(state, load_offset)) {
                          Tab *load_tab = TUI_TAB(state);
                          if (load_tab) {
                            load_tab->cursor_row =
                                target_row - load_tab->loaded_offset;
                            if (load_tab->cursor_row < load_tab->scroll_row) {
                              load_tab->scroll_row = load_tab->cursor_row;
                            } else if (load_tab->cursor_row >=
                                       load_tab->scroll_row +
                                           (size_t)state->content_rows) {
                              load_tab->scroll_row =
                                  load_tab->cursor_row - state->content_rows + 1;
                            }
                          }
                        }
                        tui_refresh(state);
                        return;
                      }
                    }
                    async_free(&count_op);
                    tui_refresh(state);
                    return;
                  }

                  /* Update tab directly */
                  Tab *upd_tab = TUI_TAB(state);
                  if (upd_tab) {
                    /* Free old data but keep schema */
                    if (upd_tab->data) {
                      db_result_free(upd_tab->data);
                    }
                    upd_tab->data = new_data;
                    upd_tab->loaded_offset = load_offset;
                    upd_tab->loaded_count = new_data->num_rows;

                    /* Apply schema column names */
                    if (upd_tab->schema && upd_tab->data) {
                      size_t min_cols = upd_tab->schema->num_columns;
                      if (upd_tab->data->num_columns < min_cols) {
                        min_cols = upd_tab->data->num_columns;
                      }
                      for (size_t i = 0; i < min_cols; i++) {
                        if (upd_tab->schema->columns[i].name) {
                          free(upd_tab->data->columns[i].name);
                          upd_tab->data->columns[i].name =
                              str_dup(upd_tab->schema->columns[i].name);
                          upd_tab->data->columns[i].type =
                              upd_tab->schema->columns[i].type;
                        }
                      }
                    }

                    /* Tab data is updated directly - no sync needed */
                    load_succeeded = true;
                  }
                }
              }
              async_free(&op);

              /* Only update cursor if load succeeded */
              if (load_succeeded) {
                Tab *cur_tab = TUI_TAB(state);
                if (cur_tab && cur_tab->data) {
                  /* Clamp cursor to actual loaded data if target was beyond */
                  size_t actual_target = target_row - cur_tab->loaded_offset;
                  if (actual_target >= cur_tab->data->num_rows) {
                    actual_target = cur_tab->data->num_rows > 0
                                        ? cur_tab->data->num_rows - 1
                                        : 0;
                  }
                  cur_tab->cursor_row = actual_target;

                  /* Adjust scroll */
                  if (cur_tab->cursor_row < cur_tab->scroll_row) {
                    cur_tab->scroll_row = cur_tab->cursor_row;
                  } else if (cur_tab->cursor_row >=
                             cur_tab->scroll_row + (size_t)state->content_rows) {
                    cur_tab->scroll_row =
                        cur_tab->cursor_row - state->content_rows + 1;
                  }
                }
              }
              /* If cancelled/failed, keep current view unchanged */
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

/* Show schema dialog */
void tui_show_schema(TuiState *state) {
  /* Get schema via VmTable if available, fallback to Tab */
  VmTable *vm = tui_vm_table(state);
  Tab *schema_tab = TUI_TAB(state);
  const TableSchema *schema = vm ? vm_table_schema(vm) : (schema_tab ? schema_tab->schema : NULL);

  if (!state || !schema) {
    tui_set_error(state, "No schema available");
    return;
  }

  /* Create a popup window */
  int height = state->term_rows - 4;
  int width = state->term_cols - 10;
  int starty = 2;
  int startx = 5;

  /* Ensure minimum dimensions */
  if (height < 5)
    height = 5;
  if (width < 20)
    width = 20;

  WINDOW *schema_win = newwin(height, width, starty, startx);
  if (!schema_win)
    return;

  keypad(schema_win, TRUE);

  int scroll_offset = 0;
  int max_scroll = 0;
  bool running = true;

  while (running) {
    werase(schema_win);
    DRAW_BOX(schema_win, COLOR_BORDER);
    WITH_ATTR(schema_win, A_BOLD,
              mvwprintw(schema_win, 0, 2, " Schema: %s ", schema->name));

    int y = 2;
    int content_height = height - 4;
    int line = 0;

    /* Calculate total lines needed */
    int total_lines = 2 + (int)schema->num_columns; /* Columns header + data */
    if (schema->num_indexes > 0) {
      total_lines += 2 + (int)schema->num_indexes; /* Indexes section */
    }
    if (schema->num_foreign_keys > 0) {
      total_lines += 2 + (int)schema->num_foreign_keys; /* FKs section */
    }

    max_scroll = total_lines - content_height;
    if (max_scroll < 0)
      max_scroll = 0;

/* Draw content with scrolling */
#define DRAW_LINE(fmt, ...)                                                    \
  do {                                                                         \
    if (line >= scroll_offset && y < height - 2) {                             \
      mvwprintw(schema_win, y++, 2, fmt, ##__VA_ARGS__);                       \
    }                                                                          \
    line++;                                                                    \
  } while (0)

    /* Columns section */
    wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
    DRAW_LINE("Columns (%zu):", schema->num_columns);
    wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

    if (line >= scroll_offset && y < height - 2) {
      wattron(schema_win, A_BOLD);
      mvwprintw(schema_win, y++, 4, "%-20s %-15s %-8s %-8s %-8s", "Name",
                "Type", "Nullable", "PK", "AI");
      wattroff(schema_win, A_BOLD);
    }
    line++;

    for (size_t i = 0; i < schema->num_columns; i++) {
      const ColumnDef *col = &schema->columns[i];
      if (line >= scroll_offset && y < height - 2) {
        mvwprintw(schema_win, y++, 4, "%-20s %-15s %-8s %-8s %-8s",
                  col->name ? col->name : "",
                  col->type_name ? col->type_name
                                 : db_value_type_name(col->type),
                  col->nullable ? "YES" : "NO", col->primary_key ? "YES" : "",
                  col->auto_increment ? "YES" : "");
      }
      line++;
    }

    /* Indexes section */
    if (schema->num_indexes > 0) {
      line++; /* blank line */
      if (line >= scroll_offset && y < height - 2)
        y++;

      wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
      DRAW_LINE("Indexes (%zu):", schema->num_indexes);
      wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

      for (size_t i = 0; i < schema->num_indexes; i++) {
        const IndexDef *idx = &schema->indexes[i];
        if (line >= scroll_offset && y < height - 2) {
          /* Build column list string safely */
          char cols[256] = "";
          size_t pos = 0;
          for (size_t j = 0; j < idx->num_columns && pos < sizeof(cols) - 1;
               j++) {
            size_t remaining = sizeof(cols) - pos;
            int written;
            if (j > 0) {
              written = snprintf(cols + pos, remaining, ", %s",
                                 idx->columns[j] ? idx->columns[j] : "");
            } else {
              written = snprintf(cols + pos, remaining, "%s",
                                 idx->columns[j] ? idx->columns[j] : "");
            }
            if (written > 0) {
              /* Check if truncation occurred before updating pos */
              if ((size_t)written >= remaining) {
                pos = sizeof(cols) - 1;
                break;
              }
              pos += (size_t)written;
            }
          }
          mvwprintw(schema_win, y++, 4, "%s%-20s %s(%s)",
                    idx->unique ? "[U] " : "    ", idx->name ? idx->name : "",
                    idx->type ? idx->type : "", cols);
        }
        line++;
      }
    }

    /* Foreign Keys section */
    if (schema->num_foreign_keys > 0) {
      line++; /* blank line */
      if (line >= scroll_offset && y < height - 2)
        y++;

      wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
      DRAW_LINE("Foreign Keys (%zu):", schema->num_foreign_keys);
      wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

      for (size_t i = 0; i < schema->num_foreign_keys; i++) {
        const ForeignKeyDef *fk = &schema->foreign_keys[i];
        if (line >= scroll_offset && y < height - 2) {
          /* Build column lists safely */
          char src_cols[128] = "";
          size_t pos = 0;
          for (size_t j = 0; j < fk->num_columns && pos < sizeof(src_cols) - 1;
               j++) {
            int written;
            if (j > 0) {
              written = snprintf(src_cols + pos, sizeof(src_cols) - pos, ", %s",
                                 fk->columns[j] ? fk->columns[j] : "");
            } else {
              written = snprintf(src_cols + pos, sizeof(src_cols) - pos, "%s",
                                 fk->columns[j] ? fk->columns[j] : "");
            }
            if (written > 0) {
              /* Check for truncation BEFORE updating pos */
              size_t remaining = sizeof(src_cols) - pos;
              if ((size_t)written >= remaining) {
                pos = sizeof(src_cols) - 1;
                break;
              }
              pos += (size_t)written;
            }
          }
          char ref_cols[128] = "";
          pos = 0;
          for (size_t j = 0;
               j < fk->num_ref_columns && pos < sizeof(ref_cols) - 1; j++) {
            int written;
            if (j > 0) {
              written = snprintf(ref_cols + pos, sizeof(ref_cols) - pos, ", %s",
                                 fk->ref_columns[j] ? fk->ref_columns[j] : "");
            } else {
              written = snprintf(ref_cols + pos, sizeof(ref_cols) - pos, "%s",
                                 fk->ref_columns[j] ? fk->ref_columns[j] : "");
            }
            if (written > 0) {
              /* Check for truncation BEFORE updating pos */
              size_t remaining = sizeof(ref_cols) - pos;
              if ((size_t)written >= remaining) {
                pos = sizeof(ref_cols) - 1;
                break;
              }
              pos += (size_t)written;
            }
          }
          mvwprintw(schema_win, y++, 4, "(%s) -> %s(%s)", src_cols,
                    fk->ref_table ? fk->ref_table : "?", ref_cols);
        }
        line++;
      }
    }

#undef DRAW_LINE

    /* Footer */
    if (max_scroll > 0) {
      mvwprintw(schema_win, height - 2, 2,
                "[Up/Down] Scroll  [Esc] Close  (%d/%d)", scroll_offset + 1,
                max_scroll + 1);
    } else {
      mvwprintw(schema_win, height - 2, 2, "[Esc] Close");
    }

    wrefresh(schema_win);

    int ch = wgetch(schema_win);
    switch (ch) {
    case 27: /* Escape */
      running = false;
      break;
    case KEY_UP:
    case 'k':
      if (scroll_offset > 0)
        scroll_offset--;
      break;
    case KEY_DOWN:
    case 'j':
      if (scroll_offset < max_scroll)
        scroll_offset++;
      break;
    case KEY_PPAGE:
      scroll_offset -= content_height / 2;
      if (scroll_offset < 0)
        scroll_offset = 0;
      break;
    case KEY_NPAGE:
      scroll_offset += content_height / 2;
      if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;
      break;
    }
  }

  delwin(schema_win);
  touchwin(stdscr);
  tui_refresh(state);
}

/* Show connect dialog */
void tui_show_connect_dialog(TuiState *state) {
  ConnectResult result = connect_view_show(state);

  if (result.mode == CONNECT_MODE_QUIT) {
    str_secure_free(result.connstr); /* Connection string may contain password */
    free(result.saved_conn_id);
    state->running = false;
    state->app->running = false;
    return;
  }

  if (result.mode == CONNECT_MODE_CANCELLED || !result.connstr) {
    str_secure_free(result.connstr); /* Connection string may contain password */
    free(result.saved_conn_id);
    tui_refresh(state);
    return;
  }

  if (result.mode == CONNECT_MODE_NEW_WORKSPACE) {
    /* Create a NEW workspace and connect (keep existing workspaces) */
    char *err = NULL;
    DbConnection *conn = tui_connect_with_progress(state, result.connstr);
    if (!conn) {
      /* Error already shown */
      str_secure_free(result.connstr); /* Connection string may contain password */
      free(result.saved_conn_id);
      tui_refresh(state);
      return;
    }

    /* Add to connection pool */
    Connection *app_conn = app_add_connection(state->app, conn, result.connstr);
    if (!app_conn) {
      db_disconnect(conn);
      tui_set_error(state, "Failed to add connection");
      str_secure_free(result.connstr); /* Connection string may contain password */
      free(result.saved_conn_id);
      tui_refresh(state);
      return;
    }

    /* Store saved connection ID for session persistence */
    app_conn->saved_conn_id = result.saved_conn_id;
    result.saved_conn_id = NULL; /* Ownership transferred */

    /* Load query history if persistent mode enabled */
    load_connection_history(state, app_conn);

    /* Load tables for this connection */
    size_t num_tables = 0;
    char **tables = db_list_tables(conn, &num_tables, &err);
    if (tables) {
      app_conn->tables = tables;
      app_conn->num_tables = num_tables;
    } else if (err) {
      tui_set_error(state, "Failed to load tables: %s", err);
      free(err);
    }

    /* Find connection index */
    size_t conn_index = app_find_connection_index(state->app, conn);

    /* Save current workspace state before switching */
    if (state->app->num_workspaces > 0) {
      tui_sync_to_workspace(state);
    }

    /* Reuse current workspace if it's empty, otherwise create a new one */
    Workspace *ws = app_current_workspace(state->app);
    if (ws && ws->num_tabs == 0) {
      /* Reuse empty workspace */
    } else {
      /* Create a NEW workspace */
      ws = app_create_workspace(state->app);
      if (!ws) {
        tui_set_error(state, "Failed to create workspace (out of memory)");
        str_secure_free(result.connstr); /* Connection string may contain password */
        tui_refresh(state);
        return;
      }
    }

    /* Tab data will be managed per-tab */

    /* Update TUI state to use new connection */
    state->conn = conn;
    state->tables = app_conn->tables;
    state->num_tables = app_conn->num_tables;

    /* Check if we should auto-open the first table */
    bool auto_open = state->app->config &&
                     state->app->config->general.auto_open_first_table &&
                     app_conn->num_tables > 0;

    if (auto_open) {
      /* Create a table tab with the first table */
      size_t table_idx = 0;
      Tab *tab = workspace_create_table_tab(ws, conn_index, table_idx,
                                            app_conn->tables[table_idx]);
      if (tab) {
        tab->table_index = table_idx;
        tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                   ws->current_tab);

        /* Load table data (updates Tab directly) */
        tui_load_table_data(state, app_conn->tables[table_idx]);

        /* Initialize UI state */
        UITabState *ui = TUI_TAB_UI(state);
        if (ui) {
          ui->sidebar_visible = true;
          ui->sidebar_focused = false;

          /* Initialize widgets for the new table tab */
          tui_init_table_tab_widgets(state, ui, tab);
        }
        state->sidebar_visible = true;
        state->sidebar_focused = false;

        tui_set_status(state, "Connected in workspace %zu - %s",
                       state->app->current_workspace + 1,
                       app_conn->tables[table_idx]);
      }
    } else {
      /* Create a connection tab (don't auto-load any table) */
      Tab *tab =
          workspace_create_connection_tab(ws, conn_index, result.connstr);
      if (tab) {
        /* Show sidebar for table selection */
        state->sidebar_visible = true;
        state->sidebar_focused = true;
        tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                   ws->current_tab);
        UITabState *ui = TUI_TAB_UI(state);
        if (ui) {
          ui->sidebar_visible = true;
          ui->sidebar_focused = true;
          ui->sidebar_highlight = 0;
          ui->sidebar_scroll = 0;
        }

        if (app_conn->num_tables == 0) {
          tui_set_status(state, "Connected in workspace %zu (no tables)",
                         state->app->current_workspace + 1);
        } else {
          tui_set_status(state,
                         "Connected in workspace %zu (%s) - Select a table",
                         state->app->current_workspace + 1, conn->database);
        }
      }
    }

    /* Recreate windows if sidebar visibility changed */
    tui_recreate_windows(state);
  } else if (result.mode == CONNECT_MODE_NEW_TAB) {
    /* Add connection to pool and create new tab in current workspace */
    char *err = NULL;
    DbConnection *conn = db_connect(state->app->client, result.connstr, &err);
    if (!conn) {
      tui_set_error(state, "Connection failed: %s",
                    err ? err : "Unknown error");
      free(err);
      str_secure_free(result.connstr); /* Connection string may contain password */
      free(result.saved_conn_id);
      tui_refresh(state);
      return;
    }

    /* Add to connection pool */
    Connection *app_conn = app_add_connection(state->app, conn, result.connstr);
    if (!app_conn) {
      db_disconnect(conn);
      tui_set_error(state, "Failed to add connection");
      str_secure_free(result.connstr); /* Connection string may contain password */
      free(result.saved_conn_id);
      tui_refresh(state);
      return;
    }

    /* Store saved connection ID for session persistence */
    app_conn->saved_conn_id = result.saved_conn_id;
    result.saved_conn_id = NULL; /* Ownership transferred */

    /* Load query history if persistent mode enabled */
    load_connection_history(state, app_conn);

    /* Load tables for this connection */
    size_t num_tables = 0;
    char **tables = db_list_tables(conn, &num_tables, &err);
    if (tables) {
      app_conn->tables = tables;
      app_conn->num_tables = num_tables;
    } else if (err) {
      tui_set_error(state, "Failed to load tables: %s", err);
      free(err);
    }

    /* Find connection index */
    size_t conn_index = app_find_connection_index(state->app, conn);

    /* Get current workspace */
    Workspace *ws = app_current_workspace(state->app);
    if (!ws) {
      ws = app_create_workspace(state->app);
    }

    if (ws) {
      /* Save current tab state */
      if (ws->num_tabs > 0) {
        tab_save(state);
        /* Tab owns its data - no cache clearing needed */
      }

      /* Update TUI state to use new connection */
      state->conn = conn;
      state->tables = app_conn->tables;
      state->num_tables = app_conn->num_tables;

      /* Check if we should auto-open the first table */
      bool auto_open = state->app->config &&
                       state->app->config->general.auto_open_first_table &&
                       app_conn->num_tables > 0;

      if (auto_open) {
        /* Create a table tab with the first table */
        size_t table_idx = 0;
        Tab *tab = workspace_create_table_tab(ws, conn_index, table_idx,
                                              app_conn->tables[table_idx]);
        if (tab) {
          tab->table_index = table_idx;
          tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                     ws->current_tab);

          /* Load table data (updates Tab directly) */
          tui_load_table_data(state, app_conn->tables[table_idx]);

          /* Initialize UI state */
          UITabState *ui = TUI_TAB_UI(state);
          if (ui) {
            ui->sidebar_visible = true;
            ui->sidebar_focused = false;

            /* Initialize widgets for the new table tab */
            tui_init_table_tab_widgets(state, ui, tab);
          }
          state->sidebar_visible = true;
          state->sidebar_focused = false;

          tui_recreate_windows(state);
          tui_set_status(state, "Connected in new tab - %s",
                         app_conn->tables[table_idx]);
        }
      } else {
        /* Create a connection tab (don't auto-load any table) */
        Tab *tab =
            workspace_create_connection_tab(ws, conn_index, result.connstr);
        if (tab) {
          /* Initialize UITabState - new connection shows sidebar focused */
          tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                     ws->current_tab);
          UITabState *ui = TUI_TAB_UI(state);
          if (ui) {
            ui->sidebar_visible = true;
            ui->sidebar_focused = true;
            ui->sidebar_highlight = 0;
            ui->sidebar_scroll = 0;
            ui->filters_visible = false;
            ui->filters_focused = false;
          }
          state->sidebar_visible = true;
          state->sidebar_focused = true;

          tui_recreate_windows(state);
          if (app_conn->num_tables == 0) {
            tui_set_status(state, "Connected in new tab (%s) - no tables found",
                           conn->database);
          } else {
            tui_set_status(state, "Connected in new tab (%s) - Select a table",
                           conn->database);
          }
        }
      }
    }
  }

  free(result.connstr);
  tui_refresh(state);
}

/* Show configuration dialog */
void tui_show_config(TuiState *state) {
  config_view_show(state);
  tui_refresh(state);
}

/* Show table selector dialog */
void tui_show_table_selector(TuiState *state) {
  char **tables = TUI_TABLES(state);
  size_t num_tables = TUI_NUM_TABLES(state);
  if (!state || !tables || num_tables == 0) {
    tui_set_error(state, "No tables available");
    return;
  }

  int height = (int)num_tables + 4;
  if (height > state->term_rows - 4) {
    height = state->term_rows - 4;
  }
  /* Ensure minimum dimensions */
  if (height < 5)
    height = 5;
  int width = 40;
  int starty, startx;
  dialog_center_position(&starty, &startx, height, width, state->term_rows, state->term_cols);

  WINDOW *menu_win = newwin(height, width, starty, startx);
  if (!menu_win)
    return;

  keypad(menu_win, TRUE);
  DRAW_BOX(menu_win, COLOR_BORDER);
  WITH_ATTR(menu_win, A_BOLD, mvwprintw(menu_win, 0, 2, " Select Table "));

  /* Create menu items */
  ITEM **items = safe_calloc(num_tables + 1, sizeof(ITEM *));

  for (size_t i = 0; i < num_tables; i++) {
    items[i] = new_item(tables[i], "");
  }
  items[num_tables] = NULL;

  MENU *menu = new_menu(items);
  if (!menu) {
    for (size_t i = 0; i < num_tables; i++) {
      free_item(items[i]);
    }
    free(items);
    delwin(menu_win);
    return;
  }

  /* Set menu options */
  menu_setup(menu, menu_win, height, width, 2);

  /* Set current item */
  Tab *sel_tab = TUI_TAB(state);
  if (sel_tab && sel_tab->table_index < num_tables) {
    set_current_item(menu, items[sel_tab->table_index]);
  }

  wrefresh(menu_win);

  mvwprintw(menu_win, height - 1, 2, "Enter:Select  Esc:Cancel");
  wrefresh(menu_win);

  bool running = true;
  while (running) {
    int ch = wgetch(menu_win);
    switch (ch) {
    case KEY_DOWN:
    case 'j':
      menu_driver(menu, REQ_DOWN_ITEM);
      break;

    case KEY_UP:
    case 'k':
      menu_driver(menu, REQ_UP_ITEM);
      break;

    case KEY_NPAGE:
      menu_driver(menu, REQ_SCR_DPAGE);
      break;

    case KEY_PPAGE:
      menu_driver(menu, REQ_SCR_UPAGE);
      break;

    case '\n':
    case KEY_ENTER: {
      ITEM *cur = current_item(menu);
      if (cur) {
        int idx = item_index(cur);
        if (idx >= 0 && (size_t)idx < num_tables) {
          /* Clear filters when switching tables */
          if (sel_tab && sel_tab->type == TAB_TYPE_TABLE) {
            sel_tab->table_index = (size_t)idx;
            filters_clear(&sel_tab->filters);
          }
          tui_load_table_data(state, tables[idx]);
        }
      }
      running = false;
      break;
    }

    case 27: /* Escape */
    case 'q':
      running = false;
      break;
    }
    wrefresh(menu_win);
  }

  /* Cleanup */
  unpost_menu(menu);
  free_menu(menu);
  for (size_t i = 0; i < num_tables; i++) {
    free_item(items[i]);
  }
  free(items);
  delwin(menu_win);

  touchwin(stdscr);
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
  if (!state || !connstr)
    return NULL;

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_CONNECT;
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

/*
 * Load table list with progress dialog.
 * Returns true on success.
 */
bool tui_load_tables_with_progress(TuiState *state) {
  if (!state || !state->conn)
    return false;

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_LIST_TABLES;
  op.conn = state->conn;

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    return false;
  }

  bool completed = tui_show_processing_dialog(state, &op, "Loading tables...");

  if (completed && op.state == ASYNC_STATE_COMPLETED) {
    /* Free old table list if exists */
    if (state->tables) {
      for (size_t i = 0; i < state->num_tables; i++) {
        free(state->tables[i]);
      }
      free(state->tables);
    }

    state->tables = (char **)op.result;
    state->num_tables = op.result_count;

    /* Also update connection state if available */
    Connection *conn = TUI_TAB_CONNECTION(state);
    if (conn) {
      conn->tables = state->tables;
      conn->num_tables = state->num_tables;
    }

    async_free(&op);
    return true;
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Failed to load tables: %s",
                  op.error ? op.error : "Unknown error");
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Operation cancelled");
  }

  async_free(&op);
  return false;
}

/*
 * Count rows with progress dialog (uses approximate count if available).
 * Returns row count on success, -1 on failure.
 */
int64_t tui_count_rows_with_progress(TuiState *state, const char *table,
                                     bool *is_approximate) {
  if (!state || !state->conn || !table)
    return -1;

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_COUNT_ROWS;
  op.conn = state->conn;
  op.table_name = str_dup(table);
  op.use_approximate = true; /* Try fast estimate first */

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    free(op.table_name);
    return -1;
  }

  bool completed = tui_show_processing_dialog(state, &op, "Counting rows...");

  int64_t result = -1;
  if (completed && op.state == ASYNC_STATE_COMPLETED) {
    result = op.count;
    if (is_approximate)
      *is_approximate = op.is_approximate;
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Failed to count rows: %s",
                  op.error ? op.error : "Unknown error");
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Operation cancelled");
  }

  async_free(&op);
  return result;
}

/*
 * Load table schema with progress dialog.
 * Returns schema on success, NULL on failure.
 */
TableSchema *tui_get_schema_with_progress(TuiState *state, const char *table) {
  if (!state || !state->conn || !table)
    return NULL;

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_GET_SCHEMA;
  op.conn = state->conn;
  op.table_name = str_dup(table);

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    free(op.table_name);
    return NULL;
  }

  bool completed = tui_show_processing_dialog(state, &op, "Loading schema...");

  TableSchema *result = NULL;
  if (completed && op.state == ASYNC_STATE_COMPLETED) {
    result = (TableSchema *)op.result;
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Failed to load schema: %s",
                  op.error ? op.error : "Unknown error");
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Operation cancelled");
  }

  async_free(&op);
  return result;
}

/*
 * Query page with progress dialog.
 * Returns result set on success, NULL on failure.
 */
ResultSet *tui_query_page_with_progress(TuiState *state, const char *table,
                                        size_t offset, size_t limit,
                                        const char *order_by, bool desc) {
  if (!state || !state->conn || !table)
    return NULL;

  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_QUERY_PAGE;
  op.conn = state->conn;
  op.table_name = str_dup(table);
  op.offset = offset;
  op.limit = limit;
  op.order_by = order_by ? str_dup(order_by) : NULL;
  op.desc = desc;

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    async_free(&op);
    return NULL;
  }

  bool completed = tui_show_processing_dialog(state, &op, "Loading data...");

  ResultSet *result = NULL;
  if (completed && op.state == ASYNC_STATE_COMPLETED) {
    result = (ResultSet *)op.result;
  } else if (op.state == ASYNC_STATE_ERROR) {
    tui_set_error(state, "Query failed: %s",
                  op.error ? op.error : "Unknown error");
  } else if (op.state == ASYNC_STATE_CANCELLED) {
    tui_set_status(state, "Query cancelled");
  }

  async_free(&op);
  return result;
}
