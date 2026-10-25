/*
 * Lace
 * SQLite CREATE-text parsers (see catalog_internal.h)
 *
 * SQLite keeps a table's collations, check constraints and AUTOINCREMENT
 * only in the CREATE TABLE text, and an index's predicate and expressions
 * only in the CREATE INDEX text. These walk that text: quotes ('', "", [],
 * ``), -- and block comments, and parentheses to any depth.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "catalog_internal.h"
#include "util/mem.h"
#include "util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Length of the quoted token at s ('...', "...", [...], `...`, doubled
 * closers inside), 0 when s opens no quote; an unterminated quote runs to
 * the end */
static size_t quoted_len(const char *s) {
  char open = s[0], close;
  if (open == '\'' || open == '"' || open == '`') close = open;
  else if (open == '[') close = ']';
  else return 0;
  size_t i = 1;
  for (;;) {
    if (!s[i]) return i;
    if (s[i] == close) {
      if (close != ']' && s[i + 1] == close) { i += 2; continue; }
      return i + 1;
    }
    i++;
  }
}

/* Length of the comment at s, 0 when none */
static size_t comment_len(const char *s) {
  if (s[0] == '-' && s[1] == '-') {
    size_t i = 2;
    while (s[i] && s[i] != '\n') i++;
    return i;
  }
  if (s[0] == '/' && s[1] == '*') {
    const char *e = strstr(s + 2, "*/");
    return e ? (size_t)(e - s) + 2 : strlen(s);
  }
  return 0;
}

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$'; }

/* The index just past whitespace and comments from i */
static size_t skip_space(const char *s, size_t i) {
  for (;;) {
    while (s[i] && isspace((unsigned char)s[i])) i++;
    size_t c = comment_len(s + i);
    if (!c) return i;
    i += c;
  }
}

/* Length of the plain or quoted word at s (0 when there is none) */
static size_t word_len(const char *s) {
  size_t q = quoted_len(s);
  if (q) return q;
  size_t i = 0;
  while (word_char(s[i])) i++;
  return i;
}

/* An identifier without its quotes (doubled closers folded) */
static char *unquote(const char *s, size_t n) {
  if (!n) return str_dup("");
  char open = s[0];
  if (!(open == '\'' || open == '"' || open == '`' || open == '[')) return str_ndup(s, n);
  char close = open == '[' ? ']' : open;
  char *out = safe_malloc(n + 1);
  size_t o = 0;
  for (size_t i = 1; i < n; i++) {
    if (s[i] == close) {
      if (i + 1 < n && s[i + 1] == close && close != ']') { out[o++] = close; i++; continue; }
      if (i + 1 >= n) break;
    }
    out[o++] = s[i];
  }
  out[o] = 0;
  return out;
}

static bool word_is(const char *s, size_t n, const char *kw) {
  return n == strlen(kw) && strncasecmp(s, kw, n) == 0;
}

/* The body between the first '(' at depth 0 after the first `after`
 * keyword occurrence and its matching ')': *len bytes at the returned
 * pointer, or NULL */
static const char *paren_body(const char *s, size_t from, size_t *len) {
  size_t i = from;
  for (;;) {
    i = skip_space(s, i);
    if (!s[i]) return NULL;
    size_t q = quoted_len(s + i);
    if (q) { i += q; continue; }
    if (s[i] == '(') break;
    i++;
  }
  size_t start = ++i;
  int depth = 1;
  for (;;) {
    i = skip_space(s, i);
    if (!s[i]) return NULL;
    size_t q = quoted_len(s + i);
    if (q) { i += q; continue; }
    if (s[i] == '(') depth++;
    else if (s[i] == ')' && --depth == 0) break;
    i++;
  }
  *len = i - start;
  return s + start;
}

/* Split a parenthesised list body at its depth-0 commas; trimmed copies */
static size_t split_items(const char *body, size_t len, char ***out) {
  char **items = NULL;
  size_t n = 0, cap = 0, start = 0;
  int depth = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i < len) {
      size_t q = quoted_len(body + i);
      if (q) { i += q - 1; continue; }
      size_t c = comment_len(body + i);
      if (c) { i += c - 1; continue; }
      if (body[i] == '(') { depth++; continue; }
      if (body[i] == ')') { depth--; continue; }
      if (body[i] != ',' || depth != 0) continue;
    }
    size_t a = skip_space(body, start), b = i; /* a comment after the previous comma is not the item's */
    if (a > b) a = b;
    while (b > a && isspace((unsigned char)body[b - 1])) b--;
    if (b > a) {
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        items = safe_realloc(items, cap * sizeof *items);
      }
      items[n++] = str_ndup(body + a, b - a);
    }
    start = i + 1;
  }
  *out = items;
  return n;
}

static void free_items(char **items, size_t n) {
  for (size_t i = 0; i < n; i++) free(items[i]);
  free(items);
}

/* The CREATE TABLE's column and constraint definitions */
static size_t table_items(const char *create_sql, char ***out) {
  size_t len = 0;
  const char *body = create_sql ? paren_body(create_sql, 0, &len) : NULL;
  if (!body) { *out = NULL; return 0; }
  return split_items(body, len, out);
}

