/*
 * Lace
 * String utilities
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_STR_H
#define LACE_STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* String duplication (NULL-safe wrappers) */
char *str_dup(const char *s);
char *str_ndup(const char *s, size_t n);

/* String formatting */
char *str_printf(const char *fmt, ...);
char *str_vprintf(const char *fmt, va_list args);

/* String comparison */
bool str_eq(const char *a, const char *b);
bool str_eq_nocase(const char *a, const char *b);

/* String manipulation */
char *str_lower(char *s); /* In-place lowercase */

/* URL encoding */
char *str_url_encode(const char *s);
char *str_url_decode(const char *s);

/* SQL identifier escaping */
char *str_escape_identifier_dquote(const char *s);   /* PostgreSQL/SQLite */
char *str_escape_identifier_backtick(const char *s); /* MySQL/MariaDB */

/* Conversion */
bool str_to_int(const char *s, int *out);
bool str_to_int64(const char *s, int64_t *out);
bool str_to_double(const char *s, double *out);

/* Secure memory handling */
void str_secure_free(char *s); /* Zero memory before freeing (for passwords) */

/* String builder */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StringBuilder;

StringBuilder *sb_new(size_t initial_cap);
void sb_free(StringBuilder *sb);
bool sb_append(StringBuilder *sb, const char *s);
bool sb_append_len(StringBuilder *sb, const char *s, size_t len);
bool sb_append_char(StringBuilder *sb, char c);
bool sb_printf(StringBuilder *sb, const char *fmt, ...);
char *sb_to_string(StringBuilder *sb); /* Returns owned string, frees builder */

#endif /* LACE_STR_H */
