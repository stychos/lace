/*
 * Lace ncurses frontend
 * Query tab - SQL editor with results
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query.h"
#include "app.h"
#include <lace.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ==========================================================================
 * Line Cache Helper
 * ========================================================================== */

typedef struct {
  size_t start;  /* Start offset in text */
  size_t len;    /* Line length (excluding newline) */
} LineInfo;

static void build_line_cache(const char *text, size_t text_len,
                             LineInfo **lines, size_t *num_lines) {
  /* Count lines */
  size_t count = 1;
  for (size_t i = 0; i < text_len; i++) {
    if (text[i] == '\n') count++;
  }

  *lines = calloc(count, sizeof(LineInfo));
  *num_lines = count;
  if (!*lines) return;

  size_t line_idx = 0;
  size_t line_start = 0;
  for (size_t i = 0; i <= text_len; i++) {
    if (i == text_len || text[i] == '\n') {
      (*lines)[line_idx].start = line_start;
      (*lines)[line_idx].len = i - line_start;
      line_idx++;
      line_start = i + 1;
    }
  }
}

/* ==========================================================================
 * Query State Functions
 * ========================================================================== */

void query_init(QueryState *qs) {
  memset(qs, 0, sizeof(QueryState));
  qs->text_cap = QUERY_INITIAL_CAPACITY;
  qs->text = calloc(qs->text_cap, 1);
  qs->text_len = 0;
  qs->cursor = 0;
}

void query_free(QueryState *qs) {
  if (!qs) return;
  free(qs->text);
  free(qs->error);
  free(qs->base_sql);
  free(qs->col_widths);
  if (qs->results) {
    lace_result_free(qs->results);
  }
  memset(qs, 0, sizeof(QueryState));
}

/* ==========================================================================
 * Text Editing
 * ========================================================================== */

static bool ensure_capacity(QueryState *qs, size_t needed) {
  if (needed <= qs->text_cap) return true;

  size_t new_cap = qs->text_cap * 2;
  while (new_cap < needed) new_cap *= 2;

  char *new_text = realloc(qs->text, new_cap);
  if (!new_text) return false;

  qs->text = new_text;
  qs->text_cap = new_cap;
  return true;
}

void query_insert_char(QueryState *qs, char c) {
  if (!ensure_capacity(qs, qs->text_len + 2)) return;

  /* Make room for new character */
  memmove(qs->text + qs->cursor + 1,
          qs->text + qs->cursor,
          qs->text_len - qs->cursor + 1);

  qs->text[qs->cursor] = c;
  qs->cursor++;
  qs->text_len++;
}

void query_backspace(QueryState *qs) {
  if (qs->cursor == 0) return;

  memmove(qs->text + qs->cursor - 1,
          qs->text + qs->cursor,
          qs->text_len - qs->cursor + 1);

  qs->cursor--;
  qs->text_len--;
}

void query_delete_char(QueryState *qs) {
  if (qs->cursor >= qs->text_len) return;

  memmove(qs->text + qs->cursor,
          qs->text + qs->cursor + 1,
          qs->text_len - qs->cursor);

  qs->text_len--;
}

void query_cursor_to_line_col(QueryState *qs, size_t *line, size_t *col) {
  *line = 0;
  *col = 0;

  size_t line_start = 0;
  for (size_t i = 0; i < qs->cursor && i < qs->text_len; i++) {
    if (qs->text[i] == '\n') {
      (*line)++;
      line_start = i + 1;
    }
  }
  *col = qs->cursor - line_start;
}

void query_set_cursor_line_col(QueryState *qs, size_t line, size_t col) {
  LineInfo *lines = NULL;
  size_t num_lines = 0;
  build_line_cache(qs->text, qs->text_len, &lines, &num_lines);

  if (!lines || num_lines == 0) {
    qs->cursor = 0;
    free(lines);
    return;
  }

  if (line >= num_lines) line = num_lines - 1;

  size_t max_col = lines[line].len;
  if (col > max_col) col = max_col;

  qs->cursor = lines[line].start + col;
  free(lines);
}

/* ==========================================================================
 * Query Execution
 * ========================================================================== */

