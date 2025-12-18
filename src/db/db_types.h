/*
 * lace - Database Viewer and Manager
 * Database type definitions
 */

#ifndef LACE_DB_TYPES_H
#define LACE_DB_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Database value types */
typedef enum {
    DB_TYPE_NULL,
    DB_TYPE_INT,
    DB_TYPE_FLOAT,
    DB_TYPE_TEXT,
    DB_TYPE_BLOB,
    DB_TYPE_BOOL,
    DB_TYPE_DATE,
    DB_TYPE_TIMESTAMP
} DbValueType;

/* A single database value */
typedef struct {
    DbValueType type;
    union {
        int64_t     int_val;
        double      float_val;
        struct {
            char   *data;
            size_t  len;
        } text;
        struct {
            uint8_t *data;
            size_t   len;
        } blob;
        bool        bool_val;
    };
    bool is_null;
} DbValue;

/* Column definition */
typedef struct {
    char        *name;
    DbValueType  type;
    char        *type_name;     /* Original type name from DB */
    bool         nullable;
    bool         primary_key;
    bool         auto_increment;
    char        *default_val;
    char        *foreign_key;   /* "table.column" or NULL */
    int          max_length;    /* For VARCHAR etc, -1 if unlimited */
} ColumnDef;

/* A single row of data */
typedef struct {
    DbValue    *cells;
    size_t      num_cells;
} Row;

/* Result set from a query */
typedef struct {
    ColumnDef  *columns;
    size_t      num_columns;
    Row        *rows;
    size_t      num_rows;
    size_t      total_rows;     /* Total matching rows (for pagination) */
    int64_t     rows_affected;  /* For INSERT/UPDATE/DELETE */
    char       *error;          /* Error message if any */
} ResultSet;

/* Index definition */
typedef struct {
    char       *name;
    char      **columns;
    size_t      num_columns;
    bool        unique;
    bool        primary;
    char       *type;           /* BTREE, HASH, etc */
} IndexDef;

/* Foreign key definition */
typedef struct {
    char       *name;
    char      **columns;
    size_t      num_columns;
    char       *ref_table;
    char      **ref_columns;
    size_t      num_ref_columns;
    char       *on_delete;      /* CASCADE, SET NULL, etc */
    char       *on_update;
} ForeignKeyDef;

/* Complete table schema */
typedef struct {
    char          *name;
    char          *schema;      /* Schema/database name */
    ColumnDef     *columns;
    size_t         num_columns;
    IndexDef      *indexes;
    size_t         num_indexes;
    ForeignKeyDef *foreign_keys;
    size_t         num_foreign_keys;
    int64_t        row_count;   /* Approximate row count */
} TableSchema;

/* Database information */
typedef struct {
    char       *name;
    char       *charset;
    char       *collation;
} DatabaseInfo;

/* Connection status */
typedef enum {
    CONN_STATUS_DISCONNECTED,
    CONN_STATUS_CONNECTING,
    CONN_STATUS_CONNECTED,
    CONN_STATUS_ERROR
} ConnStatus;

/* Helper functions */
const char *db_value_type_name(DbValueType type);
DbValueType db_value_type_from_name(const char *name);

/* Memory management */
void db_value_free(DbValue *val);
void db_row_free(Row *row);
void db_result_free(ResultSet *rs);
void db_column_free(ColumnDef *col);
void db_index_free(IndexDef *idx);
void db_fk_free(ForeignKeyDef *fk);
void db_schema_free(TableSchema *schema);

/* Value creation helpers */
DbValue db_value_null(void);
DbValue db_value_int(int64_t val);
DbValue db_value_float(double val);
DbValue db_value_text(const char *str);
DbValue db_value_text_len(const char *str, size_t len);
DbValue db_value_blob(const uint8_t *data, size_t len);
DbValue db_value_bool(bool val);

/* Value conversion */
char *db_value_to_string(const DbValue *val);
bool db_value_to_bool(const DbValue *val);
int64_t db_value_to_int(const DbValue *val);
double db_value_to_float(const DbValue *val);

#endif /* LACE_DB_TYPES_H */
