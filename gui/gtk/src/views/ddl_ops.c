/*
 * Lace GTK frontend
 * The run path of structure changes and table actions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ddl_ops.h"
#include "app.h"
#include "connection_page.h"
#include "query_view.h"
#include "util.h"
#include "window.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  LaceConnection *conn;
  LaceObjectKind kind;
  char *object;
  char *title, *sql, *arg;
  LaceDdlAfter after;
  GWeakRef editor, editor_banner, origin;
} DdlReq;

static void req_free(gpointer p) {
  DdlReq *r = p;
  g_clear_object(&r->conn);
  g_free(r->object);
  g_free(r->title);
  free(r->sql);
  g_free(r->arg);
  g_weak_ref_clear(&r->editor);
  g_weak_ref_clear(&r->editor_banner);
  g_weak_ref_clear(&r->origin);
  g_free(r);
}

static void banner_say(GtkWidget *banner, const char *msg) {
  if (!banner)
    return;
  adw_banner_set_title(ADW_BANNER(banner), msg ? msg : "");
  adw_banner_set_revealed(ADW_BANNER(banner), msg && *msg);
}

void lace_ddl_refused(GtkWidget *parent, GtkWidget *banner, char *error) {
  const char *why = error && *error ? error : "Not possible here";
  if (parent)
    lace_toast(parent, "%s", why);
  banner_say(banner, why);
  free(error);
}

/* ==========================================================================
 * After a successful change
 * ========================================================================== */

static gboolean same_object(LaceTableView *v, LaceObjectKind kind, const char *object) {
  LaceObjectKind k = lace_table_view_get_object_kind(v);
  gboolean ta = k == LACE_OBJ_TABLE || k == LACE_OBJ_VIEW, tb = kind == LACE_OBJ_TABLE || kind == LACE_OBJ_VIEW;
  if (ta != tb || (!ta && k != kind))
    return FALSE;
  return g_strcmp0(lace_table_view_get_table(v), object) == 0;
}

void lace_ddl_apply_after(LaceConnection *conn, LaceObjectKind kind, const char *object, LaceDdlAfter after,
                          const char *arg, GtkWidget *origin) {
  gboolean sidebar = after != LACE_DDL_AFTER_SCHEMA;
  GList *wins = lace_app_get_lace_windows(lace_app_get_default());
  for (GList *l = wins; l; l = l->next) {
    AdwTabView *wtv = lace_window_get_tab_view(LACE_WINDOW(l->data));
    int np = adw_tab_view_get_n_pages(wtv);
    for (int i = 0; i < np; i++) {
      GtkWidget *child = adw_tab_page_get_child(adw_tab_view_get_nth_page(wtv, i));
      if (!LACE_IS_CONNECTION_PAGE(child) || lace_connection_page_get_connection(LACE_CONNECTION_PAGE(child)) != conn)
        continue;
      LaceConnectionPage *cp = LACE_CONNECTION_PAGE(child);
      AdwTabView *tv = lace_connection_page_get_tab_view(cp);
      GPtrArray *views = g_ptr_array_new();
      for (int k = 0; k < adw_tab_view_get_n_pages(tv); k++) {
        GtkWidget *w = lace_connection_page_view_of(adw_tab_view_get_nth_page(tv, k));
        if (LACE_IS_TABLE_VIEW(w) && same_object(LACE_TABLE_VIEW(w), kind, object))
          g_ptr_array_add(views, w);
      }
      for (guint k = 0; k < views->len; k++) {
        GtkWidget *w = g_ptr_array_index(views, k);
        if (after == LACE_DDL_AFTER_DROP)
          lace_connection_page_close_view(cp, w);
        else
          lace_table_view_after_ddl(LACE_TABLE_VIEW(w), after == LACE_DDL_AFTER_RENAME ? arg : NULL);
      }
      g_ptr_array_free(views, TRUE);
    }
  }
  g_list_free(wins);
  if (sidebar && !lace_connection_is_closed(conn))
    lace_connection_refresh_tables(conn);
  if (after == LACE_DDL_AFTER_CREATE_TABLE && arg && origin) {
    GtkWidget *page = gtk_widget_get_ancestor(origin, LACE_TYPE_CONNECTION_PAGE);
    if (page) {
      LaceTableView *nv = lace_connection_page_open_table(LACE_CONNECTION_PAGE(page), arg);
      if (nv)
        lace_table_view_set_subview(nv, LACE_SUBVIEW_STRUCTURE);
    }
  }
}