/* Find query at cursor position (between semicolons) */
static char *find_query_at_cursor(const char *text, size_t cursor) {
  if (!text || !*text) return NULL;

  size_t len = strlen(text);
  if (cursor > len) cursor = len;

  /* Find start - search backward for semicolon or start */
  size_t start = cursor;
  while (start > 0) {
    if (text[start - 1] == ';') break;
    start--;
  }

  /* Find end - search forward for semicolon or end */
  size_t end = cursor;
  bool in_string = false;
  char quote = 0;
  for (size_t i = start; i < len; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote) in_string = false;
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote = c;
      } else if (c == ';') {
        end = i;
        break;
      }
    }
    end = i + 1;
  }

  /* Trim whitespace */
  while (start < end && isspace((unsigned char)text[start])) start++;
  while (end > start && isspace((unsigned char)text[end - 1])) end--;

  if (start >= end) return NULL;

  char *query = malloc(end - start + 1);
  if (!query) return NULL;

  memcpy(query, text + start, end - start);
  query[end - start] = '\0';
  return query;
}

bool query_execute(TuiState *tui, QueryState *qs, const char *sql) {
  if (!tui || !tui->app || !qs || !sql || !*sql) return false;

  Tab *tab = app_current_tab(tui->app);
  if (!tab) return false;

  /* Clear previous results/error */
  free(qs->error);
  qs->error = NULL;
  if (qs->results) {
    lace_result_free(qs->results);
    qs->results = NULL;
  }
  free(qs->col_widths);
  qs->col_widths = NULL;
  qs->exec_success = false;
  qs->affected_rows = 0;

  /* Execute via lace client */
  LaceResult *result = NULL;
  int err = lace_exec(tui->app->client, tab->conn_id, sql, &result);

  if (err != LACE_OK) {
    qs->error = strdup(lace_client_error(tui->app->client));
    app_set_error(tui->app, qs->error ? qs->error : "Query failed");
    return false;
  }

  if (result) {
    qs->results = result;
    qs->result_row = 0;
    qs->result_col = 0;
    qs->result_scroll_row = 0;
    qs->result_scroll_col = 0;

    /* Calculate column widths */
    if (result->num_columns > 0) {
      qs->col_widths = calloc(result->num_columns, sizeof(int));
      for (size_t c = 0; c < result->num_columns; c++) {
        int w = result->columns[c].name ?
                (int)strlen(result->columns[c].name) : 4;
        if (w < 4) w = 4;

        /* Check data widths (first 100 rows) */
        size_t check = result->num_rows < 100 ? result->num_rows : 100;
        for (size_t r = 0; r < check; r++) {
          LaceValue *v = &result->rows[r].cells[c];
          int vw = 0;
          if (v->type == LACE_TYPE_TEXT && v->text.data) {
            vw = (int)strlen(v->text.data);
          } else if (v->type == LACE_TYPE_INT) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)v->int_val);
            vw = (int)strlen(buf);
          } else if (v->type == LACE_TYPE_FLOAT) {
            vw = 12;
          }
          if (vw > w) w = vw;
        }
        if (w > 40) w = 40;
        qs->col_widths[c] = w;
      }
    }

    char status[128];
    snprintf(status, sizeof(status), "Query returned %zu rows", result->num_rows);
    app_set_status(tui->app, status);
  } else {
    qs->exec_success = true;
    app_set_status(tui->app, "Statement executed successfully");
  }

  return true;
}

bool query_execute_at_cursor(TuiState *tui, QueryState *qs) {
  if (!qs || !qs->text) return false;

  char *sql = find_query_at_cursor(qs->text, qs->cursor);
  if (!sql || !*sql) {
    app_set_error(tui->app, "No query at cursor");
    free(sql);
    return false;
  }

  bool ok = query_execute(tui, qs, sql);
  free(sql);
  return ok;
}

bool query_execute_all(TuiState *tui, QueryState *qs) {
  if (!qs || !qs->text || !qs->text_len) {
    app_set_error(tui->app, "No queries to execute");
    return false;
  }

  /* Execute all queries separated by semicolons */
  char *text = strdup(qs->text);
  if (!text) return false;

  char *p = text;
  int count = 0;
  int errors = 0;

  while (p && *p) {
    /* Skip whitespace */
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;

    /* Find end of query */
    char *end = p;
    bool in_string = false;
    char quote = 0;
    while (*end) {
      if (in_string) {
        if (*end == quote) in_string = false;
      } else {
        if (*end == '\'' || *end == '"') {
          in_string = true;
          quote = *end;
        } else if (*end == ';') {
          break;
        }
      }
      end++;
    }

    char saved = *end;
    *end = '\0';

    /* Trim */
    char *q = p;
    while (*q && isspace((unsigned char)*q)) q++;
    size_t qlen = strlen(q);
    while (qlen > 0 && isspace((unsigned char)q[qlen - 1])) qlen--;

    if (qlen > 0) {
      char *query = malloc(qlen + 1);
      if (query) {
        memcpy(query, q, qlen);
        query[qlen] = '\0';
        if (!query_execute(tui, qs, query)) {
          errors++;
        }
        count++;
        free(query);
      }
    }

    *end = saved;
    p = *end ? end + 1 : end;
  }

  free(text);

  if (errors > 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Executed %d queries, %d errors", count, errors);
    app_set_error(tui->app, msg);
  } else {
    char msg[128];
    snprintf(msg, sizeof(msg), "Executed %d queries", count);
    app_set_status(tui->app, msg);
  }

  return errors == 0;
}

