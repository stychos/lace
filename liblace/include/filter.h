/*
 * liblace - Lace Client Library
 * Filter collection and WHERE clause building
 *
 * This module provides filter collection management and SQL WHERE clause
 * generation for use by clients (TUI, GUI, web).
 *
 * Basic filter types (LaceFilterOp, LaceFilter) are defined in types.h.
 * This module adds collection management and SQL generation.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_FILTER_H
#define LIBLACE_FILTER_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * Filter Collection
 * ========================================================================== */

/* Collection of filters (dynamic array) */
typedef struct {
  LaceFilter *filters;  /* Array of filters */
  size_t num_filters;   /* Number of active filters */
  size_t capacity;      /* Allocated capacity */
} LaceFilters;

/* ==========================================================================
 * Filter Collection Management
 * ========================================================================== */

/* Initialize a filter collection (zeroes all fields) */
void lace_filters_init(LaceFilters *f);

/* Free all filters and reset collection */
void lace_filters_free(LaceFilters *f);

/* Clear all filters but keep allocated memory */
void lace_filters_clear(LaceFilters *f);

/* Add a filter to the collection
 * Returns true on success, false on allocation failure */
bool lace_filters_add(LaceFilters *f, size_t column, LaceFilterOp op,
                      const char *value, const char *value2);

/* Remove a filter by index */
void lace_filters_remove(LaceFilters *f, size_t index);

/* Get number of active filters (filters with values or no-value operators) */
size_t lace_filters_count_active(const LaceFilters *f);

/* ==========================================================================
 * SQL WHERE Clause Generation
 * ========================================================================== */

/* Supported database drivers for SQL generation */
typedef enum {
  LACE_SQL_SQLITE,
  LACE_SQL_POSTGRES,
  LACE_SQL_MYSQL,
  LACE_SQL_MARIADB
} LaceSqlDriver;

/* Convert driver name string to enum */
LaceSqlDriver lace_sql_driver_from_name(const char *name);

/* Build WHERE clause from filters
 *
 * Parameters:
 *   f           - Filter collection
 *   schema      - Table schema (for column names)
 *   driver      - Database driver (affects identifier quoting, regex syntax)
 *   err         - Output: error message on failure (caller must free)
 *
 * Returns:
 *   Allocated WHERE clause string (without "WHERE" keyword), or
 *   NULL if no active filters or on error (check err).
 *   Caller must free returned string.
 *
 * Example output: "\"id\" = '123' AND \"name\" LIKE '%test%'"
 */
char *lace_filters_build_where(const LaceFilters *f, const LaceSchema *schema,
                               LaceSqlDriver driver, char **err);

/* Parse IN clause value list
 *
 * Accepts formats:
 *   - Comma-separated: 1, 2, 3
 *   - Quoted values: 'a', 'b', 'c'
 *   - Mixed: 1, 'two', 3
 *   - Parenthesized: (1, 2, 3)
 *
 * Returns:
 *   Formatted SQL value list (e.g., "1, 2, 3" or "'a', 'b', 'c'")
 *   Caller must free returned string.
 *   Returns NULL on error (sets err).
 */
char *lace_filters_parse_in_values(const char *input, char **err);

/* ==========================================================================
 * Constants
 * ========================================================================== */

/* Maximum number of values in an IN clause (DoS protection) */
#define LACE_MAX_IN_VALUES 1000

/* Sentinel value for RAW filter (no column) */
#define LACE_FILTER_COL_RAW SIZE_MAX

#endif /* LIBLACE_FILTER_H */
