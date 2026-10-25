/*
 * Lace
 * String utilities implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../include/util/str.h"
#include "../../include/constants.h"
#include "../../include/util/json_helpers.h"
#include "../../include/util/mem.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *str_dup(const char *s) {
  if (!s)
    s = "";
  char *result = strdup(s);
  if (!result) {
    fprintf(stderr, "Fatal: out of memory in str_dup\n");
    abort();
  }
  return result;
}

char *str_ndup(const char *s, size_t n) {
  if (!s)
    s = "";
  /* Not strndup: Windows has none */
  size_t len = 0;
  while (len < n && s[len])
    len++;
  char *result = malloc(len + 1);
  if (!result) {
    fprintf(stderr, "Fatal: out of memory in str_ndup\n");
    abort();
  }
  memcpy(result, s, len);
  result[len] = '\0';
  return result;
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
    if ((c < 128 && isalnum(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      sb_append_char(sb, c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      sb_append(sb, hex);
    }
  }

  return sb_to_string(sb);
}

/* Percent-decoding for URL parts (RFC 3986): "%XX" with two hex digits
 * is that byte, everything else is itself. '+' is a literal plus (it means
 * a space only in form data), and %00 stays as written: a NUL would cut
 * the value short. */
static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *str_url_decode(const char *s) {
  if (!s)
    return NULL;

  char *result = safe_malloc(strlen(s) + 1);
  char *dst = result;
  for (const char *p = s; *p; p++) {
    if (*p == '%') {
      int hi = hex_digit(p[1]), lo = hi >= 0 ? hex_digit(p[2]) : -1;
      if (hi >= 0 && lo >= 0 && (hi || lo)) {
        *dst++ = (char)(hi * 16 + lo);
        p += 2;
        continue;
      }
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

bool str_buf_ensure_capacity(char **buf, size_t *cap, size_t min_cap,
                             size_t initial_cap) {
  if (!buf || !cap)
    return false;

  if (min_cap <= *cap)
    return true;

  /* Calculate new capacity with doubling growth */
  size_t new_cap = *cap == 0 ? initial_cap : *cap;
  while (new_cap < min_cap) {
    if (new_cap > SIZE_MAX / 2)
      return false; /* Would overflow */
    new_cap *= 2;
  }

  *buf = safe_realloc(*buf, new_cap);
  *cap = new_cap;
  return true;
}

/* StringBuilder implementation */

StringBuilder *sb_new(size_t initial_cap) {
  StringBuilder *sb = safe_malloc(sizeof(StringBuilder));

  if (initial_cap == 0)
    initial_cap = LACE_SB_INITIAL_CAP;

  sb->data = safe_malloc(initial_cap);
  sb->data[0] = '\0';
  sb->len = 0;
  sb->cap = initial_cap;
  sb->failed = false;
  return sb;
}

void sb_free(StringBuilder *sb) {
  if (sb) {
    free(sb->data);
    free(sb);
  }
}

static bool sb_grow(StringBuilder *sb, size_t min_cap) {
  /* Sanity check - refuse to allocate more than 1GB for a string builder */
  static const size_t MAX_SB_SIZE = 1024UL * 1024UL * 1024UL;
  if (min_cap > MAX_SB_SIZE) {
    return false;
  }

  size_t new_cap = sb->cap;
  while (new_cap < min_cap) {
    /* Check for overflow before multiplying */
    if (new_cap > SIZE_MAX / LACE_SB_GROWTH_FACTOR) {
      /* Would overflow, try exact allocation */
      new_cap = min_cap;
      break;
    }
    new_cap *= LACE_SB_GROWTH_FACTOR;
  }

  sb->data = safe_realloc(sb->data, new_cap);
  sb->cap = new_cap;
  return true;
}

bool sb_append(StringBuilder *sb, const char *s) {
  if (!sb || !s) {
    if (sb)
      sb->failed = true;
    return false;
  }
  return sb_append_len(sb, s, strlen(s));
}

bool sb_append_len(StringBuilder *sb, const char *s, size_t len) {
  if (!sb || !s) {
    if (sb)
      sb->failed = true;
    return false;
  }

  /* Check for overflow before calculating needed size */
  if (len > SIZE_MAX - sb->len - 1) {
    sb->failed = true;
    return false;
  }

  size_t needed = sb->len + len + 1;
  if (needed > sb->cap && !sb_grow(sb, needed)) {
    sb->failed = true;
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
  if (sb->len > SIZE_MAX - 2) {
    sb->failed = true;
    return false;
  }

  size_t needed = sb->len + 2;
  if (needed > sb->cap && !sb_grow(sb, needed)) {
    sb->failed = true;
    return false;
  }

  sb->data[sb->len++] = c;
  sb->data[sb->len] = '\0';
  return true;
}

bool sb_printf(StringBuilder *sb, const char *fmt, ...) {
  if (!sb || !fmt) {
    if (sb)
      sb->failed = true;
    return false;
  }

  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int len = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  if (len < 0) {
    sb->failed = true;
    va_end(args);
    return false;
  }

  /* Check for overflow before calculating needed size */
  if ((size_t)len > SIZE_MAX - sb->len - 1) {
    sb->failed = true;
    va_end(args);
    return false;
  }

  size_t needed = sb->len + (size_t)len + 1;
  if (needed > sb->cap && !sb_grow(sb, needed)) {
    sb->failed = true;
    va_end(args);
    return false;
  }

  vsnprintf(sb->data + sb->len, len + 1, fmt, args);
  sb->len += len;
  va_end(args);

  return true;
}

/*
 * Consumes the StringBuilder and transfers ownership of the string to caller.
 * The StringBuilder struct is freed; caller must free the returned string.
 * See header for full documentation.
 */
char *sb_to_string(StringBuilder *sb) {
  if (!sb)
    return NULL;
  char *result = sb->data;
  /* Intentionally don't NULL out sb->data - we're transferring ownership */
  free(sb);
  return result;
}

/*
 * Consumes the StringBuilder and returns the string if successful.
 * If any operation on the builder failed, returns NULL and frees everything.
 */
char *sb_finish(StringBuilder *sb) {
  if (!sb)
    return NULL;
  if (sb->failed) {
    sb_free(sb);
    return NULL;
  }
  return sb_to_string(sb);
}

/* Check if the StringBuilder is in a valid state */
bool sb_ok(StringBuilder *sb) { return sb && !sb->failed; }

/* ============================================================================
 * Error string helpers
 * ============================================================================
 */

void err_set(char **err, const char *msg) {
  if (!err)
    return;
  free(*err);
  *err = msg ? str_dup(msg) : NULL;
}

void err_setf(char **err, const char *fmt, ...) {
  if (!err || !fmt)
    return;
  free(*err);
  va_list args;
  va_start(args, fmt);
  *err = str_vprintf(fmt, args);
  va_end(args);
}

void err_clear(char **err) {
  if (err) {
    free(*err);
    *err = NULL;
  }
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

  sb_append_char(sb, '"');
  for (const char *p = s; *p; p++) {
    if (*p == '"')
      sb_append(sb, "\"\""); /* Double the quote */
    else
      sb_append_char(sb, *p);
  }
  sb_append_char(sb, '"');

  return sb_finish(sb); /* Returns NULL on any failure */
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

  sb_append_char(sb, '`');
  for (const char *p = s; *p; p++) {
    if (*p == '`')
      sb_append(sb, "``"); /* Double the backtick */
    else
      sb_append_char(sb, *p);
  }
  sb_append_char(sb, '`');

  return sb_finish(sb); /* Returns NULL on any failure */
}

/* ============================================================================
 * JSON file I/O helpers
 * ============================================================================
 */

#define JSON_DEFAULT_MAX_SIZE (1024 * 1024) /* 1MB */

cJSON *json_load_from_file(const char *path, size_t max_size, char **error) {
  if (!path) {
    err_setf(error, "NULL path");
    return NULL;
  }

  if (max_size == 0)
    max_size = JSON_DEFAULT_MAX_SIZE;

  FILE *f = fopen(path, "r");
  if (!f) {
    err_setf(error, "Failed to open %s: %s", path, strerror(errno));
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0 || (size_t)size > max_size) {
    fclose(f);
    err_setf(error, "Invalid file size for %s", path);
    return NULL;
  }

  char *content = safe_malloc((size_t)size + 1);

  size_t read_bytes = fread(content, 1, (size_t)size, f);
  fclose(f);
  content[read_bytes] = '\0';

  cJSON *json = cJSON_Parse(content);
  free(content);

  if (!json) {
    const char *err_ptr = cJSON_GetErrorPtr();
    err_setf(error, "JSON parse error in %s: %s", path,
             err_ptr ? err_ptr : "unknown");
    return NULL;
  }

  return json;
}
