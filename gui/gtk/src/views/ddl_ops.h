/*
 * Lace GTK frontend
 * The one run path of every structure change and table action: the
 * statement liblace built (lace_ddl_*) is shown in an alert dialog under a
 * plain-words summary, run as a recorded user statement on the view's
 * connection, and on success every table view of the object on that
 * connection, in every window, reloads (schema, pages, count, what the
 * sub-views loaded); the sidebar reloads after a rename, create or drop, a
 * rename renames the tabs, a drop closes them, a new table opens on
 * Structure. A liblace refusal never reaches a confirmation; a server
 * refusal is shown in full (a toast, and in the editor when one is open).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_DDL_OPS_H
#define LACE_GTK_DDL_OPS_H

#include "table_view.h"
#include <adwaita.h>

G_BEGIN_DECLS

typedef enum {
  LACE_DDL_AFTER_SCHEMA,       /* the object's views reload (schema, page 0, count) */
  LACE_DDL_AFTER_TABLES,       /* the same, and the sidebar's lists */
  LACE_DDL_AFTER_RENAME,       /* arg: the new name; the object's views follow, the sidebar reloads */
  LACE_DDL_AFTER_DROP,         /* the object's views close, the sidebar reloads */
  LACE_DDL_AFTER_CREATE_TABLE, /* arg: the table; the sidebar reloads and the table opens on Structure */
} LaceDdlAfter;

typedef struct {
  const char *title;   /* the confirmation's ("Drop column") */
  const char *summary; /* plain words */
  char *sql;           /* adopted (allocated by liblace) */
  gboolean destructive;
  LaceDdlAfter after;
  char *arg;              /* adopted; the new name (RENAME, CREATE_TABLE) */
  AdwDialog *editor;      /* closed on success, kept on a refusal; may be NULL */
  GtkWidget *editor_banner; /* the editor's banner for a refusal; may be NULL */
} LaceDdlChange;

/* Confirm, then run; v names the object the change is about and the widget
 * the dialogs open over */
void lace_ddl_confirm(LaceTableView *v, const LaceDdlChange *change);
/* A refusal before any confirmation: a toast over parent and, when banner
 * is given, the reason in it; error (allocated) is freed */
void lace_ddl_refused(GtkWidget *parent, GtkWidget *banner, char *error);
/* The names a picker lists, fetched on the connection into the string list
 * (spliced in) once they land; owner's death drops them */
void lace_ddl_fetch_names(LaceConnection *conn, GObject *owner, GtkStringList *into, const char *what,
                          const char *arg);
/* A query view holding text on the view's connection page; it reloads the
 * sidebar once a statement ran */
void lace_ddl_open_query(LaceTableView *v, const char *text);
/* Every table view of the object on the connection, in every window, takes
 * the change (origin: the page a new table opens in; may be NULL) */
void lace_ddl_apply_after(LaceConnection *conn, LaceObjectKind kind, const char *object, LaceDdlAfter after,
                          const char *arg, GtkWidget *origin);

G_END_DECLS

#endif /* LACE_GTK_DDL_OPS_H */
