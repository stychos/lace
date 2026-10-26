/*
 * Lace
 * The structure editors and table actions of a table tab (ddl_run.c,
 * structure_edit.c, constraint_edit.c, table_actions.c): every change is a
 * statement liblace built (lace_ddl_*), shown in a confirmation under a
 * plain-words summary, run with history, then the object's tabs reload.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_DDL_EDIT_H
#define LACE_DDL_EDIT_H

#include "core/catalog_rows.h"
#include "tui_internal.h"

typedef enum {
  DDL_AFTER_SCHEMA,       /* the object's tabs reload (schema, rows, count) */
  DDL_AFTER_TABLES,       /* the same, and the sidebar's lists */
  DDL_AFTER_RENAME,       /* arg: the new name; the object's tabs follow, the sidebar reloads */
  DDL_AFTER_DROP,         /* the object's tabs close, the sidebar reloads */
  DDL_AFTER_CREATE_TABLE  /* arg: the table; the sidebar reloads and the table opens on Structure */
} DdlAfter;

/* Confirm (the summary, the statement, a transaction note), run the
 * statement on the current tab's connection behind the processing dialog,
 * record it in history and apply the after-effects to every tab of the
 * current tab's object on that connection. sql and arg are adopted. True
 * when it ran. */
bool tui_ddl_run(TuiState *state, const char *title, const char *summary, char *sql, bool destructive,
                 DdlAfter after, char *arg);
/* A refusal before any confirmation: the reason in the status line; error is freed */
void tui_ddl_refused(TuiState *state, char *error);
/* "orders (12,400 rows)" from what the tab knows; "orders" else */
char *tui_ddl_rows_text(const Tab *tab);
/* The names a picker lists ("types", "collations" with a charset or NULL,
 * "engines"), read now; freed with lace_tables_free. NULL with *n 0 on failure. */
char **tui_ddl_names(TuiState *state, const char *what, const char *arg, size_t *n);
/* A query tab holding text (NULL: empty); the table list reloads once a
 * statement ran in it */
bool tui_ddl_open_query(TuiState *state, const char *text);
/* query_execute ran a statement on the current tab: the hook for the above */
void tui_ddl_query_ran(TuiState *state);
/* A yes/no dialog whose message may hold several lines (the statement) */
bool tui_ddl_confirm(TuiState *state, const char *title, const char *message, const char *yes_label,
                     bool destructive);
LaceDriver tui_tab_driver(TuiState *state);

/* The STRUCTURE-category keys in a sub-view list, on the row with that tag
 * (CATALOG_TAG_NONE: none); false: not taken */
bool tui_structure_action(TuiState *state, Tab *tab, HotkeyAction a, int tag);
/* Enter on a row of the structure, indexes, foreign keys or information
 * list: the editor, rename, or an Information action; false: not one of
 * ours (the trigger and dependent rows stay with subviews.c) */
bool tui_structure_activate(TuiState *state, Tab *tab, int tag);
/* The per-list parts (constraint_edit.c, table_actions.c) */
bool tui_index_action(TuiState *state, Tab *tab, HotkeyAction a, int tag);
bool tui_fk_action(TuiState *state, Tab *tab, HotkeyAction a, int tag);
bool tui_info_activate(TuiState *state, Tab *tab, int tag);
/* Information's "Actions" rows, appended after the facts */
void tui_info_action_rows(TuiState *state, Tab *tab, CatalogRows *t);
/* The table's actions pick list (table_menu, m) */
void tui_table_menu(TuiState *state);

/* Tags of the Information actions (above every shared range) */
enum {
  SV_TAG_AUTOINC = 90001, SV_TAG_COMMENT, SV_TAG_ENGINE, SV_TAG_CHARSET, SV_TAG_ANALYZE, SV_TAG_VACUUM,
  SV_TAG_OPTIMIZE, SV_TAG_EDIT_DEFINITION
};

#define SQLITE_CANNOT_ALTER "SQLite cannot alter this; create a new table and copy the rows"

/* Helpers shared by the editors */
const LaceColumn *tui_schema_column(const Tab *tab, const char *name);
/* The table's column names (the schema's, not copied) into an array the caller frees */
size_t tui_schema_column_names(const Tab *tab, const char ***out);
static inline bool tui_driver_mysql(LaceDriver d) { return d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB; }

#endif /* LACE_DDL_EDIT_H */
