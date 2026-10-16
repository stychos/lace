#include "ui/sqlhl.h"
#include <ctype.h>
#include <string.h>

static const char *const KEYWORDS[] = {
    "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "NULL", "IS", "IN", "LIKE", "ORDER", "BY",
    "GROUP", "HAVING", "LIMIT", "OFFSET", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
    "CREATE", "TABLE", "DROP", "ALTER", "JOIN", "LEFT", "RIGHT", "INNER", "OUTER", "ON", "AS",
    "DISTINCT", "UNION", "ALL", "CASE", "WHEN", "THEN", "ELSE", "END", "BEGIN", "COMMIT",
    "ROLLBACK", "WITH", "RETURNING", "EXISTS", "BETWEEN", "ASC", "DESC", "PRIMARY", "KEY",
    "DEFAULT"};

static bool is_keyword(const char *s, size_t n) {
  if (n == 0 || n > 10) return false;
  char up[16];
  for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)s[i]);
  up[n] = 0;
  for (size_t i = 0; i < sizeof KEYWORDS / sizeof *KEYWORDS; i++)
    if (strcmp(KEYWORDS[i], up) == 0) return true;
  return false;
}

static bool is_word_byte(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

size_t sqlhl_next(const char *s, size_t pos, LaceDriver d, HlClass *cls) {
  const char *p = s + pos;
  *cls = HL_PLAIN;
  if (!*p) return 0;
  bool comment = false;
  size_t n = lace_sql_skip(p, (int)d, &comment);
  if (n > 0) {
    /* A quoted name or a $tag$ body is not a string to colour specially, but
     * only strings and comments are told apart here */
    *cls = comment ? HL_COMMENT : HL_STRING;
    return n;
  }
  if (isdigit((unsigned char)*p)) {
    size_t i = 1;
    while (isdigit((unsigned char)p[i]) || p[i] == '.') i++;
    if (!is_word_byte((unsigned char)p[i])) {
      *cls = HL_NUMBER;
      return i;
    }
  }
  size_t w = lace_sql_word_length(p, (int)d);
  if (w == 0 && is_word_byte((unsigned char)*p)) {
    while (is_word_byte((unsigned char)p[w])) w++;
  }
  if (w > 0) {
    if (isdigit((unsigned char)*p)) {
      /* digits followed by letters: a name such as 1abc, plain */
      *cls = HL_PLAIN;
    } else if (is_keyword(p, w)) {
      *cls = HL_KEYWORD;
    }
    return w;
  }
  /* One character (a whole UTF-8 sequence stays together) */
  size_t k = 1;
  while (((unsigned char)p[k] & 0xC0) == 0x80) k++;
  return k;
}
