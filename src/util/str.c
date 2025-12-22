/*
 * lace - Database Viewer and Manager
 * String utilities implementation
 */

#include "str.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SB_INITIAL_CAP 64
#define SB_GROWTH_FACTOR 2

char *str_dup(const char *s) {
  if (!s)
    return NULL;
  return strdup(s);
}

char *str_ndup(const char *s, size_t n) {
  if (!s)
    return NULL;
  return strndup(s, n);
}

char *str_printf(const char *fmt, ...) {
  if (!fmt)
    return NULL;
  char *result = NULL;
  va_list args;
  va_start(args, fmt);
  if (vasprintf(&result, fmt, args) < 0)
    result = NULL;
  va_end(args);
  return result;
}

char *str_vprintf(const char *fmt, va_list args) {
  if (!fmt)
    return NULL;
  char *result = NULL;
  if (vasprintf(&result, fmt, args) < 0)
    return NULL;
  return result;
}

bool str_eq(const char *a, const char *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  return strcmp(a, b) == 0;
}

bool str_eq_nocase(const char *a, const char *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  return strcasecmp(a, b) == 0;
}

char *str_lower(char *s) {
  if (!s)
    return NULL;
  for (char *p = s; *p; p++) {
    *p = tolower((unsigned char)*p);
  }
  return s;
}

char *str_upper(char *s) {
  if (!s)
    return NULL;
  for (char *p = s; *p; p++) {
    *p = toupper((unsigned char)*p);
  }
  return s;
}

char *str_url_encode(const char *s) {
  if (!s)
    return NULL;

  /* Check for overflow in capacity calculation */
  size_t slen = strlen(s);
  if (slen > SIZE_MAX / 3)
    return NULL;

  StringBuilder *sb = sb_new(slen * 3);
  if (!sb)
    return NULL;

  for (const char *p = s; *p; p++) {
    unsigned char c = *p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      sb_append_char(sb, c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      sb_append(sb, hex);
    }
  }

  return sb_to_string(sb);
}

char *str_url_decode(const char *s) {
  if (!s)
    return NULL;

  size_t len = strlen(s);
  char *result = malloc(len + 1);
  if (!result)
    return NULL;

  char *dst = result;
  for (const char *p = s; *p; p++) {
    if (*p == '%' && p[1] && p[2]) {
      char hex[3] = {p[1], p[2], '\0'};
      char *endptr;
      long val = strtol(hex, &endptr, 16);
      if (*endptr == '\0' && val >= 0 && val <= 255) {
        *dst++ = (unsigned char)val;
        p += 2;
        continue;
      }
    } else if (*p == '+') {
      *dst++ = ' ';
      continue;
    }
    *dst++ = *p;
  }
  *dst = '\0';

  return result;
}

bool str_to_int(const char *s, int *out) {
  if (!s || !out)
    return false;
  char *endptr;
  errno = 0;
  long val = strtol(s, &endptr, 10);
  if (errno != 0 || *endptr != '\0' || endptr == s)
    return false;
  if (val < INT32_MIN || val > INT32_MAX)
    return false;
  *out = (int)val;
  return true;
}

bool str_to_int64(const char *s, int64_t *out) {
  if (!s || !out)
    return false;
  char *endptr;
  errno = 0;
  long long val = strtoll(s, &endptr, 10);
  if (errno != 0 || *endptr != '\0' || endptr == s)
    return false;
  *out = (int64_t)val;
  return true;
}

bool str_to_double(const char *s, double *out) {
  if (!s || !out)
    return false;
  char *endptr;
  errno = 0;
  double val = strtod(s, &endptr);
  if (errno != 0 || *endptr != '\0' || endptr == s)
    return false;
  *out = val;
  return true;
}

/* Secure memory handling */

void str_secure_free(char *s) {
  if (!s)
    return;
  size_t len = strlen(s);
  /* Use volatile function pointer to prevent compiler from optimizing away */
  static void *(*const volatile memset_ptr)(void *, int, size_t) = memset;
  memset_ptr(s, 0, len);
  free(s);
}

/* StringBuilder implementation */

StringBuilder *sb_new(size_t initial_cap) {
  StringBuilder *sb = malloc(sizeof(StringBuilder));
  if (!sb)
    return NULL;

  if (initial_cap == 0)
    initial_cap = SB_INITIAL_CAP;

  sb->data = malloc(initial_cap);
  if (!sb->data) {
    free(sb);
    return NULL;
  }

  sb->data[0] = '\0';
  sb->len = 0;
  sb->cap = initial_cap;
  return sb;
}

