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

/* String duplication (memory-safe, never returns NULL)
 * - NULL input is treated as empty string
 * - Aborts on allocation failure (OOM is unrecoverable) */
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

/* SQL WHERE clause builder for composite primary keys.
 * Builds clauses like: "col1" = ? AND "col2" = ? (for SQLite/MySQL)
 *                  or: "col1" = $1 AND "col2" = $2 (for PostgreSQL)
 * Parameters:
 *   pk_cols      - Array of column names
 *   num_pk_cols  - Number of columns
 *   use_dollar   - true for $N placeholders (PostgreSQL), false for ? (SQLite/MySQL)
 *   start_idx    - Starting index for $N placeholders (typically 1 or 2)
 *   use_backtick - true for backtick escaping (MySQL), false for double quotes
 * Returns: Heap-allocated WHERE clause string, or NULL on failure.
 *          Caller must free(). */
char *str_build_pk_where(const char **pk_cols, size_t num_pk_cols, bool use_dollar,
                         size_t start_idx, bool use_backtick);

/* Conversion */
bool str_to_int(const char *s, int *out);
bool str_to_int64(const char *s, int64_t *out);
bool str_to_double(const char *s, double *out);

/* Secure memory handling */
void str_secure_free(char *s); /* Zero memory before freeing (for passwords) */

/* Buffer capacity management for char arrays.
 * Ensures *buf has at least min_cap bytes allocated.
 * If *buf is NULL, allocates initial_cap bytes (or min_cap if larger).
 * Returns true on success, false on allocation failure or overflow.
 * Does NOT modify *buf or *cap on failure. */
bool str_buf_ensure_capacity(char **buf, size_t *cap, size_t min_cap,
                             size_t initial_cap);

/* String builder */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool failed; /* Set to true if any operation fails */
} StringBuilder;

StringBuilder *sb_new(size_t initial_cap);
void sb_free(StringBuilder *sb);
bool sb_append(StringBuilder *sb, const char *s);
bool sb_append_len(StringBuilder *sb, const char *s, size_t len);
bool sb_append_char(StringBuilder *sb, char c);
bool sb_printf(StringBuilder *sb, const char *fmt, ...);
/*
 * Consumes the StringBuilder and returns the internal string buffer.
 * WARNING: After calling this function:
 *   - The StringBuilder pointer is INVALID and must not be used
 *   - The caller owns the returned string and must free() it
 *   - Set your sb pointer to NULL after calling to prevent accidental reuse
 * Usage: char *str = sb_to_string(sb); sb = NULL;
 */
char *sb_to_string(StringBuilder *sb);
/*
 * Consumes the StringBuilder and returns the string if successful.
 * If any operation on the builder failed, returns NULL and frees the builder.
 * This is the preferred way to finish a StringBuilder when using unchecked
 * append calls.
 * Usage: char *str = sb_finish(sb); sb = NULL;
 */
char *sb_finish(StringBuilder *sb);
/* Check if the StringBuilder is in a valid state (no operations have failed) */
bool sb_ok(StringBuilder *sb);

/* ============================================================================
 * Error string helpers
 * ============================================================================
 * Use these instead of manual `if (err) *err = str_dup(msg)` patterns.
 * All functions safely handle NULL err pointer.
 */
void err_set(char **err, const char *msg);
void err_setf(char **err, const char *fmt, ...);
void err_clear(char **err);

/* Error + return macros to reduce boilerplate error handling */

/* Set error message and return a value */
#define ERR_RETURN(err_ptr, msg, ret_val)                                      \
  do {                                                                         \
    err_set(err_ptr, msg);                                                     \
    return ret_val;                                                            \
  } while (0)

/* Check pointer, set "Memory allocation failed" and return if NULL */
#define CHECK_ALLOC(ptr, err_ptr, ret_val)                                     \
  do {                                                                         \
    if (!(ptr)) {                                                              \
      err_set(err_ptr, "Memory allocation failed");                            \
      return ret_val;                                                          \
    }                                                                          \
  } while (0)

/* ============================================================================
 * Common utility macros
 * ============================================================================
 */

/* Free a pointer and set it to NULL */
#define FREE_NULL(ptr)                                                         \
  do {                                                                         \
    free(ptr);                                                                 \
    (ptr) = NULL;                                                              \
  } while (0)

/* Get the number of elements in a static array */
#define ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/* ============================================================================
 * Array cleanup macros
 * ============================================================================
 * Use these to reduce boilerplate when freeing arrays of elements.
 */

/* Free an array of elements, calling elem_free_fn on each element.
 * elem_free_fn should be a function that takes a pointer to an element.
 * Sets arr to NULL after freeing. */
#define FREE_ARRAY(arr, count, elem_free_fn)                                   \
  do {                                                                         \
    if (arr) {                                                                 \
      for (size_t _fa_i = 0; _fa_i < (count); _fa_i++) {                       \
        elem_free_fn(&(arr)[_fa_i]);                                           \
      }                                                                        \
      free(arr);                                                               \
      (arr) = NULL;                                                            \
    }                                                                          \
  } while (0)

/* Free an array of strings (char*). Sets arr to NULL after freeing. */
#define FREE_STRING_ARRAY(arr, count)                                          \
  do {                                                                         \
    if (arr) {                                                                 \
      for (size_t _fsa_i = 0; _fsa_i < (count); _fsa_i++) {                    \
        free((arr)[_fsa_i]);                                                   \
      }                                                                        \
      free(arr);                                                               \
      (arr) = NULL;                                                            \
    }                                                                          \
  } while (0)

/* ============================================================================
 * Capacity management helpers
 * ============================================================================
 * Safe capacity doubling with overflow detection for dynamic arrays.
 */

/* Calculate doubled capacity with overflow protection.
 * Returns true if new capacity is valid, false on overflow.
 * new_cap receives the calculated capacity (doubled, or initial if current=0).
 * Usage: if (!capacity_grow(&new_cap, current, 16, sizeof(Item))) return; */
static inline bool capacity_grow(size_t *new_cap, size_t current,
                                 size_t initial, size_t elem_size) {
  size_t cap = (current == 0) ? initial : current * 2;
  /* Check for doubling overflow */
  if (current > 0 && cap < current)
    return false;
  /* Check for allocation overflow */
  if (cap > SIZE_MAX / elem_size)
    return false;
  *new_cap = cap;
  return true;
}

#endif /* LACE_STR_H */
