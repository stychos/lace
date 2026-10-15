/*
 * liblace - Lace Client Library
 * SQL synthesis and statement scanning for frontends
 *
 * Everything a frontend needs to turn filters, sorts and editor text into
 * SQL, and to read SQL text to decide how to run it: value display text and
 * the cell-edit rules, the lexical shape of a statement (what can be paged,
 * and how), statement boundaries in editor text, filters to a WHERE body,
 * sorts to an ORDER BY body, one page or a count of a table, and the table
 * a plain single-table SELECT reads (so its rows can be edited by key).
 *
 * Identifiers are quoted only with lace_quote_identifier()/lace_quote_table(),
 * string literals only with lace_sql_string_literal(), so every clause built
 * here is as safe as the rest of liblace's SQL. Shared by the GTK frontend
 * (and any other native one); the TUI still has its own older copies.
 * Tested by tests/test_liblace.c.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_SQL_BUILDER_H
#define LIBLACE_SQL_BUILDER_H

#include "lace_export.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

LACE_BEGIN_DECLS

/* ==========================================================================
 * Value display and editing
 * ========================================================================== */

/* One line of text for a cell: "NULL" for null, exact numbers, booleans as
 * true/false, text with line breaks shown as "⏎", a BLOB as "0x<hex>… (N
 * bytes)", a value too large to load as its label. At most max_chars
 * characters (0: no limit), with "…" when cut. Allocated. */
char *lace_value_display_text(const LaceValue *v, size_t max_chars);

/* True if values of this type are drawn right-aligned (numbers) */
bool lace_type_is_numeric(LaceValueType t);

/* The value an edit writes, from the cell's original value and the edited
 * text (NULL text: the explicit NULL action). Unchanged text, or a NULL cell
 * left empty, sets *unchanged and writes nothing; empty text is '' (never
 * NULL); the text converts like the value the cell held (hex for a BLOB,
 * exact digits for a DECIMAL), else like the column's type. False with *err
 * (allocated) for text that cannot be such a value. */
bool lace_edit_value(const LaceValue *orig, LaceValueType col_type,
                     const char *text, LaceValue *out, bool *unchanged,
                     char **err);

/* ==========================================================================
 * Statement shape
 * ========================================================================== */

/* Lexical shape of one statement (strings, comments and $tag$ bodies
 * skipped): its first keyword, its verb (the first top-level SELECT/INSERT/
 * UPDATE/DELETE/MERGE/REPLACE/VALUES/TABLE/UPSERT, after any WITH list),
 * whether it has a top-level INTO or LIMIT/OFFSET/FETCH, whether anything in
 * it writes (lace_sql_read_only), and where it really ends (before trailing
 * ';', whitespace or comments). */
typedef struct {
  char first_keyword[16];
  char verb[8];
  bool top_level_into;
  bool top_level_limit;
  bool nested_write;
  size_t end;
} LaceSqlShape;

void lace_sql_shape(const char *sql, LaceDriver driver, LaceSqlShape *out);

/* True for a statement that only reads and can take LIMIT/OFFSET */
bool lace_sql_shape_pageable(const LaceSqlShape *s);

/* True if a page must wrap the statement in SELECT * FROM (...) instead of
 * appending LIMIT: it has its own top-level LIMIT/OFFSET/FETCH, or it is a
 * VALUES list */
bool lace_sql_shape_needs_wrap(const LaceSqlShape *s);

/* base: the statement cut at shape.end. The newline keeps a trailing
 * "-- comment" (if base still holds one) from swallowing the clause.
 * Allocated. */
char *lace_sql_page_of_statement(const char *base, bool wrap, size_t limit,
                                 size_t offset);
char *lace_sql_count_of_statement(const char *base);

/* ==========================================================================
 * Statement boundaries in editor text
 * ========================================================================== */

/* Byte range [*start, *end) of the statement at cursor, its ';' excluded,
 * surrounding whitespace trimmed. In blank space after a ';' it is the
 * previous statement. False if there is no statement text there. */
