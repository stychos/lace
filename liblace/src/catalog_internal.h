/*
 * Lace
 * Internal: catalog reads shared by client.c and catalog.c, and the SQLite
 * CREATE-text parsers (what the pragmas do not expose: collations, check
 * constraints, AUTOINCREMENT, partial-index predicates, index expressions)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CATALOG_INTERNAL_H
#define LACE_CATALOG_INTERNAL_H

#include "lace.h"
#include <stdbool.h>
#include <stddef.h>

/* ---- SQLite CREATE text (catalog_sqlite.c) --------------------------------
 * All take the statement as sqlite_master.sql holds it. Identifiers in the
 * results are unquoted. Quotes, brackets, backticks, comments and nested
 * parentheses are honoured; anything that does not parse gives nothing. */

/* The COLLATE name of a column in a CREATE TABLE, or NULL (allocated) */
char *lace_sqlite_column_collation(const char *create_sql, const char *column);
/* The CHECK constraints of a CREATE TABLE: parallel arrays of names (NULL
 * for an unnamed or column-level check) and conditions (the text inside the
 * parentheses); returns the count. Free each string and both arrays. */
size_t lace_sqlite_check_constraints(const char *create_sql, char ***names, char ***conds);
/* The word AUTOINCREMENT appears outside quotes */
bool lace_sqlite_has_autoincrement(const char *create_sql);
/* A CREATE INDEX's partial predicate (the text after its WHERE), or NULL */
char *lace_sqlite_index_where(const char *index_sql);
/* A CREATE INDEX's column list, one entry per item as written ("name",
 * "name COLLATE NOCASE DESC", "lower(email)"); returns the count */
size_t lace_sqlite_index_columns(const char *index_sql, char ***cols);

/* ---- Shared with client.c (catalog.c) ------------------------------------- */

/* Split a newline-joined list (as string_agg / GROUP_CONCAT / group_concat
 * build them) into allocated strings; NULL or "" gives 0 */
size_t lace_catalog_split_lines(const char *text, char ***out);

/* Indexes, constraints and foreign keys of a table into sch (allocated
 * arrays); the SQLite extras (collations, checks) too. Reads that an older
 * server cannot answer leave their part empty; returns LACE_OK unless a
 * read that every server has failed. */
int lace_catalog_fill_schema(lace_client_t *client, int conn_id, LaceDriver driver,
                             const char *table, LaceSchema *sch);

#endif
