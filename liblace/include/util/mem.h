/*
 * Lace
 * Safe memory allocation utilities
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_MEM_H
#define LACE_MEM_H

#include "../lace_export.h"
#include <stddef.h>

LACE_BEGIN_DECLS

/* Safe memory allocation (never returns NULL)
 * - Checks for integer overflow before allocation
 * - Aborts on allocation failure or overflow (OOM is unrecoverable)
 * - Use these instead of malloc/calloc/realloc throughout the codebase */
void *safe_malloc(size_t size);
void *safe_calloc(size_t count, size_t size);
void *safe_realloc(void *ptr, size_t size);
void *safe_reallocarray(void *ptr, size_t count, size_t size);

LACE_END_DECLS

#endif /* LACE_MEM_H */
