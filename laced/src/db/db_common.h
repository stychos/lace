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

#include "../util/str.h"
#include "db_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
typedef struct DbConnection DbConnection;

/* Quote style for SQL identifiers */
typedef enum {
  DB_QUOTE_DOUBLE,  /* PostgreSQL, SQLite: "identifier" */
  DB_QUOTE_BACKTICK /* MySQL, MariaDB: `identifier` */
} DbQuoteStyle;

/* Escape a table name with optional schema qualification.
 * For PostgreSQL, handles "schema"."table" format.
 * For SQLite/MySQL, handles simple escaping.
 * Returns newly allocated string, caller must free. */
char *db_common_escape_table(const char *table, DbQuoteStyle style,
                             bool support_schema);

/* Escape a single identifier using the specified quote style.
 * Returns newly allocated string, caller must free. */
char *db_common_escape_identifier(const char *name, DbQuoteStyle style);

/* Build a paginated SELECT query.
 * Parameters:
 *   escaped_table - Pre-escaped table name (from db_common_escape_table)
 *   offset, limit - Pagination parameters
 *   order_by      - Column name or pre-built ORDER BY clause (or NULL)
 *   desc          - Sort direction (ignored if order_by is pre-built)
 *   style         - Quote style for order_by column escaping
 *   err           - Error output
 * Returns: Newly allocated SQL string, or NULL on error. */
char *db_common_build_query_page_sql(const char *escaped_table, size_t offset,
                                     size_t limit, const char *order_by,
                                     bool desc, DbQuoteStyle style, char **err);

/* Result of building insert column/value lists */
typedef struct {
  char *col_list;    /* Comma-separated escaped column names */
  char *val_list;    /* Comma-separated placeholders (? or $N) */
  size_t num_params; /* Number of parameters (columns to insert) */
  size_t *col_map;   /* Maps parameter index to original column index */
} DbInsertLists;

/* Build column list and value placeholders for INSERT.
 * Skips auto_increment columns with NULL values.
 * Parameters:
 *   cols, vals, num_cols - Column definitions and values
 *   style                - Quote style for column name escaping
 *   use_dollar           - true for $N placeholders (PostgreSQL)
 *   err                  - Error output
 * Returns: DbInsertLists with allocated strings, or all NULL on error.
 *          Caller must call db_common_free_insert_lists() to cleanup. */
DbInsertLists db_common_build_insert_lists(const ColumnDef *cols,
                                           const DbValue *vals, size_t num_cols,
                                           DbQuoteStyle style, bool use_dollar,
                                           char **err);

/* Free resources allocated by db_common_build_insert_lists */
void db_common_free_insert_lists(DbInsertLists *lists);

/* Common implementation for freeing a string list (identical across drivers) */
void db_common_free_string_list(char **list, size_t count);

/* Build UPDATE SET clause: "col" = ? or "col" = $1
 * Returns newly allocated string, or NULL on error. */
char *db_common_build_update_set(const char *col, DbQuoteStyle style,
                                 bool use_dollar, size_t param_idx, char **err);

/* Build full UPDATE statement.
 * Parameters:
 *   escaped_table - Pre-escaped table name
 *   col           - Column name to update (will be escaped)
 *   pk_cols       - Primary key column names
 *   num_pk_cols   - Number of primary key columns
 *   style         - Quote style
 *   use_dollar    - Use $N placeholders (PostgreSQL) vs ? (SQLite/MySQL)
 *   err           - Error output
 * Returns: Newly allocated SQL string, or NULL on error.
 *          Parameter 1 is the new value, parameters 2..N+1 are PK values. */
char *db_common_build_update_sql(const char *escaped_table, const char *col,
                                 const char **pk_cols, size_t num_pk_cols,
                                 DbQuoteStyle style, bool use_dollar,
                                 char **err);

/* Build full DELETE statement.
 * Parameters:
 *   escaped_table - Pre-escaped table name
 *   pk_cols       - Primary key column names
 *   num_pk_cols   - Number of primary key columns
 *   style         - Quote style
 *   use_dollar    - Use $N placeholders (PostgreSQL) vs ? (SQLite/MySQL)
 *   err           - Error output
 * Returns: Newly allocated SQL string, or NULL on error. */
char *db_common_build_delete_sql(const char *escaped_table,
                                 const char **pk_cols, size_t num_pk_cols,
                                 DbQuoteStyle style, bool use_dollar,
                                 char **err);

/* Build full INSERT statement.
 * Parameters:
 *   escaped_table - Pre-escaped table name
 *   cols, vals    - Column definitions and values
 *   num_cols      - Number of columns
 *   style         - Quote style
 *   use_dollar    - Use $N placeholders (PostgreSQL) vs ? (SQLite/MySQL)
 *   lists_out     - Output: receives the insert lists for binding
 *   err           - Error output
 * Returns: Newly allocated SQL string, or NULL on error.
 *          On success, lists_out is populated and must be freed by caller. */
char *db_common_build_insert_sql(const char *escaped_table,
                                 const ColumnDef *cols, const DbValue *vals,
                                 size_t num_cols, DbQuoteStyle style,
                                 bool use_dollar, DbInsertLists *lists_out,
                                 char **err);

/* Free common DbConnection fields (but not driver_data).
 * Call this from driver disconnect functions after freeing driver_data.
 * Handles: connstr (secure), database, host, user, last_error, and conn itself.
 */
void db_common_free_connection(DbConnection *conn);

/* Parse integer from string with error handling.
 * Returns true on success, false if parsing fails.
 * On failure, value_out is unchanged. */
bool db_common_parse_int64(const char *str, int64_t *value_out);

/* Parse double from string with error handling.
 * Returns true on success, false if parsing fails.
 * On failure, value_out is unchanged. */
bool db_common_parse_double(const char *str, double *value_out);

/* Parse PostgreSQL array format: {elem1,elem2,...}
 * Returns array of strings (caller must free each element and array).
 * Sets count_out to number of elements.
 * Returns NULL on error or empty array. */
char **db_common_parse_pg_array(const char *array_str, size_t *count_out);

/* Free array returned by db_common_parse_pg_array */
void db_common_free_pg_array(char **array, size_t count);

#endif /* LACE_DB_COMMON_H */
