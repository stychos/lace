/*
 * lace - Database Viewer and Manager
 * Modal dialogs
 */

#include "tui_internal.h"
#include "../async/async.h"
#include "views/connect_view.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Show confirmation dialog - returns true if user confirms */
bool tui_show_confirm_dialog(TuiState *state, const char *message) {
  if (!state)
    return false;

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  int msg_len = (int)strlen(message);
  int width = msg_len + 6;
  if (width < 30)
    width = 30;
  if (width > term_cols - 4)
    width = term_cols - 4;

  int height = 7;
  int start_y = (term_rows - height) / 2;
  int start_x = (term_cols - width) / 2;

  /* Clamp coordinates to prevent negative values */
  if (start_y < 0)
    start_y = 0;
  if (start_x < 0)
    start_x = 0;

  WINDOW *dialog = newwin(height, width, start_y, start_x);
  if (!dialog)
    return false;

  keypad(dialog, TRUE);

  int selected = 0; /* 0 = Yes, 1 = No */

  while (1) {
    werase(dialog);
    box(dialog, 0, 0);

    /* Title */
    wattron(dialog, A_BOLD);
    mvwprintw(dialog, 0, (width - 11) / 2, " Confirm ");
    wattroff(dialog, A_BOLD);

    /* Message */
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);

    /* Buttons */
    int btn_y = height - 2;
    int yes_x = width / 2 - 10;
    int no_x = width / 2 + 4;

    if (selected == 0) {
      wattron(dialog, A_REVERSE);
      mvwprintw(dialog, btn_y, yes_x, "[ Yes ]");
      wattroff(dialog, A_REVERSE);
      mvwprintw(dialog, btn_y, no_x, "[ No ]");
    } else {
      mvwprintw(dialog, btn_y, yes_x, "[ Yes ]");
      wattron(dialog, A_REVERSE);
      mvwprintw(dialog, btn_y, no_x, "[ No ]");
      wattroff(dialog, A_REVERSE);
    }

    wrefresh(dialog);

    int ch = wgetch(dialog);
    switch (ch) {
    case KEY_LEFT:
    case KEY_RIGHT:
    case '\t':
    case 'h':
    case 'l':
      selected = 1 - selected;
      break;

    case 'y':
    case 'Y':
      delwin(dialog);
      touchwin(stdscr);
      return true;

    case 'n':
    case 'N':
    case 27: /* Escape */
      delwin(dialog);
      touchwin(stdscr);
      return false;

    case '\n':
    case KEY_ENTER:
      delwin(dialog);
      touchwin(stdscr);
      return (selected == 0);
    }
  }
}

