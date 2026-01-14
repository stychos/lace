/*
 * Lace
 * Query tab text editor functions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_internal.h"
#include "../../util/mem.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Ensure query buffer has enough capacity. Returns false on allocation failure.
 */
bool query_ensure_capacity(Tab *tab, size_t needed) {
  /* Reject unreasonably large queries */
  if (needed > QUERY_MAX_SIZE)
    return false;

  return str_buf_ensure_capacity(&tab->query_text, &tab->query_capacity, needed,
                                 QUERY_INITIAL_CAPACITY);
}

/* Rebuild line cache from query text */
void query_rebuild_line_cache(Tab *tab, QueryLineInfo **lines,
                              size_t *num_lines) {
  /* Count lines */
  size_t count = 1;
  for (size_t i = 0; i < tab->query_len; i++) {
    if (tab->query_text[i] == '\n')
      count++;
  }

  *lines = safe_calloc(count, sizeof(QueryLineInfo));

  /* Build line info */
  size_t line_idx = 0;
  size_t line_start = 0;

  for (size_t i = 0; i <= tab->query_len; i++) {
    if (i == tab->query_len || tab->query_text[i] == '\n') {
      (*lines)[line_idx].start = line_start;
      (*lines)[line_idx].len = i - line_start;
      line_idx++;
      line_start = i + 1;
    }
  }

  *num_lines = count;
}

/* Convert cursor offset to line/column */
void query_cursor_to_line_col(Tab *tab, size_t *line, size_t *col) {
  *line = 0;
  *col = 0;

  size_t line_start = 0;
  for (size_t i = 0; i < tab->query_cursor && i < tab->query_len; i++) {
    if (tab->query_text[i] == '\n') {
      (*line)++;
      line_start = i + 1;
    }
  }
  *col = tab->query_cursor - line_start;
}

/* Convert line/column to cursor offset */
size_t query_line_col_to_cursor(Tab *tab __attribute__((unused)), size_t line,
                                size_t col, QueryLineInfo *lines,
                                size_t num_lines) {
  if (!lines || num_lines == 0)
    return 0;

  if (line >= num_lines)
    line = num_lines - 1;

  size_t cursor = lines[line].start + col;
  if (col > lines[line].len)
    cursor = lines[line].start + lines[line].len;

  return cursor;
}

/* Insert character at cursor */
void query_insert_char(Tab *tab, char c) {
  if (!query_ensure_capacity(tab, tab->query_len + 2)) {
    return; /* Allocation failed, silently ignore insert */
  }

  /* Shift text after cursor */
  memmove(tab->query_text + tab->query_cursor + 1,
          tab->query_text + tab->query_cursor,
          tab->query_len - tab->query_cursor);

  tab->query_text[tab->query_cursor] = c;
  tab->query_len++;
  tab->query_cursor++;
  tab->query_text[tab->query_len] = '\0';
}

/* Delete character at cursor (forward delete) */
void query_delete_char(Tab *tab) {
  if (tab->query_cursor >= tab->query_len)
    return;

  memmove(tab->query_text + tab->query_cursor,
          tab->query_text + tab->query_cursor + 1,
          tab->query_len - tab->query_cursor);
  tab->query_len--;
  tab->query_text[tab->query_len] = '\0';
}

/* Delete character before cursor (backspace) */
void query_backspace(Tab *tab) {
  if (tab->query_cursor == 0)
    return;

  tab->query_cursor--;
  query_delete_char(tab);
}

/* Find the byte boundaries of the query at cursor position.
 * Returns true if a valid query range was found.
 * out_start and out_end are set to the byte positions (not trimmed).
 * If cursor is in empty space after a query, falls back to the last query. */
