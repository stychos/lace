/*
 * liblace - Lace Client Library
 * Keyset (cursor-based) pagination support
 *
 * Keyset pagination uses WHERE pk > last_pk ORDER BY pk LIMIT N instead of
 * LIMIT/OFFSET. This provides O(log N) performance regardless of position,
 * compared to O(offset) for traditional offset pagination.
 *
 * Usage:
 *   // Check if table supports keyset pagination
 *   if (lace_keyset_can_use(schema)) {
 *     LaceKeysetState *ks = lace_keyset_create(schema, sorts, num_sorts);
 *
 *     // Query with keyset pagination
 *     LaceResult *result;
 *     lace_query_keyset(client, conn_id, table, ks, true, filters, num_filters,
 *                       PAGE_SIZE, &result);
 *
 *     // Extract boundaries for next query
 *     lace_keyset_extract_boundaries(ks, result);
 *
 *     // Query next page
 *     lace_query_keyset(client, conn_id, table, ks, true, ...);
 *
 *     lace_keyset_free(ks);
 *   }
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_KEYSET_H
#define LIBLACE_KEYSET_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for client handle */
typedef struct lace_client lace_client_t;

/* ==========================================================================
 * Keyset State
 * ========================================================================== */

/* Opaque keyset pagination state */
typedef struct LaceKeysetState LaceKeysetState;

/* ==========================================================================
 * Keyset Capability Checking
 * ========================================================================== */

/*
 * Check if a table can use keyset pagination.
 * Returns true if the table has a usable primary key.
 *
 * @param schema  Table schema
 * @return        true if keyset pagination is possible
 */
bool lace_keyset_can_use(const LaceSchema *schema);

/*
 * Check if keyset pagination has a suitable index for the given sort columns.
 * Used by "smart" pagination mode to decide between keyset and offset.
 *
 * @param schema     Table schema (contains indexes)
 * @param sorts      Sort specifications (NULL if no sorting)
 * @param num_sorts  Number of sort specifications
 * @return           true if there's a suitable index for keyset pagination
 */
bool lace_keyset_has_suitable_index(const LaceSchema *schema,
                                    const LaceSort *sorts, size_t num_sorts);

/* ==========================================================================
 * Keyset State Lifecycle
 * ========================================================================== */

/*
 * Create keyset state for a schema with optional sort columns.
 * The keyset columns are: sort columns (if any) + primary key columns.
 *
 * @param schema     Table schema (must have primary key)
 * @param sorts      Sort specifications (NULL if no sorting)
 * @param num_sorts  Number of sort specifications
 * @param driver     Database driver type (for SQL syntax)
 * @return           Keyset state, or NULL if table doesn't support keyset
 */
LaceKeysetState *lace_keyset_create(const LaceSchema *schema,
                                    const LaceSort *sorts, size_t num_sorts,
                                    LaceDriver driver);

/*
 * Free keyset state and all associated memory.
 *
 * @param ks  Keyset state (NULL is safe)
 */
void lace_keyset_free(LaceKeysetState *ks);

/* ==========================================================================
 * Boundary Management
 * ========================================================================== */

/*
 * Extract boundary values from the first and last rows of a result set.
 * These boundaries are used to build WHERE clauses for subsequent queries.
 *
 * @param ks    Keyset state
 * @param data  Result set from which to extract boundaries
 */
void lace_keyset_extract_boundaries(LaceKeysetState *ks, const LaceResult *data);

/*
 * Clear boundary values (e.g., when jumping to a specific offset).
 *
 * @param ks  Keyset state
 */
void lace_keyset_clear_boundaries(LaceKeysetState *ks);

/*
 * Check if cursor is at the start of data (no more rows before).
 *
 * @param ks  Keyset state
 * @return    true if at start
 */
bool lace_keyset_at_start(const LaceKeysetState *ks);

/*
 * Check if cursor is at the end of data (no more rows after).
 *
 * @param ks  Keyset state
 * @return    true if at end
 */