bool lace_sql_statement_at(const char *text, size_t cursor, LaceDriver driver,
                           size_t *start, size_t *end);

/* Iterate statements: the first statement starting at or after `from`,
 * trimmed and without its ';'. A fragment holding only whitespace and
 * comments is skipped. Returns false when none is left; *next is where to
 * continue. */
bool lace_sql_next_statement(const char *text, size_t from, LaceDriver driver,
                             size_t *start, size_t *end, size_t *next);

/* ==========================================================================
 * Filters and sorts
 * ========================================================================== */

typedef enum {
  LACE_FILTER_EQ,
  LACE_FILTER_NE,
  LACE_FILTER_GT,
  LACE_FILTER_GE,
  LACE_FILTER_LT,
  LACE_FILTER_LE,
  LACE_FILTER_IN,
  LACE_FILTER_CONTAINS,
  LACE_FILTER_STARTS,
  LACE_FILTER_ENDS,
  LACE_FILTER_REGEX,
  LACE_FILTER_BETWEEN,
  LACE_FILTER_EMPTY,
  LACE_FILTER_NOT_EMPTY,
  LACE_FILTER_NULL,
  LACE_FILTER_NOT_NULL,
  LACE_FILTER_RAW,
  LACE_FILTER_OP_COUNT
} LaceFilterOp;

/* A column filter; column NULL (or op LACE_FILTER_RAW) is a raw SQL
 * condition held in value */
typedef struct {
  char *column;
  LaceFilterOp op;
  char *value;
  char *value2; /* BETWEEN upper bound */
} LaceFilter;

const char *lace_filter_op_label(LaceFilterOp op); /* "=", "contains", "is NULL" */
const char *lace_filter_op_key(LaceFilterOp op);   /* "eq", "contains" (stable, for session files) */
LaceFilterOp lace_filter_op_from_key(const char *key);
bool lace_filter_op_needs_value(LaceFilterOp op);
bool lace_filter_op_two_values(LaceFilterOp op);

/* Free the filter's strings and zero it */
void lace_filter_clear(LaceFilter *f);

/* A sort column by name (LaceSort indexes a schema instead) */
typedef struct {
  char *column;
  bool desc;
} LaceSortSpec;

/* WHERE body (no keyword) for the filters, AND-ed, plus a text search over
 * every text column of the schema (case-insensitive contains) when search is
 * non-empty. Filters whose column is not in the schema, or that need a
 * value and have none, are skipped. NULL when nothing applies. Allocated. */
char *lace_filter_build_where(const LaceFilter *filters, size_t num_filters,
                              const char *search, const LaceSchema *schema,
                              LaceDriver driver);

/* ORDER BY body (no keyword); NULL when there is no sort. Allocated. */
char *lace_sort_build_order_by(const LaceSortSpec *sorts, size_t num_sorts,
                               LaceDriver driver);

/* One page of a table: SELECT * FROM t [WHERE w] [ORDER BY o] LIMIT n
 * OFFSET m. where/order_by may be NULL. table is a table reference. */
char *lace_sql_table_page(LaceDriver driver, const char *table,
                          const char *where, const char *order_by,
                          size_t limit, size_t offset);

/* SELECT COUNT(*) FROM t [WHERE w] */
char *lace_sql_table_count(LaceDriver driver, const char *table,
                           const char *where);

/* ==========================================================================
 * Editable query results
 * ========================================================================== */

/* The table a plain single-table SELECT reads (as a table reference), or
 * NULL when its rows cannot be addressed by primary key: JOINs, set
 * operations, subqueries, GROUP BY, DISTINCT, aggregates, multi-table FROM,
 * and names qualified with another database. Allocated. */
char *lace_sql_simple_select_table(const char *sql, LaceDriver driver);

LACE_END_DECLS

#endif /* LIBLACE_SQL_BUILDER_H */
