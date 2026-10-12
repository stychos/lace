/*
 * Lace
 * Database type definitions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DB_TYPES_H
#define LACE_DB_TYPES_H

#include "../constants.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Database value types */
typedef enum {
  DB_TYPE_NULL,
  DB_TYPE_INT,
  DB_TYPE_FLOAT,
  DB_TYPE_TEXT,
  DB_TYPE_BLOB,
  DB_TYPE_BOOL,
  DB_TYPE_DATE,
  DB_TYPE_TIMESTAMP,
  DB_TYPE_DECIMAL /* Exact numeric (NUMERIC/DECIMAL), kept as its text */
} DbValueType;

/* A single database value */
typedef struct {
  DbValueType type;
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
  bool is_null;
  size_t omitted;  /* > 0: a value this large was not loaded; text holds a
                      "[TEXT: N bytes]" label (read-only for clients) */
} DbValue;

/* Column definition */
typedef struct {
  char *name;
  DbValueType type;
  char *type_name; /* Original type name from DB */
  bool nullable;
  bool primary_key;
  bool auto_increment;
  char *default_val;
  char *foreign_key; /* "table.column" or NULL */
  int max_length;    /* For VARCHAR etc, -1 if unlimited */
} ColumnDef;

/* A single row of data */
typedef struct {
  DbValue *cells;
  size_t num_cells;
} Row;

/* Result set from a query */
typedef struct {
  ColumnDef *columns;
  size_t num_columns;
  Row *rows;
  size_t num_rows;
  size_t total_rows;     /* Total matching rows (for pagination) */
  int64_t rows_affected; /* For INSERT/UPDATE/DELETE */
  char *error;           /* Error message if any */
  bool truncated;        /* Stopped at LACE_MAX_RESULT_ROWS / LACE_MAX_RESULT_BYTES */
} ResultSet;

/* Helper functions */
const char *db_value_type_name(DbValueType type);

/* Memory management */
void db_value_free(DbValue *val);
void db_row_free(Row *row);
void db_result_free(ResultSet *rs);

/* Allocate an empty result set (for non-SELECT statements) */
ResultSet *db_result_alloc_empty(void);

void db_column_free(ColumnDef *col);

DbValue db_value_oversized_placeholder(const char *type_label, size_t size);
DbValue db_value_copy(const DbValue *src);

#endif /* LACE_DB_TYPES_H */
