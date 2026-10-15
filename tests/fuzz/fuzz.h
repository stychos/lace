/*
 * Lace
 * Fuzz targets: shared helpers. Each target is a libFuzzer entry point that
 * checks invariants, not only that nothing crashes; a broken invariant
 * aborts with a message, which libFuzzer reports as a crash.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FUZZ_H
#define LACE_FUZZ_H

#include "lace.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_FAIL(...)                                                         \
  do {                                                                         \
    fprintf(stderr, "invariant broken (%s:%d): ", __FILE__, __LINE__);        \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
    abort();                                                                   \
  } while (0)

/* The input as a C string (up to its first NUL); caller frees */
static inline char *fuzz_cstr(const uint8_t *data, size_t size) {
  char *s = malloc(size + 1);
  memcpy(s, data, size);
  s[size] = '\0';
  return s;
}

static const int fuzz_drivers[] = {LACE_DRIVER_SQLITE, LACE_DRIVER_POSTGRES,
                                   LACE_DRIVER_MYSQL, LACE_DRIVER_MARIADB};

/* SQL that liblace built from untrusted text (a literal, an identifier, a
 * WHERE clause) must stay inside its quotes: lexed with the shared lexer,
 * everything outside quoted tokens is words, numbers, spaces and the
 * punctuation in `allowed`, never a comment or a ';' */
static inline void fuzz_check_contained(const char *sql, int driver,
                                        const char *allowed, const char *what,
                                        const char *input) {
  size_t len = strlen(sql);
  for (size_t i = 0; i < len;) {
    bool comment = false;
    size_t k = lace_sql_skip(sql + i, driver, &comment);
    if (k > len - i)
      FUZZ_FAIL("%s: lexer skipped past the end", what);
    if (k) {
      if (comment)
        FUZZ_FAIL("%s for [%s] contains a comment: [%s]", what, input, sql);
      i += k;
      continue;
    }
    unsigned char c = (unsigned char)sql[i];
    if (!(isalnum(c) || c == '_' || c == ' ' || strchr(allowed, c)) || c == '\0')
      FUZZ_FAIL("%s for [%s] has '%c' outside quotes: [%s]", what, input, c, sql);
    i++;
  }
}

#endif
