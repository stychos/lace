/*
 * Lace
 * Common database driver utilities
 *
 * This module provides shared implementations for operations that are
 * nearly identical across database drivers, reducing code duplication.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_COMMON_H
#define LACE_DB_COMMON_H

#include "util/str.h"
#include "db_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
typedef struct DbConnection DbConnection;

/* Free common DbConnection fields (but not driver_data).
 * Call this from driver disconnect functions after freeing driver_data.
 * Handles: connstr (secure), database, host, user, and conn itself.
 */
void db_common_free_connection(DbConnection *conn);

/* True if sql (ignoring leading space and comments) starts with word,
 * case-insensitively and as a whole word */
bool db_common_sql_starts_with_word(const char *sql, const char *word);

/* Collecting a result's rows under the daemon's limits (LACE_MAX_RESULT_ROWS
 * rows, LACE_MAX_RESULT_BYTES of cell data), the same for every driver */
typedef struct {
  ResultSet *rs;
  size_t bytes; /* Cell data so far, plus a per-cell overhead */
  size_t cap;   /* Allocated rows */
} DbRowCollector;

/* Start collecting into rs (its columns are set by the caller) */
void db_rows_init(DbRowCollector *rc, ResultSet *rs);

/* The zeroed cells of a new row of num_cells, or NULL when a limit is
 * reached: then rs->truncated is set and the caller stops reading (and
 * stops the server, for a streamed result). Count each cell's size with
 * db_rows_count(). */
DbValue *db_rows_add(DbRowCollector *rc, size_t num_cells);

static inline void db_rows_count(DbRowCollector *rc, size_t cell_bytes) {
  rc->bytes += cell_bytes + 16;
}

/* A network connection that can be lost, and the transaction lost with it.
 * Lost with no transaction open, it is quietly re-established before the
 * next statement (the statement that found it dead still fails: nobody
 * knows whether it ran). Lost with a transaction open, its work is gone:
 * statements are refused until the client acknowledges with ROLLBACK (or
 * ABORT), because carrying on would run them in autocommit on a new
 * session and a COMMIT would "succeed" with nothing in it. */
typedef struct {
  bool broken;  /* Re-establish before the next statement */
  bool tx_lost; /* ... and a transaction went with it */
} DbLinkState;

typedef enum {
  DB_LINK_READY,     /* Run the statement */
  DB_LINK_RECONNECT, /* Re-establish first, then run it */
  DB_LINK_ACK,       /* A ROLLBACK acknowledged the loss: re-establish and
                        answer it without running it */
  DB_LINK_REFUSE,    /* Refused (*err set to lost_msg) */
} DbLinkAction;

/* The connection was found dead; tx_open: a transaction was open on it */
void db_link_lost(DbLinkState *link, bool tx_open);

/* Before a statement */
DbLinkAction db_link_gate(DbLinkState *link, const char *sql,
                          const char *lost_msg, char **err);

/* After a successful re-establish */
static inline void db_link_restored(DbLinkState *link) {
  link->broken = false;
  link->tx_lost = false;
}

#endif /* LACE_DB_COMMON_H */