static void ddl_done(LaceJob *job, gpointer data) {
  DdlReq *r = data;
  GtkWidget *origin = g_weak_ref_get(&r->origin);
  GtkWidget *editor = g_weak_ref_get(&r->editor);
  GtkWidget *banner = g_weak_ref_get(&r->editor_banner);
  if (job->error) {
    const char *why = job->cancelled ? "Cancelled" : lace_job_error(job);
    if (origin)
      lace_toast(origin, "%s: %s", r->title, why);
    banner_say(banner, why);
    if (editor)
      gtk_widget_set_sensitive(editor, TRUE);
  } else {
    if (origin)
      lace_toast(origin, "%s: done", r->title);
    if (editor)
      adw_dialog_close(ADW_DIALOG(editor));
    lace_ddl_apply_after(r->conn, r->kind, r->object, r->after, r->arg, origin);
  }
  g_clear_object(&origin);
  g_clear_object(&editor);
  g_clear_object(&banner);
}

static void run(DdlReq *r) {
  GtkWidget *editor = g_weak_ref_get(&r->editor);
  if (editor) {
    gtk_widget_set_sensitive(editor, FALSE); /* until the answer */
    g_object_unref(editor);
  }
  LaceJob *job = lace_job_exec(r->sql);
  job->user_statement = TRUE; /* the lock banner */
  lace_connection_submit_full(r->conn, job, NULL, ddl_done, r, req_free);
}

static void confirmed(gboolean yes, gpointer data) {
  DdlReq *r = data;
  if (!yes) {
    req_free(r);
    return;
  }
  if (lace_connection_is_closed(r->conn)) {
    GtkWidget *origin = g_weak_ref_get(&r->origin);
    if (origin)
      lace_toast(origin, "Not connected");
    g_clear_object(&origin);
    req_free(r);
    return;
  }
  run(r);
}

void lace_ddl_confirm(LaceTableView *v, const LaceDdlChange *c) {
  LaceConnection *conn = lace_table_view_get_connection(v);
  if (!c->sql || lace_connection_is_closed(conn)) {
    lace_toast(GTK_WIDGET(v), "Not connected");
    free(c->sql);
    g_free(c->arg);
    return;
  }
  DdlReq *r = g_new0(DdlReq, 1);
  r->conn = g_object_ref(conn);
  r->kind = lace_table_view_get_object_kind(v);
  r->object = g_strdup(lace_table_view_get_table(v));
  r->title = g_strdup(c->title ? c->title : "Change");
  r->sql = c->sql;
  r->arg = c->arg;
  r->after = c->after;
  g_weak_ref_init(&r->editor, c->editor);
  g_weak_ref_init(&r->editor_banner, c->editor_banner);
  g_weak_ref_init(&r->origin, v);
  const char *tx_note = "";
  if (lace_connection_get_tx_state(conn) == LACE_TX_OPEN) {
    LaceDriver d = lace_connection_get_driver(conn);
    tx_note = d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB ? "\n\nMySQL commits the open transaction first."
                                                                  : "\n\nRuns inside the open transaction: ROLLBACK undoes it.";
  }
  char *body = g_strdup_printf("%s\n\n%s%s", c->summary ? c->summary : "", c->sql, tx_note);
  lace_confirm(GTK_WIDGET(v), r->title, body, c->destructive ? r->title : "Run", c->destructive, confirmed, r);
  g_free(body);
}

/* ==========================================================================
 * Pickers' names, a query view for templates
 * ========================================================================== */

typedef struct {
  GWeakRef into;
} NamesReq;

static void names_req_free(gpointer p) {
  NamesReq *r = p;
  g_weak_ref_clear(&r->into);
  g_free(r);
}

static void names_done(LaceJob *job, gpointer data) {
  NamesReq *r = data;
  GtkStringList *into = g_weak_ref_get(&r->into);
  if (!into || job->error) {
    g_clear_object(&into);
    return;
  }
  const char **names = g_new0(const char *, job->num_tables + 1);
  for (size_t i = 0; i < job->num_tables; i++)
    names[i] = job->tables[i];
  gtk_string_list_splice(into, 0, g_list_model_get_n_items(G_LIST_MODEL(into)), names);
  g_free(names);
  g_object_unref(into);
}

void lace_ddl_fetch_names(LaceConnection *conn, GObject *owner, GtkStringList *into, const char *what,
                          const char *arg) {
  if (lace_connection_is_closed(conn))
    return;
  NamesReq *r = g_new0(NamesReq, 1);
  g_weak_ref_init(&r->into, into);
  lace_connection_submit_full(conn, lace_job_names(what, arg), owner, names_done, r, names_req_free);
}

void lace_ddl_open_query(LaceTableView *v, const char *text) {
  GtkWidget *page = gtk_widget_get_ancestor(GTK_WIDGET(v), LACE_TYPE_CONNECTION_PAGE);
  if (!page)
    return;
  LaceQueryView *q = lace_connection_page_new_query(LACE_CONNECTION_PAGE(page), text);
  if (q)
    lace_query_view_set_reload_tables(q, TRUE);
}
