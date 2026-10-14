/*
 * Lace GTK frontend
 * Minimal unit test harness
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_TEST_H
#define LACE_GTK_TEST_H

#include <stdio.h>
#include <string.h>

static int g_test_failures = 0;
static int g_test_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    g_test_checks++;                                                           \
    if (!(cond)) {                                                             \
      g_test_failures++;                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                          \
  } while (0)

#define CHECK_STR(actual, expected)                                            \
  do {                                                                         \
    g_test_checks++;                                                           \
    const char *_a = (actual);                                                 \
    const char *_e = (expected);                                               \
    if (!_a || !_e || strcmp(_a, _e) != 0) {                                   \
      g_test_failures++;                                                       \
      fprintf(stderr, "FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__,   \
              __LINE__, _a ? _a : "(null)", _e ? _e : "(null)");               \
    }                                                                          \
  } while (0)

#define TEST_MAIN_END()                                                        \
  do {                                                                         \
    fprintf(stderr, "%s: %d checks, %d failures\n", __FILE__, g_test_checks,   \
            g_test_failures);                                                  \
    return g_test_failures ? 1 : 0;                                            \
  } while (0)

#endif
