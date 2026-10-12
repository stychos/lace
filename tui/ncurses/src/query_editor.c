/*
 * Lace
 * Query tab text editor functions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_internal.h"
#include "../../liblace/include/util/mem.h"
#include <ctype.h>
#include <strings.h>
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
size_t query_line_col_to_cursor(Tab *tab, size_t line,
                                size_t col, QueryLineInfo *lines,
                                size_t num_lines) {
  if (!lines || num_lines == 0)
    return 0;

  if (line >= num_lines)
    line = num_lines - 1;

  const char *text = tab->query_text + lines[line].start;
  size_t len = lines[line].len;
  size_t pos = 0, width = 0;
  while (pos < len) {
    size_t next = text[pos] == '\t' ? pos + 1 : utf8_next(text, len, pos);
    size_t w = query_display_width(text + pos, next - pos);
    if (width + w > col)
      break;
    width += w;
    pos = next;
  }
  return lines[line].start + pos;
}

size_t query_display_width(const char *text, size_t n) {
  /* As the editor draws: tabs 4 columns, other control characters not at
   * all */
  size_t width = 0, start = 0;
  for (size_t i = 0; i <= n; i++) {
    unsigned char c = i < n ? (unsigned char)text[i] : 0;
    if (i == n || c < 32 || c == 127) {
      width += (size_t)utf8_width(text + start, i - start);
      if (c == '\t')
        width += 4;
      start = i + 1;
    }
  }
  return width;
}

size_t query_cursor_display_col(Tab *tab) {
  size_t line, col;
  query_cursor_to_line_col(tab, &line, &col);
  return query_display_width(tab->query_text + tab->query_cursor - col, col);
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

  size_t next = utf8_next(tab->query_text, tab->query_len, tab->query_cursor);
  memmove(tab->query_text + tab->query_cursor, tab->query_text + next,
          tab->query_len - next);
  tab->query_len -= next - tab->query_cursor;
  tab->query_text[tab->query_len] = '\0';
}

/* Delete character before cursor (backspace) */
void query_backspace(Tab *tab) {
  if (tab->query_cursor == 0)
    return;

  tab->query_cursor = utf8_prev(tab->query_text, tab->query_cursor);
  query_delete_char(tab);
}

SqlDialect query_sql_dialect(const char *driver) {
  SqlDialect d = {false, false, false};
  if (!driver)
    return d;
  d.mysql = strcmp(driver, "mysql") == 0 || strcmp(driver, "mariadb") == 0;
  d.sqlite = strcmp(driver, "sqlite") == 0;
  d.postgres = strcmp(driver, "postgres") == 0 ||
               strcmp(driver, "postgresql") == 0 || strcmp(driver, "pg") == 0;
  return d;
}

SqlDialect query_sql_dialect_of(LaceDriver driver) {
  SqlDialect d = {driver == LACE_DRIVER_MYSQL || driver == LACE_DRIVER_MARIADB,
                  driver == LACE_DRIVER_SQLITE, driver == LACE_DRIVER_POSTGRES};
  return d;
}

/* The shared lexer (liblace) for this dialect: an unknown driver lexes the
 * common subset */
static int dialect_driver(SqlDialect d) {
  return d.mysql      ? LACE_DRIVER_MYSQL
         : d.sqlite   ? LACE_DRIVER_SQLITE
         : d.postgres ? LACE_DRIVER_POSTGRES
                      : -1;
}

size_t query_sql_skip(const char *s, SqlDialect d, bool *is_comment) {
  return lace_sql_skip(s, dialect_driver(d), is_comment);
}

/* Find the query at cursor position (caller must free result) */
/* Length of the SQL statement starting at p, up to (not including) the
 * terminating ';' or end of text. Semicolons inside string literals,
 * quoted identifiers, -- and / * * / comments, and PostgreSQL dollar-quoted
 * bodies ($$ ... $$, $tag$ ... $tag$) do not end a statement. Backslash
 * escapes inside strings only exist in MySQL/MariaDB. */
size_t query_statement_length(const char *p, const char *driver) {
  return lace_sql_statement_length(p, dialect_driver(query_sql_dialect(driver)));
}

