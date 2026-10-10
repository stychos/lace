/*
 * liblace - Lace Client Library
 * Shared type definitions for daemon and clients
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_TYPES_H
#define LIBLACE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * Limits
 * ========================================================================== */

/* Maximum field size before truncation (1MB) - DoS/OOM protection */
#define LACE_MAX_FIELD_SIZE (1024 * 1024)

/* Maximum rows for a result set (1M rows) */
#define LACE_MAX_RESULT_ROWS (1024 * 1024)

/* Maximum primary key columns */
#define LACE_MAX_PK_COLUMNS 16

/* ==========================================================================
 * Database Value Types
 * ========================================================================== */

typedef enum {
  LACE_TYPE_NULL,
  LACE_TYPE_INT,
  LACE_TYPE_FLOAT,
  LACE_TYPE_TEXT,
  LACE_TYPE_BLOB,
  LACE_TYPE_BOOL,
  LACE_TYPE_DATE,
  LACE_TYPE_TIMESTAMP,
  LACE_TYPE_DECIMAL  /* Exact numeric (NUMERIC/DECIMAL) kept as its digits in text */
} LaceValueType;

/* A single database value */
typedef struct {
  LaceValueType type;
  bool is_null;
  size_t omitted;   /* > 0: a value of this many bytes was too large to load;
                       text holds a "[TEXT: N bytes]" label (never write it) */
  union {
    int64_t int_val;
    double float_val;
    struct {
      char *data;
      size_t len;
    } text;
    struct {
      uint8_t *data;
      size_t len;
    } blob;
    bool bool_val;
  };
} LaceValue;

/* ==========================================================================
 * Schema Types
 * ========================================================================== */

/* Column definition */
typedef struct {
  char *name;
  LaceValueType type;
  char *type_name;     /* Original type name from DB (e.g., "VARCHAR(255)") */
  bool nullable;
  bool primary_key;
  bool auto_increment;
  bool generated;      /* Computed by the database (GENERATED ... AS): never
                          inserted or updated */
  char *default_val;
  char *foreign_key;   /* "table.column" or NULL */
  int max_length;      /* For VARCHAR etc, -1 if unlimited */
} LaceColumn;

/* Index definition */
typedef struct {
  char *name;
  char **columns;
  size_t num_columns;
  bool unique;
  bool primary;
  char *type;          /* BTREE, HASH, etc */
} LaceIndex;

/* Foreign key definition */
typedef struct {
  char *name;
  char **columns;
  size_t num_columns;
  char *ref_table;
  char **ref_columns;
  size_t num_ref_columns;
  char *on_delete;     /* CASCADE, SET NULL, etc */
  char *on_update;
} LaceForeignKey;

/* Complete table schema */
typedef struct {
  char *name;
  char *schema;        /* Schema/database name */
  LaceColumn *columns;
  size_t num_columns;
  LaceIndex *indexes;
  size_t num_indexes;
  LaceForeignKey *foreign_keys;
  size_t num_foreign_keys;
  int64_t row_count;   /* Approximate row count, -1 if unknown */
} LaceSchema;

/* ==========================================================================
 * Result Set
 * ========================================================================== */

/* A single row of data */
typedef struct {
  LaceValue *cells;
  size_t num_cells;
} LaceRow;

/* Result set from a query */
typedef struct {
  LaceColumn *columns;
  size_t num_columns;
  LaceRow *rows;
  size_t num_rows;
  bool truncated;         /* The daemon stopped at its row or size limit */
  bool in_transaction;    /* A transaction is open on the connection after
                             this statement (as reported by the daemon) */
  size_t total_rows;      /* Total matching rows (for pagination info) */
  int64_t rows_affected;  /* For INSERT/UPDATE/DELETE, -1 for SELECT */
  bool has_more;          /* More rows available beyond this result */
  char *source_table;     /* Detected source table (for edit support), NULL if unknown */
} LaceResult;

/* ==========================================================================
 * Filter Types
 * ========================================================================== */

