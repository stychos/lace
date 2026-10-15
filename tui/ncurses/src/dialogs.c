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

/* Draw cells left to right, each padded to its display width, clipped to
 * max_width columns in all */
static void schema_draw_cells(WINDOW *win, int y, int x, int max_width,
                              const char *const *cells, const int *widths,
                              int n) {
  int used = 0;
  for (int i = 0; i < n && used < max_width; i++) {
    int w = widths[i] < max_width - used ? widths[i] : max_width - used;
    tui_print_fit(win, y, x + used, w, cells[i]);
    used += widths[i] + 1;
  }
}

/* "a, b, c" (caller frees) */
static char *schema_join(char **names, size_t n) {
  StringBuilder *sb = sb_new(64);
  for (size_t j = 0; j < n; j++)
    sb_printf(sb, "%s%s", j ? ", " : "", names[j] ? names[j] : "");
  return sb_finish(sb);
}

/* Show schema dialog */
void tui_show_schema(TuiState *state) {
  /* Get schema via VmTable if available, fallback to Tab */
  VmTable *vm = tui_vm_table(state);
  Tab *schema_tab = TUI_TAB(state);
  const TableSchema *schema = vm ? table_vm_get_schema(vm) : (schema_tab ? schema_tab->schema : NULL);

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
    {
      char *title = str_printf(" Schema: %s ", schema->name ? schema->name : "");
      WITH_ATTR(schema_win, A_BOLD,
                tui_print_clip(schema_win, 0, 2, width - 4, title ? title : ""));
      free(title);
    }

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

    /* Column table: cells padded and clipped by display width */
    static const int widths[] = {20, 15, 8, 8, 8};
    if (line >= scroll_offset && y < height - 2) {
      const char *head[] = {"Name", "Type", "Nullable", "PK", "AI"};
      wattron(schema_win, A_BOLD);
      schema_draw_cells(schema_win, y++, 4, width - 6, head, widths, 5);
      wattroff(schema_win, A_BOLD);
    }
    line++;

    for (size_t i = 0; i < schema->num_columns; i++) {
      const ColumnDef *col = &schema->columns[i];
      if (line >= scroll_offset && y < height - 2) {
        const char *cells[] = {
            col->name ? col->name : "",
            col->type_name ? col->type_name : db_value_type_name(col->type),
            col->nullable ? "YES" : "NO", col->primary_key ? "YES" : "",
            col->auto_increment ? "YES" : ""};
        schema_draw_cells(schema_win, y++, 4, width - 6, cells, widths, 5);
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
          char *cols = schema_join(idx->columns, idx->num_columns);
          char *text = str_printf("%s%s %s(%s)", idx->unique ? "[U] " : "    ",
                                  idx->name ? idx->name : "",
                                  idx->type ? idx->type : "", cols ? cols : "");
          tui_print_clip(schema_win, y++, 4, width - 6, text ? text : "");
          free(text);
          free(cols);
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
          char *src = schema_join(fk->columns, fk->num_columns);
          char *ref = schema_join(fk->ref_columns, fk->num_ref_columns);
          char *text = str_printf("(%s) -> %s(%s)", src ? src : "",
                                  fk->ref_table ? fk->ref_table : "?",
                                  ref ? ref : "");
          tui_print_clip(schema_win, y++, 4, width - 6, text ? text : "");
          free(text);
          free(src);
          free(ref);
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

    UiEvent event;
    render_translate_key(wgetch(schema_win), &event);
    const Config *cfg = state->app->config;
    if (render_event_is_special(&event, UI_KEY_ESCAPE) ||
        render_event_get_char(&event) == 'q') {
      running = false;
    } else if (hotkey_matches(cfg, &event, HOTKEY_MOVE_UP)) {
      if (scroll_offset > 0)
        scroll_offset--;
    } else if (hotkey_matches(cfg, &event, HOTKEY_MOVE_DOWN)) {
      if (scroll_offset < max_scroll)
        scroll_offset++;
    } else if (hotkey_matches(cfg, &event, HOTKEY_PAGE_UP)) {
      scroll_offset -= content_height / 2;
      if (scroll_offset < 0)
        scroll_offset = 0;
    } else if (hotkey_matches(cfg, &event, HOTKEY_PAGE_DOWN)) {
      scroll_offset += content_height / 2;
      if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;
    } else if (hotkey_matches(cfg, &event, HOTKEY_FIRST_ROW)) {
      scroll_offset = 0;
    } else if (hotkey_matches(cfg, &event, HOTKEY_LAST_ROW)) {
      scroll_offset = max_scroll;
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
    if (result.conn)
      db_disconnect(result.conn);
    str_secure_free(result.connstr); /* Connection string may contain password */
    free(result.saved_conn_id);
    state->running = false;
    state->app->running = false;
    return;
  }

  if (result.mode == CONNECT_MODE_CANCELLED || !result.connstr) {
    if (result.conn)
      db_disconnect(result.conn);
    str_secure_free(result.connstr); /* Connection string may contain password */
    free(result.saved_conn_id);
    tui_refresh(state);
    return;
  }

  if (result.mode == CONNECT_MODE_NEW_WORKSPACE) {
    /* Create a NEW workspace and connect (keep existing workspaces) */
    char *err = NULL;
    /* The dialog has connected already: never a second time */
    DbConnection *conn = result.conn ? result.conn
                                     : tui_connect_with_progress(state, result.connstr);
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

    /* Check if we should auto-open the first table */
    bool auto_open = state->app->config &&
                     state->app->config->general.auto_open_first_table &&
                     app_conn->num_tables > 0;

    if (auto_open) {
      /* Create a table tab with the first table */
      size_t table_idx = 0;
      Tab *tab = workspace_create_table_tab(ws, conn_index, app_conn->tables[table_idx]);
      if (tab) {
        tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                   ws->current_tab);

        /* Load table data (updates Tab directly) */
        tui_load_table_data(state, app_conn->tables[table_idx]);

        /* Initialize sidebar state (global) */
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
        /* Show sidebar for table selection (global state) */
        state->sidebar_visible = true;
        state->sidebar_focused = true;
        state->sidebar_highlight = 0;
        state->sidebar_scroll = 0;
        tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                   ws->current_tab);

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
    /* The dialog has connected already: never a second time */
    DbConnection *conn = result.conn ? result.conn
                                     : tui_connect_with_progress(state, result.connstr);
    if (!conn) {
      /* Error already shown by tui_connect_with_progress */
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

      /* Check if we should auto-open the first table */
      bool auto_open = state->app->config &&
                       state->app->config->general.auto_open_first_table &&
                       app_conn->num_tables > 0;

      if (auto_open) {
        /* Create a table tab with the first table */
        size_t table_idx = 0;
        Tab *tab = workspace_create_table_tab(ws, conn_index, app_conn->tables[table_idx]);
        if (tab) {
          tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                     ws->current_tab);

          /* Load table data (updates Tab directly) */
          tui_load_table_data(state, app_conn->tables[table_idx]);

          /* Initialize sidebar state (global) */
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
          /* Initialize sidebar state (global) and per-tab UI state */
          tui_ensure_tab_ui_capacity(state, state->app->current_workspace,
                                     ws->current_tab);
          UITabState *ui = TUI_TAB_UI(state);
          if (ui) {
            ui->filters_visible = false;
            ui->filters_focused = false;
          }
          state->sidebar_visible = true;
          state->sidebar_focused = true;
          state->sidebar_highlight = 0;
          state->sidebar_scroll = 0;

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

  str_secure_free(result.connstr); /* May hold a password typed in the dialog */
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

