/*
 * Lace
 * Minimal unit test helpers (no framework: each test binary is one main)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TEST_H
#define LACE_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One binary per file keeps static counters; a binary made of several
 * files defines TEST_COUNTERS_EXTERN and defines the counters once */
#ifdef TEST_COUNTERS_EXTERN
extern int g_test_checks;
extern int g_test_failures;
#else
static int g_test_checks = 0;
static int g_test_failures = 0;
#endif

#define CHECK(cond)                                                            \
  do {                                                                         \
    g_test_checks++;                                                           \
    if (!(cond)) {                                                             \
      g_test_failures++;                                                       \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                          \
  } while (0)

/* Compare a string the test owns (freed here) with the expected one */
#define CHECK_STR_FREE(got_expr, want)                                         \
  do {                                                                         \
    char *got_ = (got_expr);                                                   \
    const char *want_ = (want);                                                \
    g_test_checks++;                                                           \
    if (!got_ || !want_ ? got_ != (char *)want_ : strcmp(got_, want_) != 0) {  \
      g_test_failures++;                                                       \
      fprintf(stderr, "%s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__,         \
              __LINE__, #got_expr, got_ ? got_ : "(null)",                     \
              want_ ? want_ : "(null)");                                       \
    }                                                                          \
    free(got_);                                                                \
  } while (0)

/* Compare a borrowed string with the expected one */
#define CHECK_STR(got_expr, want)                                              \
  do {                                                                         \
    const char *got_ = (got_expr), *want_ = (want);                            \
    g_test_checks++;                                                           \
    if (!got_ || !want_ || strcmp(got_, want_) != 0) {                          \
      g_test_failures++;                                                       \
      fprintf(stderr, "%s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__,         \
              __LINE__, #got_expr, got_ ? got_ : "(null)",                     \
              want_ ? want_ : "(null)");                                       \
    }                                                                          \
  } while (0)

#define CHECK_SIZE(got_expr, want)                                             \
  do {                                                                         \
    size_t got_ = (size_t)(got_expr), want_ = (size_t)(want);                  \
    g_test_checks++;                                                           \
    if (got_ != want_) {                                                       \
      g_test_failures++;                                                       \
      fprintf(stderr, "%s:%d: %s = %zu, want %zu\n", __FILE__, __LINE__,       \
              #got_expr, got_, want_);                                         \
    }                                                                          \
  } while (0)

static inline int test_report(const char *suite) {
  printf("%s: %d checks, %d failed\n", suite, g_test_checks, g_test_failures);
  return g_test_failures ? 1 : 0;
}

#endif /* LACE_TEST_H */
