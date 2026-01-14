/*
 * Lace
 * Safe memory allocation utilities - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "mem.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *safe_malloc(size_t size) {
  if (size == 0)
    size = 1; /* malloc(0) behavior is implementation-defined */
  void *ptr = malloc(size);
  if (!ptr) {
    fprintf(stderr, "Fatal: out of memory in safe_malloc(%zu)\n", size);
    abort();
  }
  return ptr;
}

void *safe_calloc(size_t count, size_t size) {
  if (count == 0 || size == 0) {
    count = 1;
    size = 1;
  }
  /* Check for overflow before allocation */
  if (count > SIZE_MAX / size) {
    fprintf(stderr, "Fatal: allocation overflow in safe_calloc(%zu, %zu)\n",
            count, size);
    abort();
  }
  void *ptr = calloc(count, size);
  if (!ptr) {
    fprintf(stderr, "Fatal: out of memory in safe_calloc(%zu, %zu)\n", count,
            size);
    abort();
  }
  return ptr;
}

void *safe_realloc(void *ptr, size_t size) {
  if (size == 0)
    size = 1;
  void *new_ptr = realloc(ptr, size);
  if (!new_ptr) {
    fprintf(stderr, "Fatal: out of memory in safe_realloc(%zu)\n", size);
    abort();
  }
  return new_ptr;
}

void *safe_reallocarray(void *ptr, size_t count, size_t size) {
  if (count == 0 || size == 0) {
    count = 1;
    size = 1;
  }
  /* Check for overflow before allocation */
  if (count > SIZE_MAX / size) {
    fprintf(stderr,
            "Fatal: allocation overflow in safe_reallocarray(%zu, %zu)\n",
            count, size);
    abort();
  }
  void *new_ptr = realloc(ptr, count * size);
  if (!new_ptr) {
    fprintf(stderr, "Fatal: out of memory in safe_reallocarray(%zu, %zu)\n",
            count, size);
    abort();
  }
  return new_ptr;
}