void sb_free(StringBuilder *sb) {
  if (sb) {
    free(sb->data);
    free(sb);
  }
}

static bool sb_grow(StringBuilder *sb, size_t min_cap) {
  size_t new_cap = sb->cap;
  while (new_cap < min_cap) {
    /* Check for overflow before multiplying */
    if (new_cap > SIZE_MAX / SB_GROWTH_FACTOR) {
      /* Would overflow, try exact allocation */
      new_cap = min_cap;
      break;
    }
    new_cap *= SB_GROWTH_FACTOR;
  }

  char *new_data = realloc(sb->data, new_cap);
  if (!new_data)
    return false;

  sb->data = new_data;
  sb->cap = new_cap;
  return true;
}

bool sb_append(StringBuilder *sb, const char *s) {
  if (!sb || !s)
    return false;
  return sb_append_len(sb, s, strlen(s));
}

bool sb_append_len(StringBuilder *sb, const char *s, size_t len) {
  if (!sb || !s)
    return false;

  /* Check for overflow before calculating needed size */
  if (len > SIZE_MAX - sb->len - 1)
    return false;

  size_t needed = sb->len + len + 1;
  if (needed > sb->cap && !sb_grow(sb, needed)) {
    return false;
  }

  memcpy(sb->data + sb->len, s, len);
  sb->len += len;
  sb->data[sb->len] = '\0';
  return true;
}

bool sb_append_char(StringBuilder *sb, char c) {
  if (!sb)
    return false;

  /* Check for overflow (extremely unlikely but possible) */
  if (sb->len > SIZE_MAX - 2)
    return false;

  size_t needed = sb->len + 2;
  if (needed > sb->cap && !sb_grow(sb, needed)) {
    return false;
  }

  sb->data[sb->len++] = c;
  sb->data[sb->len] = '\0';
  return true;
}

bool sb_printf(StringBuilder *sb, const char *fmt, ...) {
  if (!sb || !fmt)
    return false;

  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int len = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  if (len < 0) {
    va_end(args);
    return false;
  }

  /* Check for overflow before calculating needed size */
  if ((size_t)len > SIZE_MAX - sb->len - 1) {
    va_end(args);
    return false;
  }

  size_t needed = sb->len + (size_t)len + 1;
  if (needed > sb->cap && !sb_grow(sb, needed)) {
    va_end(args);
    return false;
  }

  vsnprintf(sb->data + sb->len, len + 1, fmt, args);
  sb->len += len;
  va_end(args);

  return true;
}

char *sb_to_string(StringBuilder *sb) {
  if (!sb)
    return NULL;
  char *result = sb->data;
  free(sb);
  return result;
}

/*
 * SQL Identifier escaping functions
 * These escape identifiers (table/column names) to prevent SQL injection
 */

char *str_escape_identifier_dquote(const char *s) {
  /* Escape for PostgreSQL/SQLite: double quotes, escape by doubling */
  if (!s)
    return NULL;

  /* Check for overflow in capacity calculation */
  size_t slen = strlen(s);
  if (slen >= (SIZE_MAX - 3) / 2)
    return NULL;

  StringBuilder *sb = sb_new(slen * 2 + 3);
  if (!sb)
    return NULL;

  if (!sb_append_char(sb, '"')) {
    sb_free(sb);
    return NULL;
  }
  for (const char *p = s; *p; p++) {
    bool ok;
    if (*p == '"') {
      ok = sb_append(sb, "\"\""); /* Double the quote */
    } else {
      ok = sb_append_char(sb, *p);
    }
    if (!ok) {
      sb_free(sb);
      return NULL;
    }
  }
  if (!sb_append_char(sb, '"')) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}

char *str_escape_identifier_backtick(const char *s) {
  /* Escape for MySQL/MariaDB: backticks, escape by doubling */
  if (!s)
    return NULL;

  /* Check for overflow in capacity calculation */
  size_t slen = strlen(s);
  if (slen >= (SIZE_MAX - 3) / 2)
    return NULL;

  StringBuilder *sb = sb_new(slen * 2 + 3);
  if (!sb)
    return NULL;

  if (!sb_append_char(sb, '`')) {
    sb_free(sb);
    return NULL;
  }
  for (const char *p = s; *p; p++) {
    bool ok;
    if (*p == '`') {
      ok = sb_append(sb, "``"); /* Double the backtick */
    } else {
      ok = sb_append_char(sb, *p);
    }
    if (!ok) {
      sb_free(sb);
      return NULL;
    }
  }
  if (!sb_append_char(sb, '`')) {
    sb_free(sb);
    return NULL;
  }

  return sb_to_string(sb);
}