void query_sql_shape(const char *sql, const char *driver, QuerySqlShape *out) {
  memset(out, 0, sizeof(*out));
  if (!sql) {
    return;
  }
  SqlDialect d = query_sql_dialect(driver);
  int depth = 0;
  const char *s = sql;
  while (*s) {
    char c = *s;
    /* Strings, quoted identifiers and $tag$ bodies count toward the end;
     * comments are skipped, never part of the significant end */
    bool comment;
    size_t skip = query_sql_skip(s, d, &comment);
    if (skip) {
      s += skip;
      if (!comment)
        out->end = (size_t)(s - sql);
      continue;
    }
    if (isspace((unsigned char)c) || c == ';') {
      s++; /* Trailing ';' and whitespace do not count */
      continue;
    }
    if (c == '(') {
      depth++;
    } else if (c == ')' && depth > 0) {
      depth--;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      const char *w = s;
      while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '$')) s++;
      size_t len = (size_t)(s - w);
      if (!out->first_keyword[0] && len < sizeof(out->first_keyword)) {
        for (size_t i = 0; i < len; i++) {
          out->first_keyword[i] = (char)toupper((unsigned char)w[i]);
        }
        out->first_keyword[len] = '\0';
      }
      /* The statement's verb: the first top-level DML/query keyword, so a
       * WITH list (its bodies are in parentheses) is looked through */
      static const char *const verbs[] = {"SELECT", "INSERT", "UPDATE", "DELETE",
                                          "MERGE",  "REPLACE", "VALUES", "TABLE",
                                          "UPSERT", NULL};
      if (depth == 0 && !out->verb[0]) {
        for (int v = 0; verbs[v]; v++) {
          if (strlen(verbs[v]) == len && strncasecmp(w, verbs[v], len) == 0) {
            memcpy(out->verb, verbs[v], len + 1);
            break;
          }
        }
      }
      if (depth == 0 && len == 4 && strncasecmp(w, "INTO", 4) == 0) {
        out->top_level_into = true;
      }
      if (depth == 0 && ((len == 5 && strncasecmp(w, "LIMIT", 5) == 0) ||
                         (len == 6 && strncasecmp(w, "OFFSET", 6) == 0) ||
                         (len == 5 && strncasecmp(w, "FETCH", 5) == 0))) {
        out->top_level_limit = true;
      }
      out->end = (size_t)(s - sql);
      continue;
    }
    s++;
    out->end = (size_t)(s - sql);
  }
  /* A writing verb anywhere (a data-modifying WITH) or INTO: liblace's
   * read-only test, the same one the daemon's row limit uses */
  out->nested_write = !lace_sql_read_only(sql, dialect_driver(d));
}

bool query_page_needs_wrap(const QuerySqlShape *shape) {
  return shape->top_level_limit || strcmp(shape->verb, "VALUES") == 0;
}

bool query_sql_pageable(const QuerySqlShape *shape) {
  const char *kw = shape->first_keyword;
  const char *v = shape->verb;
  bool starts_query = strcmp(kw, "SELECT") == 0 || strcmp(kw, "WITH") == 0 ||
                      strcmp(kw, "VALUES") == 0 || strcmp(kw, "TABLE") == 0;
  bool reads = strcmp(v, "SELECT") == 0 || strcmp(v, "VALUES") == 0 ||
               strcmp(v, "TABLE") == 0;
  return starts_query && reads && !shape->top_level_into &&
         !shape->nested_write && shape->end > 0;
}

/* Copy text[start, start+len) without surrounding whitespace */
static char *dup_trimmed(const char *text, size_t start, size_t len) {
  while (len > 0 && isspace((unsigned char)text[start])) {
    start++;
    len--;
  }
  while (len > 0 && isspace((unsigned char)text[start + len - 1])) {
    len--;
  }
  return str_ndup(text + start, len);
}

bool query_find_bounds_at_cursor(const char *text, size_t cursor,
                                 const char *driver, size_t *out_start,
                                 size_t *out_end) {
  *out_start = 0;
  *out_end = 0;
  if (!text || !*text)
    return false;

  size_t len = strlen(text);
  if (cursor > len)
    cursor = len;

  /* Walk statements with the same splitter "run all" uses. The statement
   * containing the cursor (or ending at it) wins; if the cursor sits in
   * blank space after the last ';', fall back to the previous statement. */
  size_t start = 0;
  bool have_prev = false;
  size_t prev_start = 0, prev_end = 0;
  while (start <= len) {
    size_t slen = query_statement_length(text + start, driver);
    size_t end = start + slen < len ? start + slen + 1 : len; /* With ';' */
    char *stmt = dup_trimmed(text, start, slen);
    QuerySqlShape shape;
    query_sql_shape(stmt, driver, &shape);
    bool blank = !stmt || shape.end == 0; /* Whitespace or comments only */
    free(stmt);
    if (cursor <= start + slen) {
      if (blank && have_prev) {
        *out_start = prev_start;
        *out_end = prev_end;
      } else {
        *out_start = start;
        *out_end = end;
      }
      return *out_start < *out_end;
    }
    if (!blank) {
      have_prev = true;
      prev_start = start;
      prev_end = end;
    }
    start += slen + 1; /* skip the ';' */
  }
  if (have_prev) {
    *out_start = prev_start;
    *out_end = prev_end;
  }
  return have_prev;
}

char *query_find_at_cursor(const char *text, size_t cursor, const char *driver) {
  size_t start, end;
  if (!query_find_bounds_at_cursor(text, cursor, driver, &start, &end))
    return str_dup("");
  size_t slen = query_statement_length(text + start, driver);
  return dup_trimmed(text, start, slen);
}
