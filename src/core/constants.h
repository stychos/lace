/*
 * Lace
 * Centralized application constants
 *
 * All magic numbers and limits are defined here for easy tuning.
 * Domain-specific enums and platform detection remain in their respective headers.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONSTANTS_H
#define LACE_CONSTANTS_H

/* ==========================================================================
 * Memory & Buffer Sizes
 * ========================================================================== */

/* Maximum field size before truncation (1MB) - DoS/OOM protection */
#define MAX_FIELD_SIZE (1024 * 1024)

/* Maximum rows for a result set (1M rows) - prevents unbounded memory growth */
#define MAX_RESULT_ROWS (1024 * 1024)

/* Connection string maximum length */
#define MAX_CONNSTR_LEN 4096

/* Query editor initial buffer capacity */
#define QUERY_INITIAL_CAPACITY 1024

/* String builder defaults */
#define SB_INITIAL_CAP 64
#define SB_GROWTH_FACTOR 2

/* Dynamic array default capacity */
#define DYNARRAY_INITIAL_CAP 8

/* ==========================================================================
 * Pagination
 * ========================================================================== */

/* Page size for data loading */
#define PAGE_SIZE 1000

/* Config validation bounds for user-configurable page size */
#define CONFIG_PAGE_SIZE_MIN 10
#define CONFIG_PAGE_SIZE_MAX 10000
#define CONFIG_PAGE_SIZE_DEFAULT 500

/* Number of pages to prefetch */
#define PREFETCH_PAGES 2
#define CONFIG_PREFETCH_PAGES_MIN 1
#define CONFIG_PREFETCH_PAGES_MAX 10
#define CONFIG_PREFETCH_PAGES_DEFAULT 2

/* Load more data when within this many rows of edge */
#define LOAD_THRESHOLD 50

/* Start prefetch when within 1 page of edge */
#define PREFETCH_THRESHOLD PAGE_SIZE

/* Maximum pages to keep in memory (~5000 rows at default page size) */
#define MAX_LOADED_PAGES 5

/* Trim data farther than this many pages from cursor */
#define TRIM_DISTANCE_PAGES 2

/* Maximum result rows for config validation */
#define CONFIG_MAX_RESULT_ROWS_MIN 1000
#define CONFIG_MAX_RESULT_ROWS_MAX (10 * 1024 * 1024) /* 10M rows */
#define CONFIG_MAX_RESULT_ROWS_DEFAULT (1024 * 1024)  /* 1M rows */

/* ==========================================================================
 * Column Display
 * ========================================================================== */

#define MIN_COL_WIDTH 4
#define MAX_COL_WIDTH 40
#define DEFAULT_COL_WIDTH 15

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
 * Database Limits
 * ========================================================================== */

/* Maximum registered database drivers */
#define MAX_DRIVERS 16

/* Maximum primary key columns */
#define MAX_PK_COLUMNS 16

/* Maximum transaction nesting depth */
#define MAX_TRANSACTION_DEPTH 100

/* Maximum iterations consuming MySQL results */
#define MAX_RESULT_CONSUME_ITERATIONS 1000

/* Default ports */
#define DEFAULT_PORT_POSTGRES 5432
#define DEFAULT_PORT_MYSQL 3306

/* ==========================================================================
 * History
 * ========================================================================== */

#define HISTORY_SIZE_MIN 10
#define HISTORY_SIZE_MAX 100000
#define HISTORY_SIZE_DEFAULT 1000
#define HISTORY_INITIAL_CAPACITY 32

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

#endif /* LACE_CONSTANTS_H */
