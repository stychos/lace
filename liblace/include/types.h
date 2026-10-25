/*
 * liblace - Lace Client Library
 * Shared type definitions for daemon and clients
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_TYPES_H
#define LIBLACE_TYPES_H

#include "lace_export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"

LACE_BEGIN_DECLS

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
  char *collation;     /* The column's own collation; NULL for the type's or
                          table's default (SQLite: read from the CREATE text) */
  char *comment;       /* MySQL and PostgreSQL column comment; NULL when none */
  int identity;        /* PostgreSQL identity column: 0 none, 1 ALWAYS, 2 BY DEFAULT */
  char *generated_expr;/* The expression of a generated column, NULL otherwise */
  int key_position;    /* 1-based position in the primary key, 0 when not part of it */
} LaceColumn;

/* What an index entry is: an index, or a constraint the catalog lists with
 * the indexes (a check or exclusion constraint has no index of its own) */
typedef enum {
  LACE_INDEX_PLAIN = 0,
  LACE_INDEX_UNIQUE,
  LACE_INDEX_PRIMARY,
  LACE_INDEX_CHECK,     /* columns empty; check holds the condition */
  LACE_INDEX_EXCLUSION  /* PostgreSQL EXCLUDE; check holds the definition */
} LaceIndexKind;

/* Index definition */
typedef struct {
  char *name;
  char **columns;      /* in index order; an expression column is its text */
  size_t num_columns;
  bool unique;
  bool primary;
  char *type;          /* BTREE, HASH, etc */
  LaceIndexKind kind;
  char *where;         /* A partial index's predicate, NULL otherwise */
  char *check;         /* LACE_INDEX_CHECK: the condition; EXCLUSION: the definition */
  char *constraint;    /* The constraint this index backs (its name), NULL otherwise */
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
  bool deferrable;     /* PostgreSQL */
  bool initially_deferred;
} LaceForeignKey;

/* A table whose foreign key references another (lace_table_dependents) */
typedef struct {
  char *table;     /* table reference, as lace_list_tables gives it */
  char *on_delete; /* CASCADE, SET NULL, SET DEFAULT, RESTRICT or NO ACTION */
  char *name;      /* the constraint's name; NULL on SQLite */
  char **columns;  /* the referencing table's columns, in key order */
  char **ref_columns; /* the referenced columns of this table, same order */
  size_t num_columns;
} LaceDependent;

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

/* ---- Catalog objects besides tables (lace_list_objects) ---- */
typedef enum { LACE_OBJ_TABLE = 0, LACE_OBJ_VIEW, LACE_OBJ_ROUTINE, LACE_OBJ_TRIGGER } LaceObjectKind;

typedef struct {
  LaceObjectKind kind;
  char *name;   /* A view: a table reference. A routine: its name; PostgreSQL
                   name(argtypes) as regprocedure spells it, schema-qualified
                   outside the search path. A trigger: the trigger's name. */
  char *table;  /* A trigger's table (a reference); NULL otherwise */
  char *detail; /* A routine: "function" or "procedure"; a materialized view:
                   "materialized"; else NULL */
} LaceObject;

/* What the Information view shows (lace_table_info); -1 / NULL: unknown */
typedef struct {
  int64_t row_estimate;  /* The catalog's estimate, not a count */
  int64_t data_bytes, index_bytes, total_bytes;
  bool size_is_file;     /* SQLite without dbstat: total_bytes is the whole file */
  bool partitioned;
  char *engine;          /* MySQL */
  char *charset;         /* MySQL table default; PostgreSQL the database's encoding */
  char *collation;       /* MySQL table default; PostgreSQL the database's */
  char *comment;         /* MySQL, PostgreSQL */
  char *owner;           /* PostgreSQL */
  char *schema;          /* PostgreSQL schema, MySQL database, SQLite "main" */
  char *tablespace;      /* PostgreSQL */
  char *created, *updated;   /* MySQL CREATE_TIME / UPDATE_TIME */
  char *stats_updated;       /* PostgreSQL: the last analyze or autoanalyze */
} LaceTableInfo;

/* A table's auto-increment or identity counter (lace_autoincrement_get) */
typedef struct {
  char *column;      /* NULL: the table has no such column */
  char *sequence;    /* PostgreSQL: the sequence behind it; NULL otherwise */
  bool has_counter;  /* false: SQLite INTEGER PRIMARY KEY without AUTOINCREMENT
                        (the next rowid is max + 1, nothing to reset) */
  int64_t next;      /* The value the database assigns next, -1 unknown */
  int64_t max;       /* The column's current maximum, -1 when the table is empty */
} LaceAutoInc;

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
 * Sort Types (keyset paging)
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
/* data NULL: a NULL value; len 0 with data: an empty BLOB (not NULL) */
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

LACE_END_DECLS

#endif /* LIBLACE_TYPES_H */