/* Position of the keyword kw at depth 0 of item (outside quotes), or -1 */
static long find_keyword(const char *item, const char *kw) {
  int depth = 0;
  for (size_t i = 0; item[i];) {
    size_t q = quoted_len(item + i);
    if (q) { i += q; continue; }
    size_t c = comment_len(item + i);
    if (c) { i += c; continue; }
    if (item[i] == '(') { depth++; i++; continue; }
    if (item[i] == ')') { depth--; i++; continue; }
    if (depth == 0 && word_char(item[i])) {
      size_t w = 0;
      while (word_char(item[i + w])) w++;
      if (word_is(item + i, w, kw)) return (long)i;
      i += w;
      continue;
    }
    i++;
  }
  return -1;
}

char *lace_sqlite_column_collation(const char *create_sql, const char *column) {
  if (!create_sql || !column) return NULL;
  char **items;
  size_t n = table_items(create_sql, &items);
  char *out = NULL;
  for (size_t i = 0; i < n && !out; i++) {
    const char *it = items[i];
    size_t w = word_len(it);
    if (!w) continue;
    char *name = unquote(it, w);
    bool keyword = !quoted_len(it) && (word_is(it, w, "CONSTRAINT") || word_is(it, w, "PRIMARY") ||
                                        word_is(it, w, "UNIQUE") || word_is(it, w, "CHECK") ||
                                        word_is(it, w, "FOREIGN"));
    if (!keyword && strcasecmp(name, column) == 0) {
      long k = find_keyword(it + w, "COLLATE");
      if (k >= 0) {
        size_t p = skip_space(it, (size_t)(w + k) + strlen("COLLATE"));
        size_t cl = word_len(it + p);
        if (cl) out = unquote(it + p, cl);
      }
      if (!out) out = NULL;
      free(name);
      break;
    }
    free(name);
  }
  free_items(items, n);
  return out;
}

size_t lace_sqlite_check_constraints(const char *create_sql, char ***names, char ***conds) {
  *names = NULL;
  *conds = NULL;
  if (!create_sql) return 0;
  char **items;
  size_t n = table_items(create_sql, &items), cnt = 0, cap = 0;
  for (size_t i = 0; i < n; i++) {
    const char *it = items[i];
    long k = find_keyword(it, "CHECK");
    if (k < 0) continue;
    size_t len = 0;
    const char *body = paren_body(it, (size_t)k + strlen("CHECK"), &len);
    if (!body) continue;
    char *name = NULL;
    size_t w = word_len(it);
    if (w && !quoted_len(it) && word_is(it, w, "CONSTRAINT")) {
      size_t p = skip_space(it, w);
      size_t nl = word_len(it + p);
      if (nl) name = unquote(it + p, nl);
    }
    if (cnt == cap) {
      cap = cap ? cap * 2 : 4;
      *names = safe_realloc(*names, cap * sizeof **names);
      *conds = safe_realloc(*conds, cap * sizeof **conds);
    }
    (*names)[cnt] = name;
    (*conds)[cnt] = str_ndup(body, len);
    cnt++;
  }
  free_items(items, n);
  return cnt;
}

bool lace_sqlite_has_autoincrement(const char *create_sql) {
  if (!create_sql) return false;
  for (size_t i = 0; create_sql[i];) {
    size_t q = quoted_len(create_sql + i);
    if (q) { i += q; continue; }
    size_t c = comment_len(create_sql + i);
    if (c) { i += c; continue; }
    if (word_char(create_sql[i])) {
      size_t w = 0;
      while (word_char(create_sql[i + w])) w++;
      if (word_is(create_sql + i, w, "AUTOINCREMENT")) return true;
      i += w;
      continue;
    }
    i++;
  }
  return false;
}

/* The end of the index's column list: the index just past its ')' */
static bool index_list(const char *index_sql, const char **body, size_t *len, size_t *end) {
  if (!index_sql) return false;
  long on = find_keyword(index_sql, "ON");
  if (on < 0) return false;
  *body = paren_body(index_sql, (size_t)on + 2, len);
  if (!*body) return false;
  *end = (size_t)(*body - index_sql) + *len + 1;
  return true;
}

char *lace_sqlite_index_where(const char *index_sql) {
  const char *body;
  size_t len, end;
  if (!index_list(index_sql, &body, &len, &end)) return NULL;
  long w = find_keyword(index_sql + end, "WHERE");
  if (w < 0) return NULL;
  size_t p = skip_space(index_sql, end + (size_t)w + strlen("WHERE"));
  size_t e = strlen(index_sql);
  while (e > p && (isspace((unsigned char)index_sql[e - 1]) || index_sql[e - 1] == ';')) e--;
  return e > p ? str_ndup(index_sql + p, e - p) : NULL;
}

size_t lace_sqlite_index_columns(const char *index_sql, char ***cols) {
  const char *body;
  size_t len, end;
  *cols = NULL;
  if (!index_list(index_sql, &body, &len, &end)) return 0;
  return split_items(body, len, cols);
}
