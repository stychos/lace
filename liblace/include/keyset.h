/*
 * liblace - Lace Client Library
 * Keyset (cursor-based) pagination
 *
 * A page after (or before) a row is read with WHERE <after that row> ORDER
 * BY <sort columns, then the primary key> LIMIT n instead of LIMIT n OFFSET
 * m, so its cost does not grow with the position in the table. The state
 * is only the ordering: the edge row of the rows already loaded is passed
 * in each time, so nothing goes stale when data, sorting or filters change.
 *
 *   LaceKeysetState *ks = lace_keyset_create(schema, sorts, n, driver);
 *   if (ks) {
 *     char *order = lace_keyset_build_order(ks, true);        // every page
 *     char *after = lace_keyset_build_where(ks, rows, last, true);
 *     // SELECT * FROM t WHERE <filters> AND <after> ORDER BY <order> LIMIT n
 *     // (after == NULL: that row cannot be a boundary; use OFFSET instead)
 *   }
 *
 * Pages read with OFFSET must use the same ORDER BY, so both kinds of page
 * agree on where each row is.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_KEYSET_H
#define LIBLACE_KEYSET_H

#include "lace_export.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

LACE_BEGIN_DECLS

/* The ordering of a keyset walk (opaque) */
typedef struct LaceKeysetState LaceKeysetState;

/*
 * The ordering for a table: the sort columns (indices into schema), then
 * every primary-key column not already among them, ascending, so the order
 * is total. NULL if the table has no primary key, a sort column is not in
 * the schema, or a column cannot be compared exactly with a literal (a
 * floating-point column on PostgreSQL or MySQL: FLOAT/real is single
 * precision there, and its value never equals the literal read back).
 */
LaceKeysetState *lace_keyset_create(const LaceSchema *schema,
                                    const LaceSort *sorts, size_t num_sorts,
                                    LaceDriver driver);

void lace_keyset_free(LaceKeysetState *ks);

/*
 * ORDER BY body (without the keywords) for the walk: forward is the
 * table's order; backward is the exact reverse (reverse the rows read).
 * Allocated; NULL on allocation failure.
 */
char *lace_keyset_build_order(const LaceKeysetState *ks, bool forward);

/*
 * WHERE condition for the rows after (forward) or before (backward) row
 * `row` of data, in the walk's order, following each driver's NULL
 * ordering (PostgreSQL puts NULLs last for ASC, MySQL and SQLite first).
 * Columns are found in data by name. NULL if that row cannot be a
 * boundary: a keyset column is missing from data, or a value has no exact
 * literal (too large to load, NaN or infinite). Allocated.
 */
char *lace_keyset_build_where(const LaceKeysetState *ks, const LaceResult *data,
                              size_t row, bool forward);

/* Number of columns in the ordering (sort columns + key columns) */
size_t lace_keyset_num_columns(const LaceKeysetState *ks);

LACE_END_DECLS

#endif /* LIBLACE_KEYSET_H */