typedef enum {
  LACE_FILTER_EQ,           /* = */
  LACE_FILTER_NE,           /* <> */
  LACE_FILTER_GT,           /* > */
  LACE_FILTER_GE,           /* >= */
  LACE_FILTER_LT,           /* < */
  LACE_FILTER_LE,           /* <= */
  LACE_FILTER_IN,           /* IN (value list) */
  LACE_FILTER_CONTAINS,     /* LIKE '%value%' */
  LACE_FILTER_REGEX,        /* REGEXP/~ (driver-specific) */
  LACE_FILTER_BETWEEN,      /* BETWEEN value AND value2 */
  LACE_FILTER_IS_EMPTY,     /* = '' */
  LACE_FILTER_IS_NOT_EMPTY, /* <> '' */
  LACE_FILTER_IS_NULL,      /* IS NULL */
  LACE_FILTER_IS_NOT_NULL,  /* IS NOT NULL */
  LACE_FILTER_RAW,          /* Raw SQL condition */
  LACE_FILTER_COUNT         /* Number of filter operations (must be last) */
} LaceFilterOp;

/* Single column filter */
typedef struct {
  size_t column;         /* Column index */
  LaceFilterOp op;
  char *value;           /* Filter value (for ops that need it) */
  char *value2;          /* Second value (for BETWEEN) */
} LaceFilter;

/* ==========================================================================
 * Sort Types
 * ========================================================================== */

typedef enum {
  LACE_SORT_ASC,
  LACE_SORT_DESC
} LaceSortDir;

typedef struct {
  size_t column;         /* Column index */
  LaceSortDir dir;
} LaceSort;

/* ==========================================================================
 * Primary Key Specification (for updates/deletes)
 * ========================================================================== */

typedef struct {
  char *column;          /* Column name */
  LaceValue value;       /* Column value */
} LacePkValue;

/* ==========================================================================
 * Connection Info
 * ========================================================================== */

typedef enum {
  LACE_DRIVER_SQLITE,
  LACE_DRIVER_POSTGRES,
  LACE_DRIVER_MYSQL,
  LACE_DRIVER_MARIADB
} LaceDriver;

typedef struct {
  int id;                /* Connection ID */
  LaceDriver driver;
  char *database;        /* Database name or path */
  char *host;            /* Host (NULL for SQLite) */
  int port;              /* Port (0 for default) */
  char *user;            /* Username (NULL for SQLite) */
  bool connected;        /* Connection status */
} LaceConnInfo;

/* ==========================================================================
 * Memory Management Functions
 * ========================================================================== */

/* Free a single value's internal data */
void lace_value_free(LaceValue *val);

/* Free a row's cells */
void lace_row_free(LaceRow *row);

/* Free a result set (columns, rows, all data) */
void lace_result_free(LaceResult *result);

/* Free a schema (columns, indexes, foreign keys) */
void lace_schema_free(LaceSchema *schema);

/* Free a filter */
void lace_filter_free(LaceFilter *filter);

/* Free connection info */
void lace_conn_info_free(LaceConnInfo *info);

/* ==========================================================================
 * Value Creation Helpers
 * ========================================================================== */

LaceValue lace_value_null(void);
LaceValue lace_value_int(int64_t val);
LaceValue lace_value_float(double val);
LaceValue lace_value_text(const char *str);
LaceValue lace_value_text_len(const char *str, size_t len);
LaceValue lace_value_blob(const uint8_t *data, size_t len);
LaceValue lace_value_bool(bool val);
LaceValue lace_value_copy(const LaceValue *src);

/* ==========================================================================
 * Value Conversion
 * ========================================================================== */

/* Convert value to string (caller must free result) */
char *lace_value_to_string(const LaceValue *val);

/* Get type name as string */
const char *lace_type_name(LaceValueType type);

/* Get filter operator name as string */
const char *lace_filter_op_name(LaceFilterOp op);

/* Get filter operator SQL representation */
const char *lace_filter_op_sql(LaceFilterOp op);

/* Check if filter operator needs a value */
bool lace_filter_op_needs_value(LaceFilterOp op);

/* Exact text for a double: shortest form that reads back as the same
 * value, '.' as decimal point; "NaN", "Infinity", "-Infinity" */
void lace_format_double(double v, char *buf, size_t size);

/* The full, exact text to put in an edit box (NULL -> ""; BLOB -> all of
 * its bytes as hex). Caller frees. */
char *lace_value_to_edit_text(const LaceValue *val);

/* Turn edited text back into a value for a column whose values are of type
 * `like`: hex for BLOB (bytes), exact digits for DECIMAL, an integer for INT
 * when it is one; otherwise text the database converts. Returns false (with
 * *err) for input that cannot be such a value (e.g. bad hex). */
bool lace_value_from_edit_text(const char *text, LaceValueType like,
                               LaceValue *out, char **err);

#endif /* LIBLACE_TYPES_H */
