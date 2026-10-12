/*
 * Lace
 * Centralized application constants
 *
 * Limits shared by the daemon and the client library. Daemon-only tuning is
 * in laced/src/constants.h, the TUI's in tui/ncurses/src/core/constants.h.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONSTANTS_H
#define LACE_CONSTANTS_H

/* Maximum field size before truncation (1MB) - DoS/OOM protection */
#define LACE_MAX_FIELD_SIZE (1024 * 1024)

/* Maximum primary key columns */
#define LACE_MAX_PK_COLUMNS 16

/* Maximum rows for a result set (1M rows) - prevents unbounded memory growth */
#define LACE_MAX_RESULT_ROWS (1024 * 1024)

/* Cell data (bytes) a single result may hold in the daemon before it is
 * truncated: the result is still turned into JSON and sent, so this bounds
 * the daemon's memory for one query */
#define LACE_MAX_RESULT_BYTES (256u * 1024u * 1024u)

/* Connection string maximum length */
#define LACE_MAX_CONNSTR_LEN 4096

/* String builder defaults */
#define LACE_SB_INITIAL_CAP 64
#define LACE_SB_GROWTH_FACTOR 2

#endif /* LACE_CONSTANTS_H */
