/*
 * lace - Database Viewer and Manager
 * String utilities
 */

#ifndef LACE_STR_H
#define LACE_STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* String duplication */
char *str_dup(const char *s);
char *str_ndup(const char *s, size_t n);

/* String formatting */
char *str_printf(const char *fmt, ...);
char *str_vprintf(const char *fmt, va_list args);

/* String comparison */
bool str_eq(const char *a, const char *b);
bool str_eq_nocase(const char *a, const char *b);
bool str_starts_with(const char *s, const char *prefix);
bool str_ends_with(const char *s, const char *suffix);
bool str_contains(const char *haystack, const char *needle);

/* String manipulation */
char *str_trim(char *s);           /* In-place trim */
char *str_trim_dup(const char *s); /* Returns new trimmed string */
char *str_lower(char *s);          /* In-place lowercase */
char *str_upper(char *s);          /* In-place uppercase */
char *str_replace(const char *s, const char *old, const char *new_str);

/* String splitting */
char **str_split(const char *s, char delim, size_t *count);
void str_split_free(char **parts, size_t count);

/* String joining */
char *str_join(const char **parts, size_t count, const char *sep);

/* URL encoding */
char *str_url_encode(const char *s);
char *str_url_decode(const char *s);

/* Escape/unescape */
char *str_escape(const char *s);     /* Escape special chars */
char *str_unescape(const char *s);   /* Unescape special chars */
char *str_escape_sql(const char *s); /* SQL string escaping */
char *str_escape_identifier_dquote(
    const char *s); /* SQL identifier escaping (PostgreSQL/SQLite) */
char *str_escape_identifier_backtick(
    const char *s); /* SQL identifier escaping (MySQL/MariaDB) */

/* Conversion */
bool str_to_int(const char *s, int *out);
bool str_to_long(const char *s, long *out);
bool str_to_int64(const char *s, int64_t *out);
bool str_to_double(const char *s, double *out);
bool str_to_bool(const char *s, bool *out);

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
const char *sb_data(StringBuilder *sb);
size_t sb_len(StringBuilder *sb);
void sb_clear(StringBuilder *sb);

#endif /* LACE_STR_H */
