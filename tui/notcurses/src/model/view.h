/* The view model: one table view or query view with its filters, sorts,
 * schema, cursor, scroll, selection, column widths, page cache and query
 * text state. Backend-free: SQL strings come from liblace's builders. */
#ifndef NC_VIEW_H
#define NC_VIEW_H

#include "core/undo_stack.h"
#include "config/config.h"
#include "core/constants.h"
#include "model/page_cache.h"
#include "sql_builder.h"
#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { VIEW_TABLE, VIEW_QUERY } ViewKind;
/* The sub-views of a VIEW_TABLE (ui/subviews.c): a table has all five; a
 * database view Data, Definition (the Structure slot) and Information; a
 * routine or trigger Definition and Information */
typedef enum { SV_DATA, SV_STRUCTURE, SV_INDEXES, SV_FOREIGN_KEYS, SV_INFORMATION, SV_COUNT } SubView;
/* The filter panel's fields, left to right */
enum { FP_COLUMN, FP_OP, FP_VALUE, FP_VALUE2 };

typedef struct View {
  ViewKind kind;
  uint64_t uid;
  char *table; /* VIEW_TABLE: table reference (a view's, a routine's or a trigger's name) */
  LaceObjectKind object; /* VIEW_TABLE: what table names (a table, a database view, a routine, a trigger) */
  char *object_table;    /* a trigger's table; NULL otherwise */
  char *object_detail;   /* a routine's "function"/"procedure", a materialized view's "materialized" */
  SubView subview;       /* VIEW_TABLE: the current sub-view (saved in the session) */
  size_t sv_cursor[SV_COUNT], sv_scroll[SV_COUNT]; /* the lists' positions (runtime only) */
  /* What the sub-views besides Data show, loaded when first drawn (ui/sv_*.c):
   * the table's information and counter, the tables referencing it, the
   * definition of a view, routine or trigger; a refresh frees them */
  LaceTableInfo *info;
  LaceAutoInc *autoinc;
  bool info_loading;
  char *info_error;
  int64_t info_count;      /* the exact row count asked for from Information, -1 unknown */
  bool info_count_pending;
  LaceDependent *deps;
  size_t num_deps;
  bool deps_loaded, deps_loading;
  char *deps_error;
  bool referenced_by_pending; /* referenced_by waits for the dependents, then opens its list */
  int pending_action;         /* a table action waiting for the dependents (app_table_menu.c), 0 none */
  bool reload_tables_on_run;  /* VIEW_QUERY opened from "New view/function": the sidebar reloads
                               * once a statement ran (runtime) */
  char *definition;
  bool definition_loading;
  char *definition_error;
  LaceFilter *filters;
  size_t num_filters;
  char *search;
  LaceSortSpec *sorts;
  size_t num_sorts;
  LaceSchema *schema; /* VIEW_TABLE; VIEW_QUERY: the source table's (editable results) */
  char *schema_error;
  char *source_table; /* VIEW_QUERY: lace_sql_simple_select_table() result or NULL */
  PageCache *cache;
  size_t cursor_row, cursor_col, scroll_row, scroll_col;
  size_t *selected; /* sorted row positions */
  size_t num_selected;
  int *widths; /* per column, display columns */
  size_t num_widths;
  bool *user_width; /* per column: set by view_set_width(), view_measure() leaves it alone */
  bool filters_visible;
  bool preview; /* VIEW_TABLE: shown in the preview tab, which the next single click on a table
                 * replaces; pinned (cleared) by a double click, Enter, or a sort, filter or edit (runtime only) */
  /* The docked filter panel (ui/filter_panel.c), runtime only */
  struct {
    bool focused;
    size_t row;     /* the cursor row among the filters */
    int field;      /* FP_COLUMN, FP_OP, FP_VALUE, FP_VALUE2 */
    size_t scroll;
    bool editing;   /* the value field is being typed into */
    char *text;     /* the edit's text while editing */
    size_t cursor;
  } fp;
  bool loading;
  /* Inline cell edit (ui/grid_edit.c): on the cursor cell */
  bool editing;
  char *edit_text;     /* NULL = the NULL value (Ctrl+N) */
  size_t edit_cursor;
  size_t edit_row, edit_col; /* the cell the edit started on */
  uint64_t edit_gen;         /* page_cache_generation() then */
  char *error; /* last page/count error, shown once */
  /* VIEW_QUERY */
  char *text;
  size_t text_cursor, text_sel_anchor /* SIZE_MAX none */, text_scroll_line, text_scroll_col, text_want_col;
  char *base_sql; /* the last statement run and how */
  bool paged, wrap;
  LaceResult *once; /* an unpaged result or an exec count */
  int64_t affected;
  bool ran;
  int editor_rows; /* split: editor height, 0 = default third */
  bool split_drag; /* the divider is being dragged with the mouse (runtime only) */
  bool focus_results;
  bool cancel_run;         /* Esc was pressed during the run: a script stops at the next step */
  bool running;            /* a statement or script is in flight (app_query.c) */
  bool committing;         /* a script's COMMIT is in flight: Esc must not cancel it */
  bool first_page_pending; /* page 0 of a paged statement was asked for: its failure runs the statement as written */
  uint64_t run_seq;        /* bumped for every run: late schema/exec answers of an older run are dropped */
  /* Loading state, kept by app_table.c */
  void *fetch_ctx;          /* the page cache's fetch data (freed with the view) */
  bool fail_set;            /* the last page that failed, not asked for again until the view moves */
  size_t fail_page, fail_cursor, fail_scroll;
  uint64_t fail_gen;
  bool count_pending;       /* a count job is in flight, asked under count_gen */
  uint64_t count_gen;
  bool jump_pending;        /* go to jump_row once the total is known (SIZE_MAX: last row) */
  size_t jump_row;
  /* The sorts, filters and position before the last change, to go back to
   * when its reload is cancelled */
  bool has_checkpoint;
  LaceSortSpec *cp_sorts;
  size_t cp_num_sorts;
  LaceFilter *cp_filters;
  size_t cp_num_filters;
  size_t cp_cursor_row, cp_cursor_col, cp_scroll_row, cp_scroll_col;
  UndoStack undo; /* the writes that succeeded, newest last (core/undo_stack.h, undo.c); runtime only */
  /* private: column names the widths were measured for */
  char **col_names;
  size_t num_col_names;
} View;

