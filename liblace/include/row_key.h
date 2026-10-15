/*
 * liblace - Lace Client Library
 * Addressing one row for UPDATE / DELETE: its primary key, found by column
 * name in a result, and the check that the row in the database is still the
 * one on screen
 *
 * A result's columns and a schema's columns are matched by NAME, never by
 * position: a query's result may order, alias or repeat them. A key part
 * that is NULL, NaN, infinite or too large to load cannot address a row and
 * is refused. Before a write, lace_row_key_verify() re-reads the row with
 * lace_select_by_pk() (the same WHERE the mutation uses) and compares the
 * values shown to the user as exact edit text.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_ROW_KEY_H
#define LIBLACE_ROW_KEY_H

#include "lace_export.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

LACE_BEGIN_DECLS

typedef struct lace_client lace_client_t;

typedef struct {
  char **names;      /* Key column names (owned copies) */
  LaceValue *values; /* Key values (owned) */
  size_t count;
} LaceRowKey;

typedef enum {
  LACE_ROW_KEY_OK,
  LACE_ROW_KEY_NONE, /* No usable key: the table has none, or the data lacks it */
  LACE_ROW_KEY_BAD,  /* A key part is NULL, NaN or infinite: cannot address it */
} LaceRowKeyStatus;

/* Key of row `row` of data: the schema's primary-key columns, found by name
 * in data. With unique_names, every key column must appear exactly once in
 * data (SELECT *, id repeats one). *why gets a static reason unless
 * LACE_ROW_KEY_OK. */
LaceRowKeyStatus lace_row_key_build(LaceRowKey *key, const LaceResult *data,
                                    size_t row, const LaceSchema *schema,
                                    bool unique_names, const char **why);

/* Deep copy (the key must outlive the result it was built from) */
void lace_row_key_copy(LaceRowKey *dst, const LaceRowKey *src);
void lace_row_key_free(LaceRowKey *key);

/* The key as liblace's primary-key array (borrows names and values; free
 * the array with free()) */
LacePkValue *lace_row_key_to_pk(const LaceRowKey *key);

/* A comparable snapshot of the columns of row `row` that a verification
 * checks: column `col` of data, or (col == SIZE_MAX) every column of data
 * that is a column of schema (values too large to load are skipped). */
typedef struct {
  char **names;
  LaceValue *values;
  size_t count;
} LaceRowSnapshot;

void lace_row_snapshot_build(LaceRowSnapshot *snap, const LaceResult *data,
                             size_t row, size_t col, const LaceSchema *schema);
void lace_row_snapshot_free(LaceRowSnapshot *snap);

/* Re-read the row by key, exactly as the UPDATE or DELETE will address it:
 * it must be one row whose snapshot columns still hold the values on screen
 * (compared as exact edit text). Blocks on the daemon (call it off the UI
 * thread). *why is allocated on failure. */
bool lace_row_key_verify(lace_client_t *client, int conn_id, const char *table,
                         const LaceRowKey *key, const LaceRowSnapshot *snap,
                         char **why);

LACE_END_DECLS

#endif /* LIBLACE_ROW_KEY_H */