bool query_find_bounds_at_cursor(const char *text, size_t cursor,
                                 size_t *out_start, size_t *out_end) {
  if (!text || !*text) {
    *out_start = 0;
    *out_end = 0;
    return false;
  }

  size_t len = strlen(text);
  if (cursor > len)
    cursor = len;

  /* Scan from beginning to cursor to find query start (after last ';') */
  size_t last_semi = 0;
  size_t prev_semi = 0; /* Track the semicolon before last_semi */
  bool in_string = false;
  char quote_char = 0;

  for (size_t i = 0; i < cursor; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        prev_semi = last_semi;
        last_semi = i + 1;
      }
    }
  }

  size_t start = last_semi;

  /* Scan forward from cursor to find query end (next ';' or end of text) */
  size_t end = len;
  in_string = false;
  quote_char = 0;

  for (size_t i = cursor; i < len; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        end = i + 1; /* Include the semicolon */
        break;
      }
    }
  }

  /* Check if the current range is empty/whitespace only */
  bool is_empty = true;
  for (size_t i = start; i < end && is_empty; i++) {
    char c = text[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
      is_empty = false;
    }
  }

  /* If empty and we have a previous query, use that instead */
  if (is_empty && last_semi > 0) {
    start = prev_semi;
    end = last_semi; /* Include the semicolon of the previous query */
  }

  *out_start = start;
  *out_end = end;
  return start < end;
}

/* Find the query at cursor position (caller must free result) */
char *query_find_at_cursor(const char *text, size_t cursor) {
  if (!text || !*text)
    return str_dup("");

  size_t len = strlen(text);
  if (cursor > len)
    cursor = len;

  /* Scan backward to find query start (after ';' or at beginning) */
  bool in_string = false;
  char quote_char = 0;

  /* First pass: find start by scanning from beginning to cursor */
  size_t last_semi = 0;
  in_string = false;
  for (size_t i = 0; i < cursor; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        last_semi = i + 1;
      }
    }
  }
  size_t start = last_semi;

  /* Scan forward to find query end */
  size_t end = cursor;
  in_string = false;
  for (size_t i = cursor; i < len; i++) {
    char c = text[i];
    if (in_string) {
      if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
        in_string = false;
      }
    } else {
      if (c == '\'' || c == '"') {
        in_string = true;
        quote_char = c;
      } else if (c == ';') {
        end = i;
        break;
      }
    }
    end = i + 1;
  }

  /* Extract query */
  size_t query_len = end - start;
  char *query = safe_malloc(query_len + 1);

  memcpy(query, text + start, query_len);
  query[query_len] = '\0';

  /* Trim whitespace */
  char *trimmed = query;
  while (*trimmed && isspace((unsigned char)*trimmed))
    trimmed++;

  size_t trim_len = strlen(trimmed);
  while (trim_len > 0 && isspace((unsigned char)trimmed[trim_len - 1]))
    trim_len--;

  /* If empty, try to find the last available query */
  if (trim_len == 0 && last_semi > 0) {
    free(query);

    /* Find the query before last_semi */
    size_t prev_end = last_semi - 1; /* Position of the ';' */
    size_t prev_start = 0;

    /* Scan from beginning to find the start of the previous query */
    in_string = false;
    size_t prev_semi = 0;
    for (size_t i = 0; i < prev_end; i++) {
      char c = text[i];
      if (in_string) {
        if (c == quote_char && (i == 0 || text[i - 1] != '\\')) {
          in_string = false;
        }
      } else {
        if (c == '\'' || c == '"') {
          in_string = true;
          quote_char = c;
        } else if (c == ';') {
          prev_semi = i + 1;
        }
      }
    }
    prev_start = prev_semi;

    /* Extract the previous query */
    query_len = prev_end - prev_start;
    query = safe_malloc(query_len + 1);

    memcpy(query, text + prev_start, query_len);
    query[query_len] = '\0';

    /* Trim again */
    trimmed = query;
    while (*trimmed && isspace((unsigned char)*trimmed))
      trimmed++;

    trim_len = strlen(trimmed);
    while (trim_len > 0 && isspace((unsigned char)trimmed[trim_len - 1]))
      trim_len--;
  }

  char *result = safe_malloc(trim_len + 1);
  memcpy(result, trimmed, trim_len);
  result[trim_len] = '\0';

  free(query);
  return result;
}