/* Show go-to row dialog */
void tui_show_goto_dialog(TuiState *state) {
  if (!state)
    return;

  /* Determine if we're in a query tab with results */
  Workspace *ws = NULL;
  bool is_query = false;
  size_t total_rows = 0;

  if (state->num_workspaces > 0) {
    ws = &state->workspaces[state->current_workspace];
    if (ws->type == WORKSPACE_TYPE_QUERY && ws->query_results &&
        ws->query_results->num_rows > 0) {
      is_query = true;
      total_rows = ws->query_paginated ? ws->query_total_rows
                                       : ws->query_results->num_rows;
    }
  }

  /* Fall back to regular table data if not in query tab */
  if (!is_query) {
    if (!state->data)
      return;
    total_rows = state->total_rows;
  }

  if (total_rows == 0)
    return;

  int term_rows, term_cols;
  getmaxyx(stdscr, term_rows, term_cols);

  int height = 7;
  int width = 50;
  int starty = (term_rows - height) / 2;
  int startx = (term_cols - width) / 2;

  /* Clamp coordinates to prevent negative values */
  if (starty < 0)
    starty = 0;
  if (startx < 0)
    startx = 0;

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
    box(win, 0, 0);

    wattron(win, A_BOLD);
    mvwprintw(win, 0, (width - 14) / 2, " Go to Row ");
    wattroff(win, A_BOLD);

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
        long long parsed = strtoll(input, NULL, 10);
        if (parsed <= 0 || (unsigned long long)parsed > total_rows) {
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
            if (ws->query_paginated) {
              /* Check if target is in currently loaded range */
              if (target_row >= ws->query_loaded_offset &&
                  target_row <
                      ws->query_loaded_offset + ws->query_loaded_count) {
                /* Already loaded, just move cursor */
                ws->query_result_row = target_row - ws->query_loaded_offset;
              } else {
                /* Need to load new data - use async with progress */
                size_t load_offset =
                    target_row > PAGE_SIZE / 2 ? target_row - PAGE_SIZE / 2 : 0;
                query_load_rows_at(state, ws, load_offset);
                ws->query_result_row = target_row - ws->query_loaded_offset;
              }
            } else {
              /* Non-paginated - just move cursor */
              ws->query_result_row = target_row;
            }

            /* Adjust scroll */
            int win_rows = state->term_rows - 4;
            int editor_height = (win_rows - 1) * 3 / 10;
            if (editor_height < 3)
              editor_height = 3;
            int visible = win_rows - editor_height - 4;
            if (visible < 1)
              visible = 1;

            if (ws->query_result_row < ws->query_result_scroll_row) {
              ws->query_result_scroll_row = ws->query_result_row;
            } else if (ws->query_result_row >=
                       ws->query_result_scroll_row + (size_t)visible) {
              ws->query_result_scroll_row =
                  ws->query_result_row - (size_t)visible + 1;
            }

            /* Ensure focus is on results */
            ws->query_focus_results = true;
          } else {
            /* Handle regular table navigation */
            /* Check if target is in currently loaded range */
            if (target_row >= state->loaded_offset &&
                target_row < state->loaded_offset + state->loaded_count) {
              /* Already loaded, just move cursor */
              state->cursor_row = target_row - state->loaded_offset;

              /* Adjust scroll */
              if (state->cursor_row < state->scroll_row) {
                state->scroll_row = state->cursor_row;
              } else if (state->cursor_row >=
                         state->scroll_row + (size_t)state->content_rows) {
                state->scroll_row = state->cursor_row - state->content_rows + 1;
              }
            } else {
              /* Need to load new data - use async with progress */
              size_t load_offset =
                  target_row > PAGE_SIZE / 2 ? target_row - PAGE_SIZE / 2 : 0;

              /* Get table name */
              const char *table = state->tables[state->current_table];
              char *where_clause = NULL;

              /* Build WHERE clause from filters if applicable */
              if (state->num_workspaces > 0 &&
                  state->current_workspace < state->num_workspaces) {
                Workspace *curr_ws =
                    &state->workspaces[state->current_workspace];
                if (curr_ws->filters.num_filters > 0 && state->schema &&
                    state->conn) {
                  char *err = NULL;
                  where_clause = filters_build_where(
                      &curr_ws->filters, state->schema,
                      state->conn->driver->name, &err);
                  free(err);
                }
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
              if (op.table_name && async_start(&op)) {
                bool completed =
                    tui_show_processing_dialog(state, &op, "Loading data...");

                if (completed && op.state == ASYNC_STATE_COMPLETED &&
                    op.result) {
                  ResultSet *new_data = (ResultSet *)op.result;

                  /* Check if we got 0 rows with approximate count */
                  bool was_approximate = false;
                  if (state->num_workspaces > 0 &&
                      state->current_workspace < state->num_workspaces) {
                    was_approximate = state->workspaces[state->current_workspace]
                                          .row_count_approximate;
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
                        state->total_rows = (size_t)count_op.count;
                        if (state->num_workspaces > 0 &&
                            state->current_workspace < state->num_workspaces) {
                          Workspace *curr_ws =
                              &state->workspaces[state->current_workspace];
                          curr_ws->total_rows = state->total_rows;
                          curr_ws->row_count_approximate = false;
                        }

                        /* Clamp target row to actual data */
                        if (target_row >= state->total_rows) {
                          target_row =
                              state->total_rows > 0 ? state->total_rows - 1 : 0;
                        }

                        /* Recalculate load offset and reload */
                        load_offset = target_row > PAGE_SIZE / 2
                                          ? target_row - PAGE_SIZE / 2
                                          : 0;

                        /* Clean up count dialog and refresh before next dialog */
                        async_free(&count_op);
                        touchwin(stdscr);
                        tui_refresh(state);

                        /* Use tui_load_rows_at_with_dialog which handles edge cases */
                        if (tui_load_rows_at_with_dialog(state, load_offset)) {
                          state->cursor_row = target_row - state->loaded_offset;
                          if (state->cursor_row < state->scroll_row) {
                            state->scroll_row = state->cursor_row;
                          } else if (state->cursor_row >=
                                     state->scroll_row +
                                         (size_t)state->content_rows) {
                            state->scroll_row =
                                state->cursor_row - state->content_rows + 1;
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

                  /* Free old data but keep schema */
                  if (state->data) {
                    db_result_free(state->data);
                  }
                  state->data = new_data;
                  state->loaded_offset = load_offset;
                  state->loaded_count = state->data->num_rows;

                  /* Apply schema column names */
                  if (state->schema && state->data) {
                    size_t min_cols = state->schema->num_columns;
                    if (state->data->num_columns < min_cols) {
                      min_cols = state->data->num_columns;
                    }
                    for (size_t i = 0; i < min_cols; i++) {
                      if (state->schema->columns[i].name) {
                        free(state->data->columns[i].name);
                        state->data->columns[i].name =
                            str_dup(state->schema->columns[i].name);
                        state->data->columns[i].type =
                            state->schema->columns[i].type;
                      }
                    }
                  }

                  /* Update workspace pointers */
                  if (state->num_workspaces > 0 &&
                      state->current_workspace < state->num_workspaces) {
                    Workspace *curr_ws =
                        &state->workspaces[state->current_workspace];
                    curr_ws->data = state->data;
                    curr_ws->loaded_offset = state->loaded_offset;
                    curr_ws->loaded_count = state->loaded_count;
                  }

                  load_succeeded = true;
                }
              }
              async_free(&op);

              /* Only update cursor if load succeeded */
              if (load_succeeded) {
                /* Clamp cursor to actual loaded data if target was beyond */
                size_t actual_target = target_row - state->loaded_offset;
                if (actual_target >= state->data->num_rows) {
                  actual_target =
                      state->data->num_rows > 0 ? state->data->num_rows - 1 : 0;
                }
                state->cursor_row = actual_target;

                /* Adjust scroll */
                if (state->cursor_row < state->scroll_row) {
                  state->scroll_row = state->cursor_row;
                } else if (state->cursor_row >=
                           state->scroll_row + (size_t)state->content_rows) {
                  state->scroll_row =
                      state->cursor_row - state->content_rows + 1;
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
  if (!state || !state->schema) {
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
    box(schema_win, 0, 0);
    wattron(schema_win, A_BOLD);
    mvwprintw(schema_win, 0, 2, " Schema: %s ", state->schema->name);
    wattroff(schema_win, A_BOLD);

    int y = 2;
    int content_height = height - 4;
    int line = 0;

    /* Calculate total lines needed */
    int total_lines =
        2 + (int)state->schema->num_columns; /* Columns header + data */
    if (state->schema->num_indexes > 0) {
      total_lines += 2 + (int)state->schema->num_indexes; /* Indexes section */
    }
    if (state->schema->num_foreign_keys > 0) {
      total_lines += 2 + (int)state->schema->num_foreign_keys; /* FKs section */
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
    DRAW_LINE("Columns (%zu):", state->schema->num_columns);
    wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

    if (line >= scroll_offset && y < height - 2) {
      wattron(schema_win, A_BOLD);
      mvwprintw(schema_win, y++, 4, "%-20s %-15s %-8s %-8s %-8s", "Name",
                "Type", "Nullable", "PK", "AI");
      wattroff(schema_win, A_BOLD);
    }
    line++;

    for (size_t i = 0; i < state->schema->num_columns; i++) {
      ColumnDef *col = &state->schema->columns[i];
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
    if (state->schema->num_indexes > 0) {
      line++; /* blank line */
      if (line >= scroll_offset && y < height - 2)
        y++;

      wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
      DRAW_LINE("Indexes (%zu):", state->schema->num_indexes);
      wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

      for (size_t i = 0; i < state->schema->num_indexes; i++) {
        IndexDef *idx = &state->schema->indexes[i];
        if (line >= scroll_offset && y < height - 2) {
          /* Build column list string safely */
          char cols[256] = "";
          size_t pos = 0;
          for (size_t j = 0; j < idx->num_columns && pos < sizeof(cols) - 1;
               j++) {
            int written;
            if (j > 0) {
              written = snprintf(cols + pos, sizeof(cols) - pos, ", %s",
                                 idx->columns[j] ? idx->columns[j] : "");
            } else {
              written = snprintf(cols + pos, sizeof(cols) - pos, "%s",
                                 idx->columns[j] ? idx->columns[j] : "");
            }
            if (written > 0)
              pos += written;
            if (pos >= sizeof(cols) - 1)
              break;
          }
          mvwprintw(schema_win, y++, 4, "%s%-20s %s(%s)",
                    idx->unique ? "[U] " : "    ", idx->name ? idx->name : "",
                    idx->type ? idx->type : "", cols);
        }
        line++;
      }
    }

    /* Foreign Keys section */
    if (state->schema->num_foreign_keys > 0) {
      line++; /* blank line */
      if (line >= scroll_offset && y < height - 2)
        y++;

      wattron(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
      DRAW_LINE("Foreign Keys (%zu):", state->schema->num_foreign_keys);
      wattroff(schema_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));

      for (size_t i = 0; i < state->schema->num_foreign_keys; i++) {
        ForeignKeyDef *fk = &state->schema->foreign_keys[i];
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
            if (written > 0)
              pos += written;
            if (pos >= sizeof(src_cols) - 1)
              break;
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
            if (written > 0)
              pos += written;
            if (pos >= sizeof(ref_cols) - 1)
              break;
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
                "[Up/Down] Scroll  [q/Esc] Close  (%d/%d)", scroll_offset + 1,
                max_scroll + 1);
    } else {
      mvwprintw(schema_win, height - 2, 2, "[q/Esc] Close");
    }

    wrefresh(schema_win);

    int ch = wgetch(schema_win);
    switch (ch) {
    case 'q':
    case 'Q':
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
  char *connstr = connect_view_show(state);
  if (connstr) {
    tui_disconnect(state);
    if (tui_connect(state, connstr)) {
      tui_set_status(state, "Connected successfully");
    }
    free(connstr);
  }
  tui_refresh(state);
}

/* Show table selector dialog */
void tui_show_table_selector(TuiState *state) {
  if (!state || !state->tables || state->num_tables == 0) {
    tui_set_error(state, "No tables available");
    return;
  }

  int height = (int)state->num_tables + 4;
  if (height > state->term_rows - 4) {
    height = state->term_rows - 4;
  }
  /* Ensure minimum dimensions */
  if (height < 5)
    height = 5;
  int width = 40;
  int starty = (state->term_rows - height) / 2;
  int startx = (state->term_cols - width) / 2;

  /* Clamp coordinates to prevent negative values */
  if (starty < 0)
    starty = 0;
  if (startx < 0)
    startx = 0;

  WINDOW *menu_win = newwin(height, width, starty, startx);
  if (!menu_win)
    return;

  keypad(menu_win, TRUE);
  box(menu_win, 0, 0);

  wattron(menu_win, A_BOLD);
  mvwprintw(menu_win, 0, 2, " Select Table ");
  wattroff(menu_win, A_BOLD);

  /* Create menu items */
  ITEM **items = calloc(state->num_tables + 1, sizeof(ITEM *));
  if (!items) {
    delwin(menu_win);
    return;
  }

  for (size_t i = 0; i < state->num_tables; i++) {
    items[i] = new_item(state->tables[i], "");
  }
  items[state->num_tables] = NULL;

  MENU *menu = new_menu(items);
  if (!menu) {
    for (size_t i = 0; i < state->num_tables; i++) {
      free_item(items[i]);
    }
    free(items);
    delwin(menu_win);
    return;
  }

  /* Set menu options */
  set_menu_win(menu, menu_win);
  set_menu_sub(menu, derwin(menu_win, height - 4, width - 4, 2, 2));
  set_menu_mark(menu, "> ");
  set_menu_format(menu, height - 4, 1);

  /* Set current item */
  if (state->current_table < state->num_tables) {
    set_current_item(menu, items[state->current_table]);
  }

  post_menu(menu);
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
        if (idx >= 0 && (size_t)idx < state->num_tables) {
          state->current_table = idx;
          /* Clear filters when switching tables */
          if (state->num_workspaces > 0 &&
              state->current_workspace < state->num_workspaces) {
            Workspace *ws = &state->workspaces[state->current_workspace];
            if (ws->type == WORKSPACE_TYPE_TABLE) {
              filters_clear(&ws->filters);
            }
          }
          tui_load_table_data(state, state->tables[idx]);
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
  for (size_t i = 0; i < state->num_tables; i++) {
    free_item(items[i]);
  }
  free(items);
  delwin(menu_win);

  touchwin(stdscr);
  tui_refresh(state);
}

/* Show help dialog */
void tui_show_help(TuiState *state) {
  int height = 60;
  int width = 60;

  /* Constrain to terminal size */
  if (height > state->term_rows - 2)
    height = state->term_rows - 2;
  if (width > state->term_cols - 2)
    width = state->term_cols - 2;
  if (height < 10)
    height = 10;
  if (width < 30)
    width = 30;

  int starty = (state->term_rows - height) / 2;
  int startx = (state->term_cols - width) / 2;

  /* Clamp coordinates to prevent negative values */
  if (starty < 0)
    starty = 0;
  if (startx < 0)
    startx = 0;

  WINDOW *help_win = newwin(height, width, starty, startx);
  if (!help_win)
    return;

  keypad(help_win, TRUE);
  box(help_win, 0, 0);
  wattron(help_win, A_BOLD);
  mvwprintw(help_win, 0, (width - 8) / 2, " Help ");
  wattroff(help_win, A_BOLD);

  int y = 2;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Navigation");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "Arrow keys / hjkl  Move cursor");
  mvwprintw(help_win, y++, 4, "PgUp / PgDown      Page up/down");
  mvwprintw(help_win, y++, 4, "Home / End         First/last column");
  mvwprintw(help_win, y++, 4, "a                  Go to first row");
  mvwprintw(help_win, y++, 4, "z                  Go to last row");
  mvwprintw(help_win, y++, 4, "g (or Ctrl+G, F5)  Go to row number");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Editing");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "Enter              Edit cell (inline)");
  mvwprintw(help_win, y++, 4, "e (or F4)          Edit cell (modal)");
  mvwprintw(help_win, y++, 4, "n (or Ctrl+N)      Set cell to NULL");
  mvwprintw(help_win, y++, 4, "d (or Ctrl+D)      Set cell to empty");
  mvwprintw(help_win, y++, 4, "x (or Delete)      Delete row");
  mvwprintw(help_win, y++, 4, "Escape             Cancel editing");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Tabs");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "[ / ] (or F7/F6)   Previous/next tab");
  mvwprintw(help_win, y++, 4, "- / _              Close current tab");
  mvwprintw(help_win, y++, 4, "+                  Open table in new tab");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Query Tab");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "p                  Perform query");
  mvwprintw(help_win, y++, 4, "Ctrl+R             Execute query at cursor");
  mvwprintw(help_win, y++, 4, "Ctrl+A             Execute all queries");
  mvwprintw(help_win, y++, 4, "Ctrl+T             Execute all in transaction");
  mvwprintw(help_win, y++, 4, "Ctrl+W / Esc       Switch editor/results");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Sidebar");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "t (or F9)          Toggle sidebar");
  mvwprintw(help_win, y++, 4, "/                  Filter tables (sidebar)");
  mvwprintw(help_win, y++, 4, "Enter              Select table");
  mvwprintw(help_win, y++, 4, "Left/Right         Focus sidebar/table");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Table Filters");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "/ (or f)           Toggle filters panel");
  mvwprintw(help_win, y++, 4, "Arrow keys / hjkl  Navigate (spatial)");
  mvwprintw(help_win, y++, 4, "Ctrl+W             Switch filters/table focus");
  mvwprintw(help_win, y++, 4, "Enter              Edit field (auto-applies)");
  mvwprintw(help_win, y++, 4, "+ / =              Add new filter");
  mvwprintw(help_win, y++, 4, "- / x / Delete     Remove filter");
  mvwprintw(help_win, y++, 4, "c                  Clear all filters");
  mvwprintw(help_win, y++, 4, "Escape             Close panel");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Other");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "s (or F3)          Show table schema");
  mvwprintw(help_win, y++, 4, "c (or F2)          Connect dialog");
  mvwprintw(help_win, y++, 4, "m                  Toggle menu bar");
  mvwprintw(help_win, y++, 4, "b                  Toggle status bar");
  mvwprintw(help_win, y++, 4, "? (or F1)          This help");
  mvwprintw(help_win, y++, 4, "q (or Ctrl+X, F10) Quit");
  y++;

  wattron(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 2, "Mouse");
  wattroff(help_win, A_BOLD | COLOR_PAIR(COLOR_HEADER));
  mvwprintw(help_win, y++, 4, "Click              Select cell/table");
  mvwprintw(help_win, y++, 4, "Double-click       Edit cell");
  mvwprintw(help_win, y++, 4, "Scroll             Navigate rows");

  /* Close button */
  wattron(help_win, A_REVERSE);
  mvwprintw(help_win, height - 2, (width - 9) / 2, "[ Close ]");
  wattroff(help_win, A_REVERSE);
  wrefresh(help_win);

  wgetch(help_win);
  delwin(help_win);

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

      int start_y = (term_rows - height) / 2;
      int start_x = (term_cols - width) / 2;

      if (start_y < 0)
        start_y = 0;
      if (start_x < 0)
        start_x = 0;

      dialog = newwin(height, width, start_y, start_x);
      if (dialog) {
        keypad(dialog, TRUE);
        wtimeout(dialog, POLL_INTERVAL_MS);
      }
    }

    if (dialog) {
      /* Draw dialog */
      werase(dialog);
      box(dialog, 0, 0);

      /* Title */
      wattron(dialog, A_BOLD);
      mvwprintw(dialog, 0, (width - 14) / 2, " Processing ");
      wattroff(dialog, A_BOLD);

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

  if (!op.connstr) {
    tui_set_error(state, "Memory allocation failed");
    return NULL;
  }

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start connection");
    free(op.connstr);
    return NULL;
  }

  /* Connection dialog shows immediately (no delay) */
  bool completed = tui_show_processing_dialog_ex(state, &op, "Connecting...", 0);

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

    /* Also update app state if available */
    if (state->app) {
      state->app->tables = state->tables;
      state->app->num_tables = state->num_tables;
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

  if (!op.table_name) {
    tui_set_error(state, "Memory allocation failed");
    return -1;
  }

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

  if (!op.table_name) {
    tui_set_error(state, "Memory allocation failed");
    return NULL;
  }

  if (!async_start(&op)) {
    tui_set_error(state, "Failed to start operation");
    free(op.table_name);
    return NULL;
  }

  bool completed =
      tui_show_processing_dialog(state, &op, "Loading schema...");

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

  if (!op.table_name) {
    tui_set_error(state, "Memory allocation failed");
    return NULL;
  }

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
