/* The one run path of every structure change and table action: the
 * statement liblace built (lace_ddl_*) is shown in a confirmation under a
 * plain-words summary, run as a recorded user statement on the view's
 * connection, and on success the views of the object (in every tab holding
 * the connection, in every workspace) reload their schema, drop their undo
 * stacks and what the sub-views loaded, and the sidebar reloads after a
 * rename, create or drop. A refusal from liblace never reaches a
 * confirmation; a refusal from the server is shown in full. */
#ifndef NC_APP_DDL_H
#define NC_APP_DDL_H
#include "app.h"
#include "model/view.h"

typedef enum {
  DDL_AFTER_SCHEMA,       /* the object's views reload (schema, page 0, count) */
  DDL_AFTER_TABLES,       /* the same, and the sidebar's lists */
  DDL_AFTER_RENAME,       /* arg: the new name; the object's views follow, the sidebar reloads */
  DDL_AFTER_DROP,         /* the object's views close, the sidebar reloads */
  DDL_AFTER_CREATE_TABLE, /* arg: the table; the sidebar reloads and the table opens on Structure */
  DDL_AFTER_NONE
} DdlAfter;

/* Confirm, then run. title: the confirmation's ("Drop column"); summary: the
 * plain words; sql: adopted; arg: adopted (DDL_AFTER_RENAME/CREATE_TABLE);
 * form_uid: a form to close on success, or to show the server's refusal in
 * (0: none). The view names the object the change is about. */
void app_ddl_confirm(NcApp *app, View *v, const char *title, const char *summary, char *sql, bool destructive,
                     DdlAfter after, char *arg, uint64_t form_uid);
/* A refusal before any confirmation: the reason in the status line and in
 * the form (when given); error is freed */
void app_ddl_refused(NcApp *app, uint64_t form_uid, char *error);
/* "orders (12,400 rows)" from what the view's cache knows; "orders" else */
char *app_ddl_rows_text(const View *v);
/* The names a picker lists, fetched on the view's connection into a form's
 * field once they land (the form may be gone by then) */
void app_ddl_fetch_names(NcApp *app, View *v, uint64_t form_uid, int field_id, const char *what, const char *arg);
/* Open a query view on the view's connection holding text (NULL: empty),
 * which reloads the sidebar once a statement ran */
View *app_ddl_open_query(NcApp *app, View *v, const char *text);
#endif