/* ==========================================================================
 * Drawing
 * ========================================================================== */

void query_draw(TuiState *tui, QueryState *qs, WINDOW *win) {
  if (!tui || !qs || !win) return;

  werase(win);

  int win_rows, win_cols;
  getmaxyx(win, win_rows, win_cols);

  /* Split: editor on top (30%), results on bottom */
  int editor_height = (win_rows - 1) * 3 / 10;
  if (editor_height < 3) editor_height = 3;
  int results_start = editor_height + 1;
  int results_height = win_rows - results_start;

  /* Build line cache */
  LineInfo *lines = NULL;
  size_t num_lines = 0;
  build_line_cache(qs->text, qs->text_len, &lines, &num_lines);

  /* Get cursor line/col */
  size_t cursor_line, cursor_col;
  query_cursor_to_line_col(qs, &cursor_line, &cursor_col);

  /* Adjust scroll */
  if (cursor_line < qs->scroll_line) {
    qs->scroll_line = cursor_line;
  } else if (cursor_line >= qs->scroll_line + (size_t)(editor_height - 1)) {
    qs->scroll_line = cursor_line - editor_height + 2;
  }

  /* Draw editor header */
  if (!qs->focus_results) {
    wattron(win, A_BOLD);
  }
  mvwprintw(win, 0, 1, "SQL Query (Ctrl+R: run, Ctrl+A: all, Ctrl+W: switch)");
  wattroff(win, A_BOLD);

  /* Draw editor lines */
  for (int y = 1; y < editor_height; y++) {
    size_t line_idx = qs->scroll_line + (size_t)(y - 1);
    if (line_idx >= num_lines) break;

    LineInfo *li = &lines[line_idx];

    /* Line number */
    wattron(win, A_DIM);
    mvwprintw(win, y, 0, "%3zu", line_idx + 1);
    wattroff(win, A_DIM);

    /* Line content */
    int x = 4;
    for (size_t i = 0; i < li->len && x < win_cols - 1; i++) {
      char c = qs->text[li->start + i];
      if (c == '\t') {
        for (int t = 0; t < 4 && x < win_cols - 1; t++) {
          mvwaddch(win, y, x++, ' ');
        }
      } else if (c >= 32 && c < 127) {
        mvwaddch(win, y, x++, c);
      }
    }

    /* Draw cursor if editor focused and on this line */
    if (!qs->focus_results && line_idx == cursor_line) {
      int cursor_x = 4 + (int)cursor_col;
      if (cursor_x < win_cols) {
        char cursor_char = ' ';
        if (cursor_col < li->len) {
          cursor_char = qs->text[li->start + cursor_col];
          if (cursor_char < 32 || cursor_char >= 127) cursor_char = ' ';
        }
        wattron(win, A_REVERSE);
        mvwaddch(win, y, cursor_x, cursor_char);
        wattroff(win, A_REVERSE);
      }
    }
  }

  free(lines);

  /* Draw separator */
  wattron(win, COLOR_PAIR(COLOR_BORDER));
  mvwhline(win, editor_height, 0, ACS_HLINE, win_cols);
  wattroff(win, COLOR_PAIR(COLOR_BORDER));

  /* Draw results area */
  if (qs->error) {
    wattron(win, COLOR_PAIR(COLOR_ERROR));
    mvwprintw(win, results_start, 1, "Error: %.60s", qs->error);
    wattroff(win, COLOR_PAIR(COLOR_ERROR));
  } else if (qs->results && qs->results->num_columns > 0) {
    LaceResult *res = qs->results;

    /* Draw header */
    if (qs->focus_results) {
      wattron(win, A_BOLD);
    }
    int x = 1;
    for (size_t c = qs->result_scroll_col;
         c < res->num_columns && x < win_cols - 1; c++) {
      int w = qs->col_widths ? qs->col_widths[c] : 15;
      const char *name = res->columns[c].name ? res->columns[c].name : "?";
      mvwprintw(win, results_start, x, "%-*.*s", w, w, name);
      x += w + 1;
    }
    wattroff(win, A_BOLD);

    /* Draw rows */
    int visible_rows = results_height - 2;
    for (int r = 0; r < visible_rows; r++) {
      size_t row_idx = qs->result_scroll_row + (size_t)r;
      if (row_idx >= res->num_rows) break;

      int y = results_start + 1 + r;
      x = 1;

      /* Highlight cursor row */
      bool is_cursor_row = (row_idx == qs->result_row);
      if (is_cursor_row && qs->focus_results) {
        wattron(win, COLOR_PAIR(COLOR_SELECTED));
      }

      for (size_t c = qs->result_scroll_col;
           c < res->num_columns && x < win_cols - 1; c++) {
        int w = qs->col_widths ? qs->col_widths[c] : 15;
        LaceValue *v = &res->rows[row_idx].cells[c];

        /* Highlight cursor cell */
        bool is_cursor = is_cursor_row && (c == qs->result_col);
        if (is_cursor && qs->focus_results) {
          wattron(win, A_REVERSE);
        }

        char buf[256] = "";
        if (v->is_null || v->type == LACE_TYPE_NULL) {
          wattron(win, COLOR_PAIR(COLOR_NULL));
          snprintf(buf, sizeof(buf), "NULL");
        } else {
          switch (v->type) {
          case LACE_TYPE_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)v->int_val);
            break;
          case LACE_TYPE_FLOAT:
            snprintf(buf, sizeof(buf), "%.6g", v->float_val);
            break;
          case LACE_TYPE_TEXT:
            snprintf(buf, sizeof(buf), "%s", v->text.data ? v->text.data : "");
            break;
          case LACE_TYPE_BOOL:
            snprintf(buf, sizeof(buf), "%s", v->bool_val ? "true" : "false");
            break;
          default:
            snprintf(buf, sizeof(buf), "?");
            break;
          }
        }

        mvwprintw(win, y, x, "%-*.*s", w, w, buf);

        if (v->is_null || v->type == LACE_TYPE_NULL) {
          wattroff(win, COLOR_PAIR(COLOR_NULL));
        }
        if (is_cursor && qs->focus_results) {
          wattroff(win, A_REVERSE);
        }

        x += w + 1;
      }

      if (is_cursor_row && qs->focus_results) {
        wattroff(win, COLOR_PAIR(COLOR_SELECTED));
      }
    }
  } else if (qs->exec_success) {
    wattron(win, COLOR_PAIR(COLOR_STATUS));
    mvwprintw(win, results_start + 1, 1, "Statement executed successfully");
    wattroff(win, COLOR_PAIR(COLOR_STATUS));
  } else {
    wattron(win, A_DIM);
    mvwprintw(win, results_start + 1, 1,
              "Enter SQL and press Ctrl+R to execute");
    wattroff(win, A_DIM);
  }

  wrefresh(win);
}

