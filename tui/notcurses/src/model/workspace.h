/* Workspaces: a named set of connection tabs, each holding the views
 * (table tabs, query tabs) opened on its connection. Serializes to the
 * nested workspace object every frontend shares (session.json version 2). */
#ifndef NC_WORKSPACE_H
#define NC_WORKSPACE_H

#include "conn/connection.h"
#include "conn/jobs.h"
#include "model/view.h"
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ConnTab {
  uint64_t uid;       /* unique for the process: completions and dialogs find the tab by it */
  NcConnection *conn; /* held through the app's registry (app_conn_hold); NULL for a form */
  struct FormTab *form; /* a connection form (ui/form_tab.c); NULL when connected */
  NcOwner *owner; /* released in workspace_remove_tab: done callbacks of its jobs are then skipped */
  View **views;
  size_t num_views, current;
  bool sidebar;
  char *sidebar_search;
  size_t sidebar_cursor, sidebar_scroll;
  size_t sidebar_search_pos; /* the search field's cursor (byte offset) */
  bool sidebar_focused;
  bool sidebar_from_panel; /* the arrows entered the sidebar from the filter panel: Right goes back (runtime) */
  bool auto_open; /* open the first table once the list is known (general.auto_open_first_table; runtime) */
  uint64_t sidebar_view_uid; /* the view the sidebar cursor last followed (runtime) */
  bool sidebar_search_visible; /* the search field was called (sidebar_filter); it hides again
                                * when the sidebar loses the focus with no text (runtime) */
  unsigned sidebar_collapsed;  /* bit per sidebar section (ui/sidebar.c) folded away (runtime) */
} ConnTab;

typedef struct NcWorkspace {
  char *name;
  ConnTab **tabs;
  size_t num_tabs, current;
} NcWorkspace;

NcWorkspace *workspace_new(const char *name);
void workspace_free(NcWorkspace *w); /* frees tabs and views; connections are closed by the app first */
ConnTab *workspace_add_tab(NcWorkspace *w, NcConnection *c); /* appended; becomes current */
/* at >= num_tabs appends; make_current false keeps the current tab current */
ConnTab *workspace_insert_tab(NcWorkspace *w, size_t at, NcConnection *c, bool make_current);
size_t workspace_find_tab(const NcWorkspace *w, uint64_t uid); /* SIZE_MAX when absent */
/* A connection form tab at index at (past the end: appended) */
ConnTab *workspace_add_form(NcWorkspace *w, size_t at, bool make_current);
/* The tab becomes a form in place: its views go, its owner is renewed (late
 * completions are dropped); its connection must already be released (conn NULL) */
void conntab_make_form(ConnTab *t);
/* The form becomes connected to c, with its sidebar */
void conntab_make_connected(ConnTab *t, NcConnection *c);
void workspace_remove_tab(NcWorkspace *w, size_t i);         /* frees the tab; the app closes the connection */
ConnTab *workspace_current_tab(const NcWorkspace *w);
View *conntab_add_view(ConnTab *t, View *v); /* becomes current */
void conntab_remove_view(ConnTab *t, size_t i);
View *conntab_current_view(const ConnTab *t);
size_t conntab_find_table_view(const ConnTab *t, const char *table); /* a table's or a database view's; SIZE_MAX when none */
/* The view showing that object: tables and database views share a namespace,
 * routines and triggers have their own */
size_t conntab_find_object_view(const ConnTab *t, LaceObjectKind kind, const char *name);
size_t conntab_find_preview(const ConnTab *t);                        /* the preview view; SIZE_MAX when none */
View *conntab_replace_view(ConnTab *t, size_t i, View *v); /* frees the view at i, v takes its place and becomes current */
cJSON *workspace_to_json(const NcWorkspace *w, bool with_cursor);
cJSON *view_to_json(const View *v, bool with_cursor); /* one view; inactive (blank panel) filters left out */

#endif