View *view_new_table(const char *table);
/* A view of a database view, routine or trigger (as lace_list_objects names
 * it); a routine or trigger starts on Definition, the rest on Data */
View *view_new_object(LaceObjectKind kind, const char *name, const char *table, const char *detail);
/* Whether the object has that sub-view */
bool view_has_subview(const View *v, SubView sv);
/* Drop what the sub-views loaded (info, dependents, definition): a refresh */
void view_drop_catalog(View *v);
/* The session's names: "data", "structure", ...; "definition" for an object
 * whose Structure slot is its definition */
const char *view_subview_key(const View *v, SubView sv);
bool view_subview_from_key(const char *key, SubView *out);
const char *view_object_key(LaceObjectKind kind); /* "table", "view", "routine", "trigger" */
bool view_object_from_key(const char *key, LaceObjectKind *out);
View *view_new_query(const char *text);
void view_free(View *v);
void view_set_schema(View *v, LaceSchema *schema); /* adopts */
/* True once after the cache's column set changed; resets widths, clamps the
 * cursor column and clears the selection */
bool view_columns_changed(View *v);
/* Grow-only widths from loaded rows and headers; columns whose width was set
 * with view_set_width() are left alone until the column set changes */
void view_measure(View *v, size_t from_row, size_t to_row);
void view_set_width(View *v, size_t col, int width); /* clamped to MIN_COL_WIDTH..200 */
void view_set_cursor(View *v, size_t row, size_t col); /* clamps to extent and columns */
void view_move(View *v, long drow, long dcol);
void view_scroll_into_view(View *v, int rows_visible, int cols_avail, int gutter);
size_t view_first_visible_col(const View *v);
size_t view_last_visible_col(const View *v, int cols_avail, int gutter);
void view_toggle_select(View *v, size_t row);
void view_clear_selection(View *v);
bool view_is_selected(const View *v, size_t row);
void view_cycle_sort(View *v, const char *column); /* asc -> desc -> off; others kept, newest first */
void view_set_filter(View *v, const char *column, LaceFilterOp op, const char *value,
                     const char *value2); /* replaces the one for that column, or adds */
void view_remove_filter(View *v, size_t index);
/* A blank "column = ''" row for the panel; returns its index */
size_t view_add_filter(View *v, const char *column);
/* Rewrite one filter in place (column NULL: a raw condition in value) */
void view_filter_set(View *v, size_t index, const char *column, LaceFilterOp op, const char *value,
                     const char *value2);
/* A filter that reaches the WHERE: a raw condition with text, an operator
 * without a value, or one whose value(s) are filled in */
bool view_filter_active(const LaceFilter *f);
void view_prune_filters(View *v); /* drop the inactive ones (the panel's blank rows) */
void view_add_raw_filter(View *v, const char *condition); /* a raw SQL condition, no column */
/* Remember sorts, filters and position, unless a checkpoint is held already: it
 * describes the state the stale pages came from and lives until a page of the
 * new state lands or the change is reverted. True when this call took it. */
bool view_checkpoint(View *v);
/* The last failed page request still blocks asking again (same generation,
 * cursor and scroll) */
bool view_fail_blocks(const View *v);
void view_checkpoint_drop(View *v); /* the change was accepted */
bool view_revert(View *v);          /* back to the checkpoint; false when none */
void view_clear_filters(View *v);
void view_clear_sorts(View *v);
char *view_where(const View *v, LaceDriver d);    /* NULL = none */
char *view_order_by(const View *v, LaceDriver d); /* sorts + every pk column; NULL = none */
char *view_page_sql(const View *v, LaceDriver d, size_t page);
char *view_count_sql(const View *v, LaceDriver d);
const char *view_title(const View *v); /* table name or first line of the SQL, "New query" */
bool view_row_numbers(const View *v, const Config *config);
/* The column is the first column of one of the schema's foreign keys (the
 * grid's header marks it, follow_fk follows it) */
bool view_fk_first_column(const View *v, const char *name);

#endif