/* ==========================================================================
 * Input Handling
 * ========================================================================== */

bool query_handle_input(TuiState *tui, QueryState *qs, int ch) {
  if (!tui || !qs) return false;

  /* Ctrl+W - toggle focus */
  if (ch == 23) { /* Ctrl+W */
    qs->focus_results = !qs->focus_results;
    tui->app->needs_redraw = true;
    return true;
  }

  /* Handle results navigation when focused on results */
  if (qs->focus_results) {
    if (!qs->results || qs->results->num_rows == 0) {
      /* No results - arrow up switches to editor */
      if (ch == KEY_UP || ch == 'k') {
        qs->focus_results = false;
        tui->app->needs_redraw = true;
        return true;
      }
      return false;
    }

    switch (ch) {
    case KEY_UP:
    case 'k':
      if (qs->result_row > 0) {
        qs->result_row--;
        if (qs->result_row < qs->result_scroll_row) {
          qs->result_scroll_row = qs->result_row;
        }
      } else {
        /* At top - switch to editor */
        qs->focus_results = false;
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_DOWN:
    case 'j':
      if (qs->result_row < qs->results->num_rows - 1) {
        qs->result_row++;
        /* Calculate visible rows */
        int win_rows, win_cols;
        getmaxyx(tui->main_win, win_rows, win_cols);
        (void)win_cols;
        int editor_height = (win_rows - 1) * 3 / 10;
        if (editor_height < 3) editor_height = 3;
        int visible = win_rows - editor_height - 3;
        if (visible < 1) visible = 1;
        if (qs->result_row >= qs->result_scroll_row + (size_t)visible) {
          qs->result_scroll_row = qs->result_row - visible + 1;
        }
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_LEFT:
    case 'h':
      if (qs->result_col > 0) {
        qs->result_col--;
        if (qs->result_col < qs->result_scroll_col) {
          qs->result_scroll_col = qs->result_col;
        }
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_RIGHT:
    case 'l':
      if (qs->result_col < qs->results->num_columns - 1) {
        qs->result_col++;
        /* TODO: adjust horizontal scroll */
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_HOME:
      qs->result_col = 0;
      qs->result_scroll_col = 0;
      tui->app->needs_redraw = true;
      return true;

    case KEY_END:
      if (qs->results->num_columns > 0) {
        qs->result_col = qs->results->num_columns - 1;
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_PPAGE:
      if (qs->result_row > 10) {
        qs->result_row -= 10;
      } else {
        qs->result_row = 0;
      }
      if (qs->result_row < qs->result_scroll_row) {
        qs->result_scroll_row = qs->result_row;
      }
      tui->app->needs_redraw = true;
      return true;

    case KEY_NPAGE:
      qs->result_row += 10;
      if (qs->result_row >= qs->results->num_rows) {
        qs->result_row = qs->results->num_rows > 0 ?
                         qs->results->num_rows - 1 : 0;
      }
      tui->app->needs_redraw = true;
      return true;

    case 18: /* Ctrl+R - refresh */
      query_execute_at_cursor(tui, qs);
      return true;
    }

    return false;
  }

  /* Editor input handling */

  /* Ctrl+R - execute query at cursor */
  if (ch == 18) {
    query_execute_at_cursor(tui, qs);
    return true;
  }

  /* Ctrl+A - execute all */
  if (ch == 1) {
    query_execute_all(tui, qs);
    return true;
  }

  /* Arrow keys */
  switch (ch) {
  case KEY_UP: {
    size_t line, col;
    query_cursor_to_line_col(qs, &line, &col);
    if (line > 0) {
      query_set_cursor_line_col(qs, line - 1, col);
    }
    tui->app->needs_redraw = true;
    return true;
  }

  case KEY_DOWN: {
    size_t line, col;
    query_cursor_to_line_col(qs, &line, &col);
    query_set_cursor_line_col(qs, line + 1, col);
    /* If we couldn't move down and have results, switch focus */
    size_t new_line, new_col;
    query_cursor_to_line_col(qs, &new_line, &new_col);
    if (new_line == line && qs->results && qs->results->num_rows > 0) {
      qs->focus_results = true;
    }
    tui->app->needs_redraw = true;
    return true;
  }

  case KEY_LEFT:
    if (qs->cursor > 0) {
      qs->cursor--;
    }
    tui->app->needs_redraw = true;
    return true;

  case KEY_RIGHT:
    if (qs->cursor < qs->text_len) {
      qs->cursor++;
    }
    tui->app->needs_redraw = true;
    return true;

  case KEY_HOME: {
    size_t line, col;
    query_cursor_to_line_col(qs, &line, &col);
    query_set_cursor_line_col(qs, line, 0);
    tui->app->needs_redraw = true;
    return true;
  }

  case KEY_END: {
    /* Move to end of current line */
    while (qs->cursor < qs->text_len && qs->text[qs->cursor] != '\n') {
      qs->cursor++;
    }
    tui->app->needs_redraw = true;
    return true;
  }

  case KEY_PPAGE: {
    size_t line, col;
    query_cursor_to_line_col(qs, &line, &col);
    if (line > 10) {
      query_set_cursor_line_col(qs, line - 10, col);
    } else {
      query_set_cursor_line_col(qs, 0, col);
    }
    tui->app->needs_redraw = true;
    return true;
  }

  case KEY_NPAGE: {
    size_t line, col;
    query_cursor_to_line_col(qs, &line, &col);
    query_set_cursor_line_col(qs, line + 10, col);
    tui->app->needs_redraw = true;
    return true;
  }

  case KEY_BACKSPACE:
  case 127:
  case 8:
    query_backspace(qs);
    tui->app->needs_redraw = true;
    return true;

  case KEY_DC: /* Delete */
    query_delete_char(qs);
    tui->app->needs_redraw = true;
    return true;

  case '\n':
  case KEY_ENTER:
    query_insert_char(qs, '\n');
    tui->app->needs_redraw = true;
    return true;

  case '\t':
    /* Insert tab as spaces */
    for (int i = 0; i < 4; i++) {
      query_insert_char(qs, ' ');
    }
    tui->app->needs_redraw = true;
    return true;

  default:
    /* Printable characters */
    if (ch >= 32 && ch < 127) {
      query_insert_char(qs, (char)ch);
      tui->app->needs_redraw = true;
      return true;
    }
    break;
  }

  return false;
}
