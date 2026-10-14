/*
 * Lace GTK frontend
 * Addressing one row for UPDATE / DELETE: its primary key, and the check
 * that the row in the database is still the one on screen (pure C)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_ROW_KEY_H
#define LACE_GTK_ROW_KEY_H

#include <lace.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char **names;      /* Key column names (owned copies) */
  LaceValue *values; /* Key values (owned) */
  size_t count;
} RowKey;

typedef enum {
  ROW_KEY_OK,
  ROW_KEY_NONE, /* No usable key: the table has none, or the data lacks it */
  ROW_KEY_BAD,  /* A key part is NULL, NaN or infinite: cannot address it */
} RowKeyStatus;

/* Key of row `row` of data: the schema's primary-key columns, found by NAME
 * in data (schema and result positions differ). With unique_names, every key
 * column must appear exactly once in data (SELECT *, id repeats one). *why
 * gets a static reason unless ROW_KEY_OK. */
RowKeyStatus row_key_build(RowKey *key, const LaceResult *data, size_t row,
                           const LaceSchema *schema, bool unique_names,
                           const char **why);

/* Deep copy (the key must outlive the result it was built from) */
void row_key_copy(RowKey *dst, const RowKey *src);
void row_key_free(RowKey *key);

/* The key as liblace's primary-key array (borrows names and values) */
LacePkValue *row_key_to_pk(const RowKey *key);

/* A comparable snapshot of the columns of row `row` that a verification
 * checks: column `col` of data, or (col == SIZE_MAX) every column of data
 * that is a column of schema (values too large to load are skipped). */
typedef struct {
  char **names;
  LaceValue *values;
  size_t count;
} RowSnapshot;

void row_snapshot_build(RowSnapshot *snap, const LaceResult *data, size_t row,
                        size_t col, const LaceSchema *schema);
void row_snapshot_free(RowSnapshot *snap);

/* Worker thread. Re-read the row by key, exactly as the UPDATE or DELETE
 * will address it: it must be one row whose snapshot columns still hold the
 * values on screen (compared as exact edit text). *why is allocated on
 * failure. */
bool row_key_verify(lace_client_t *client, int conn_id, const char *table,
                    const RowKey *key, const RowSnapshot *snap, char **why);

#endif /* LACE_GTK_ROW_KEY_H */
