/*
 * Lace TUI
 * Tuning constants of the frontend (shared limits come from liblace)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_TUI_CONSTANTS_H
#define LACE_TUI_CONSTANTS_H

#include "../../../liblace/include/constants.h"

/* ==========================================================================
 * Pagination
 * ========================================================================== */

/* Query editor initial buffer capacity */
#define QUERY_INITIAL_CAPACITY 1024

/* How table tabs load the page next to the rows on screen */
typedef enum {
  LACE_PAGINATION_OFFSET = 0, /* LIMIT/OFFSET only */
  LACE_PAGINATION_KEYSET = 1  /* Keyset from the edge row when the table has
                                 a primary key, else OFFSET (default) */
} LacePaginationStrategy;

/* Page size for data loading */
#define TUI_PAGE_SIZE 1000
#define TUI_LOG_MAX 500 /* status messages the log keeps (as the notcurses TUI) */

/* Number of pages to prefetch */
#define PREFETCH_PAGES 2

/* Load more data when within this many rows of edge */
#define LOAD_THRESHOLD 50

/* Maximum pages to keep in memory (~5000 rows at default page size) */
#define MAX_LOADED_PAGES 5

/* Trim data farther than this many pages from cursor */
#define TRIM_DISTANCE_PAGES 2

/* ==========================================================================
 * Column Display
 * ========================================================================== */

#define MIN_COL_WIDTH 4
#define DEFAULT_COL_WIDTH 42

/* ==========================================================================
 * UI Dimensions
 * ========================================================================== */

/* Sidebar */
#define SIDEBAR_WIDTH 20

/* Tab bar */
#define TAB_BAR_HEIGHT 1

/* Terminal minimum dimensions */
#define MIN_TERM_ROWS 10
#define MIN_TERM_COLS 40

/* Maximum visible items */
#define MAX_VISIBLE_FILTERS 8
#define MAX_VISIBLE_COLUMNS 256

/* Spinner animation frame count */
#define SPINNER_COUNT 4

/* Polling interval for async operations (ms) */
#define POLL_INTERVAL_MS 50

/* ==========================================================================
 * Limits
 * ========================================================================== */

/* ==========================================================================
 * Daemon
 * ========================================================================== */

/* Daemon idle timeout (seconds, 0=disabled) */
#define CONFIG_IDLE_TIMEOUT_MIN 0
#define CONFIG_IDLE_TIMEOUT_MAX 86400
#define CONFIG_IDLE_TIMEOUT_DEFAULT 0

/* ==========================================================================
 * History
 * ========================================================================== */

#define HISTORY_SIZE_MIN 10
#define HISTORY_SIZE_MAX 100000
#define HISTORY_SIZE_DEFAULT 1000

/* ==========================================================================
 * Data Structure Capacities
 * ========================================================================== */

/* AppState initial capacities */
#define INITIAL_CONNECTION_CAPACITY 4
#define INITIAL_WORKSPACE_CAPACITY 4
#define INITIAL_TAB_CAPACITY 8
#define INITIAL_SELECTION_CAPACITY 16

/* TUI state initial capacities */
#define INITIAL_TAB_UI_WS_CAPACITY 4
#define INITIAL_TAB_UI_TAB_CAPACITY 8

/* ==========================================================================
 * Application Limits
 * ========================================================================== */

/* Maximum columns for multi-column sort */
#define MAX_SORT_COLUMNS 8

/* Maximum values in IN() filter clause */
#define MAX_IN_VALUES 1000

/* Maximum folder nesting depth for saved connections */
#define MAX_FOLDER_DEPTH 100

/* History file version */
#define HISTORY_VERSION 1

#endif /* LACE_TUI_CONSTANTS_H */
