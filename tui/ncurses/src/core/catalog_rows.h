/* The rows of the sub-views every frontend lists (Structure, Indexes,
 * Foreign keys, Information, Definition), built from liblace's catalog
 * types into a column table the frontend only draws: an optional header
 * row, then rows of cells; a span row shows its first cell across the line
 * (a heading is a span row drawn as a title, never selected). Each row has a
 * tag saying what Enter on it means (CATALOG_TAG_*). No terminal or toolkit
 * calls: compiled into the ncurses, notcurses and GUI frontends. */
#ifndef LACE_CATALOG_ROWS_H
#define LACE_CATALOG_ROWS_H
#include "../../../liblace/include/lace.h" /* the GUI frontends import this header without -I liblace/include */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  size_t ncols, nrows, cap;
  char **head;      /* ncols or NULL */
  char ***cells;    /* nrows x ncols */
  bool *span, *heading;
  int *tag;
} CatalogRows;

/* Row tags */
enum {
  CATALOG_TAG_NONE = 0,
  CATALOG_TAG_ROWS = 1,        /* Information: the row count line (Enter counts) */
  CATALOG_TAG_FK = 100,        /* + i: foreign key i of the table */
  CATALOG_TAG_DEP = 1000,      /* + i: dependent i (Referenced by; Enter opens the table) */
  CATALOG_TAG_TRIGGER = 2000,  /* + i: object i of the connection's list (Enter opens it) */
  CATALOG_TAG_COLUMN = 3000,   /* + i: column i (Structure) */
  CATALOG_TAG_INDEX = 4000     /* + i: index or constraint i of the schema (Indexes) */
};

void catalog_rows_init(CatalogRows *t, size_t ncols, const char *const *head);
void catalog_rows_add(CatalogRows *t, int tag, ...); /* ncols strings (NULL: empty), copied */
void catalog_rows_span(CatalogRows *t, int tag, const char *text);
void catalog_rows_heading(CatalogRows *t, const char *title);
void catalog_rows_blank(CatalogRows *t);
/* A row of cells drawn as a header (bold), never selected */
void catalog_rows_head_row(CatalogRows *t, const char *const *cells);
void catalog_rows_free(CatalogRows *t);
bool catalog_rows_selectable(const CatalogRows *t, size_t row);

/* ---- builders; each frees t first ---- */
/* The columns: #, Name, Type, Null, Key, Default, Extra, and Collation /
 * Comment when a column has one */
void catalog_rows_structure(CatalogRows *t, const LaceSchema *sc);
/* Indexes and constraints: Name, Kind, Columns, Condition, Method, Constraint
 * (the last three only when any entry has them); false: none */
bool catalog_rows_indexes(CatalogRows *t, const LaceSchema *sc);
/* The foreign keys, then "Referenced by" (deps; deps_error / not loaded say so) */
void catalog_rows_fks(CatalogRows *t, const LaceSchema *sc, const char *table, const LaceDependent *deps,
                      size_t num_deps, bool deps_loaded, const char *deps_error);
/* The definition's lines (wrapped at width columns, at a space when there is
 * one past the first third), then the columns under a "Columns" heading when
 * cols is given */
void catalog_rows_definition(CatalogRows *t, const char *sql, int width, const LaceSchema *cols);
/* Information: what the catalog knows. exact: the row count when known (-1
 * else); counting: a count is running; info / autoinc may be NULL (loading
 * or error: info_error says); the triggers on the table from objects */
void catalog_rows_info(CatalogRows *t, LaceObjectKind kind, const char *name, const char *object_table,
                       const char *object_detail, const LaceTableInfo *info, const LaceAutoInc *autoinc,
                       const char *info_error, int64_t exact, bool counting, const LaceObject *objects,
                       size_t num_objects);

/* ---- helpers ---- */
char *catalog_join(char *const *names, size_t n); /* "a, b" (allocated) */
char *catalog_commas(int64_t n);                   /* "1,234" */
char *catalog_bytes(int64_t n);                    /* "1.2 MB (1,234,567 bytes)"; "unknown" below 0 */
/* The column is the first column of one of the schema's foreign keys */
bool catalog_fk_first_column(const LaceSchema *sc, const char *name);
/* The name of an object kind: "Table", "View", "Routine", "Trigger" */
const char *catalog_object_word(LaceObjectKind kind);
#endif