bool lace_keyset_at_end(const LaceKeysetState *ks);

/*
 * Set the at_start flag (after loading first page or detecting beginning).
 *
 * @param ks        Keyset state
 * @param at_start  Whether cursor is at start
 */
void lace_keyset_set_at_start(LaceKeysetState *ks, bool at_start);

/*
 * Set the at_end flag (after detecting no more rows).
 *
 * @param ks      Keyset state
 * @param at_end  Whether cursor is at end
 */
void lace_keyset_set_at_end(LaceKeysetState *ks, bool at_end);

/*
 * Check if keyset has valid boundary values for forward pagination.
 *
 * @param ks  Keyset state
 * @return    true if has valid last boundary
 */
bool lace_keyset_has_last_boundary(const LaceKeysetState *ks);

/*
 * Check if keyset has valid boundary values for backward pagination.
 *
 * @param ks  Keyset state
 * @return    true if has valid first boundary
 */
bool lace_keyset_has_first_boundary(const LaceKeysetState *ks);

/* ==========================================================================
 * SQL Generation (Internal helpers exposed for testing)
 * ========================================================================== */

/*
 * Build WHERE clause for keyset pagination.
 * For forward: WHERE (pk1, pk2) > (v1, v2) or expanded form for MySQL
 * For backward: WHERE (pk1, pk2) < (v1, v2)
 *
 * @param ks         Keyset state
 * @param forward    true for forward pagination, false for backward
 * @return           Allocated WHERE clause string (caller must free), or NULL
 */
char *lace_keyset_build_where(const LaceKeysetState *ks, bool forward);

/*
 * Build ORDER BY clause with keyset columns and PK tiebreaker.
 * For forward: ORDER BY col1 ASC, col2 ASC, pk ASC
 * For backward: ORDER BY col1 DESC, col2 DESC, pk DESC
 *
 * @param ks       Keyset state
 * @param forward  true for forward pagination, false for backward
 * @return         Allocated ORDER BY clause string (caller must free), or NULL
 */
char *lace_keyset_build_order(const LaceKeysetState *ks, bool forward);

/* ==========================================================================
 * Query Execution
 * ========================================================================== */

/*
 * Execute a keyset pagination query.
 *
 * This function:
 * 1. Builds the keyset WHERE clause using boundary values
 * 2. Combines with user-provided filters
 * 3. Builds ORDER BY with keyset columns
 * 4. Executes the query
 * 5. For backward queries, reverses the result rows before returning
 *
 * @param client       Client handle
 * @param conn_id      Connection ID
 * @param table        Table name
 * @param ks           Keyset state (must have boundaries for non-first-page)
 * @param forward      true for forward pagination, false for backward
 * @param filters      User filters (NULL for none)
 * @param num_filters  Number of filters
 * @param limit        Maximum rows to return
 * @param result       Output: result set (caller must free with lace_result_free)
 * @return             LACE_OK on success, error code on failure
 */
int lace_query_keyset(lace_client_t *client, int conn_id,
                      const char *table,
                      LaceKeysetState *ks, bool forward,
                      const char *where_clause,
                      size_t limit,
                      LaceResult **result);

/* ==========================================================================
 * Utility
 * ========================================================================== */

/*
 * Get the number of keyset columns.
 *
 * @param ks  Keyset state
 * @return    Number of keyset columns (sort cols + PK cols)
 */
size_t lace_keyset_num_columns(const LaceKeysetState *ks);

/*
 * Get a keyset column index.
 *
 * @param ks   Keyset state
 * @param idx  Index within keyset columns (0..num_columns-1)
 * @return     Column index in schema, or SIZE_MAX if invalid
 */
size_t lace_keyset_column_index(const LaceKeysetState *ks, size_t idx);

#ifdef __cplusplus
}
#endif

#endif /* LIBLACE_KEYSET_H */
