/*
 * Lace
 * Addressing one row for UPDATE / DELETE: its primary key, and the check
 * that the row in the database is still the one on screen
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_ROW_KEY_H
#define LACE_ROW_KEY_H

#include "db_compat.h"

typedef struct {
  const char **col_names; /* Key column names (borrowed from the result) */
  DbValue *values;        /* Key values (owned) */
  size_t count;
} RowKey;

typedef enum {
  ROW_KEY_OK,
  ROW_KEY_NONE, /* No usable key: the table has none, or the data lacks it */
  ROW_KEY_BAD,  /* A key part is NULL, NaN or infinite: cannot address it */
} RowKeyStatus;

/* Key of row `row` of data: the schema's primary-key columns, found by NAME
 * in data (schema and result positions differ: generated or hidden columns,
 * a query's own column list). With unique_names, every key column must
 * appear exactly once in data (a query can repeat a name: SELECT *, id).
 * *why gets a static reason unless ROW_KEY_OK. */
RowKeyStatus row_key_build(RowKey *key, const ResultSet *data, size_t row,
                           const TableSchema *schema, bool unique_names,
                           const char **why);

void row_key_free(RowKey *key);

/* Before writing: re-read the row by key, exactly as the UPDATE or DELETE
 * will address it. It must be one row, and column `col` of data, or with
 * col == SIZE_MAX every column of data that is a column of schema (values
 * too large to load are skipped), must still hold the value data shows.
 * Catches rows changed since loading, and computed or aliased columns.
 * *why is allocated on failure. */
bool row_key_verify(DbConnection *conn, const char *table, const RowKey *key,
                    const ResultSet *data, size_t row, size_t col,
                    const TableSchema *schema, char **why);

#endif /* LACE_ROW_KEY_H */
