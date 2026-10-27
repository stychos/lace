/*
 * Lace GTK frontend
 * Scripted driving for headless tests and screenshots
 *
 * When LACE_GTK_SCRIPT is set, its ';'-separated commands run one after
 * another on the main loop once the first window exists:
 *
 *   wait MS             pause
 *   table NAME          open a table in the current connection
 *   object KIND NAME    open a view, routine or trigger from the sidebar's list
 *   subview NAME        show a sub-view of the table tab (data, structure, indexes,
 *                       foreign_keys, information)
 *   sidebar-toggle KIND fold or unfold a sidebar section (view, routine, trigger)
 *   follow              follow the focused cell's foreign key
 *   referenced-by       open the first table referencing the focused row
 *   dump                log the sidebar's rows and the sub-view's rows (and whether the view is locked)
 *   ddl ACTION          a structure or table action of the current table view: struct-add, struct-edit,
 *                       struct-drop, struct-rename, struct-key, struct-move, struct-check, rename-object,
 *                       truncate, drop-object, new-table, new-view, new-function, edit-definition
 *   list-row N          select the N-th row of the current sub-view list
 *   field KEY VALUE     set a field of the open structure editor (ddl_editors.h names the keys)
 *   save                press the open editor's Save
 *   answer go|cancel [TEXT]   answer the open confirmation or prompt (TEXT fills a prompt first)
 *   cancel              cancel the statement running on the current connection
 *   query SQL           open a SQL editor with the text
 *   run | run-all       run in the current editor
 *   action NAME         activate a window action (win.filter) or app action
 *   cell ROW COL        focus a grid cell of the current view
 *   edit TEXT           write TEXT into the focused cell (NULL: set NULL)
 *   select FROM TO      select rows of the current grid
 *   delete              delete the selected rows (no confirmation)
 *   filter COL OP VALUE add a filter chip to the current table view
 *   search TEXT         the table view's text search
 *   sort COL [desc]     sort the current table view by a column
 *   connect URL         open another connection tab
 *   shot NAME           screenshot the X display to $LACE_GTK_SHOTS/NAME.png
 *   heap [LABEL]        log glibc heap use
 *   form-uri TEXT       type TEXT into the current form's URI row, then leave it
 *   form-field KEY VAL  set a form field (form_page.h names the keys; VAL may be empty)
 *   form-pick NAME      highlight a saved connection or Recent entry in the form's list
 *   form-action ACTION  open, background, new-window, to-window:N, save, save-as-new, test, clear
 *   form-menu NAME      aim the form list's context menu at a row, as a right-click does
 *   form-search TEXT    type into the form list's search
 *   form-tree ACTION    a list action: new-folder, rename, duplicate, delete, forget
 *   transfer-tab N      move the current window's selected tab to window N (as a drag does)
 *   window N            make the N-th open window (in creation order) the current one
 *   close-window        close the current window
 *   quit                close every window
 *
 * Expectations (tests/smoke.sh): a failed one prints "script: expectation
 * failed: ..." and quits; the app then exits with status 3. Those marked
 * (waits) retry for up to 5 s first.
 *
 *   expect-windows N        open windows (waits)
 *   expect-pages N          the current window's tabs (waits)
 *   expect-page KIND        its selected tab: connection, form or none (waits)
 *   expect-title TEXT       its selected tab's title (waits)
 *   expect-tables           the current connection's tables are loaded (waits)
 *   expect-connections N    open database connections in the app (waits)
 *   expect-no-dialog        no dialog over the current window
 *   expect-indicator N      tabs of the current window showing a transaction mark (waits)
 *   expect-dialog TEXT      a dialog over the current window whose heading or text has TEXT (waits)
 *   expect-field KEY [VAL]  a field of the current form holds VAL (empty when left out) (waits)
 *   expect-uri-masked       the form's URI row shows the mask and no typed password
 *   expect-message TEXT     the form's message contains TEXT (waits)
 *   expect-saved NAME       connections.json has a saved connection of that name (waits)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "views/connection_page.h"
#include "views/form_page.h"
#include "views/filter_bar.h"
#include "views/query_view.h"
#include "views/sidebar.h"
#include "views/ddl_editors.h"
#include "views/table_view.h"
#include "util.h"
#include "window.h"
#include <stdlib.h>
#include <string.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

typedef struct {
  LaceApp *app;
  char **commands;
  int index;
  gint64 waiting_since; /* an expectation being retried, 0 if none */
} Script;

static gboolean g_failed;
/* The window "window N" picked, while the set of windows stays the same */
static GWeakRef g_picked;
static guint g_picked_count;

gboolean lace_debug_script_failed(void) { return g_failed; }

static guint n_windows(LaceApp *app) {
  GList *wins = lace_app_get_lace_windows(app);
  guint n = g_list_length(wins);
  g_list_free(wins);
  return n;
}

static LaceWindow *active_window(LaceApp *app) {
  LaceWindow *picked = g_weak_ref_get(&g_picked);
  if (picked) {
    g_object_unref(picked); /* the app's window list keeps it alive */
    if (n_windows(app) == g_picked_count)
      return picked;
  }
  GtkWindow *w = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (LACE_IS_WINDOW(w))
    return LACE_WINDOW(w);
  GList *wins = lace_app_get_lace_windows(app);
  LaceWindow *first = wins ? LACE_WINDOW(wins->data) : NULL;
  g_list_free(wins);
  return first;
}

static GtkWidget *current_view(LaceApp *app) {
  LaceWindow *w = active_window(app);
  LaceConnectionPage *cp = w ? lace_window_current_page(w) : NULL;
  return cp ? lace_connection_page_current_view(cp) : NULL;
}

static LaceDataGrid *current_grid(LaceApp *app) {
  GtkWidget *v = current_view(app);
  if (LACE_IS_TABLE_VIEW(v))
    return lace_table_view_get_grid(LACE_TABLE_VIEW(v));
  if (LACE_IS_QUERY_VIEW(v))
    return lace_query_view_get_grid(LACE_QUERY_VIEW(v));
  return NULL;
}

static gboolean step(gpointer data);

static void schedule(Script *s, guint ms) {
  g_timeout_add(ms, step, s);
}

/* The N-th open window by creation order (1-based) */
static LaceWindow *window_number(LaceApp *app, int n) {
  GList *wins = lace_app_get_lace_windows(app);
  LaceWindow *found = NULL;
  for (int k = 1; k <= n; k++) {
    LaceWindow *best = NULL;
    for (GList *l = wins; l; l = l->next) {
      LaceWindow *w = l->data;
      guint num = lace_window_get_number(w);
      guint prev = found ? lace_window_get_number(found) : 0;
      if (num > prev && (!best || num < lace_window_get_number(best)))
        best = w;
    }
    found = best;
  }
  g_list_free(wins);
  return found;
}

static const char *page_kind(LaceWindow *w) {
  AdwTabPage *page = w ? adw_tab_view_get_selected_page(lace_window_get_tab_view(w)) : NULL;
  if (!page)
    return "none";
  return LACE_IS_CONNECTION_PAGE(adw_tab_page_get_child(page)) ? "connection" : "form";
}

static guint n_connections(LaceApp *app) { return lace_app_n_connections(app); }

/* An expectation: true to go on; false when it is being retried (the step
 * is scheduled again) or failed (the app quits) */
static gboolean expect(Script *s, gboolean ok, gboolean waits, const char *cmd, const char *arg,
                       const char *seen) {
  if (ok) {
    s->waiting_since = 0;
    return TRUE;
  }
  gint64 now = g_get_monotonic_time();
  if (waits && (!s->waiting_since || now - s->waiting_since < 5 * G_USEC_PER_SEC)) {
    if (!s->waiting_since)
      s->waiting_since = now;
    s->index--; /* this command again */
    schedule(s, 100);
    return FALSE;
  }
  g_printerr("script: expectation failed: %s %s: saw %s\n", cmd, arg ? arg : "", seen);
  g_failed = TRUE;
  g_action_group_activate_action(G_ACTION_GROUP(s->app), "quit", NULL);
  return FALSE;
}

static gboolean step(gpointer data) {
  Script *s = data;
  char *cmd = s->commands[s->index];
  if (!cmd) {
    g_strfreev(s->commands);
    g_free(s);
    return G_SOURCE_REMOVE;
  }
  s->index++;
  cmd = g_strdup(cmd); /* parsed in place; the original is run again by a retry */
  g_strstrip(cmd);
  char *arg = strchr(cmd, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ')
      arg++;
  }
  LaceWindow *w = active_window(s->app);
  LaceConnectionPage *cp = w ? lace_window_current_page(w) : NULL;
  guint delay = 50;

  if (g_str_equal(cmd, "wait")) {
    delay = arg ? (guint)atoi(arg) : 100;
  } else if (g_str_equal(cmd, "table") && cp && arg) {
    lace_connection_page_open_table(cp, arg);
  } else if (g_str_equal(cmd, "object") && cp && arg) {
    char **parts = g_strsplit(arg, " ", 2);
    LaceObjectKind kind;
    if (parts[0] && parts[1] && lace_object_kind_from_key(parts[0], &kind)) {
      if (!lace_sidebar_open(lace_connection_page_get_sidebar(cp), kind, parts[1]))
        g_warning("script: object %s not in the sidebar", parts[1]);
    }
    g_strfreev(parts);
  } else if (g_str_equal(cmd, "subview") && arg && LACE_IS_TABLE_VIEW(current_view(s->app))) {
    LaceSubView sv;
    if (lace_subview_from_key(arg, &sv))
      lace_table_view_set_subview(LACE_TABLE_VIEW(current_view(s->app)), sv);
    delay = 300;
  } else if (g_str_equal(cmd, "sidebar-toggle") && cp && arg) {
    LaceObjectKind kind;
    if (lace_object_kind_from_key(arg, &kind))
      lace_sidebar_toggle_section(lace_connection_page_get_sidebar(cp), kind);
  } else if (g_str_equal(cmd, "follow")) {
    LaceDataGrid *g = current_grid(s->app);
    if (g)
      g_signal_emit_by_name(g, "follow-fk");
    delay = 300;
  } else if (g_str_equal(cmd, "referenced-by") && cp && LACE_IS_TABLE_VIEW(current_view(s->app))) {
    lace_table_view_referenced_by(LACE_TABLE_VIEW(current_view(s->app)));
    delay = 300;
  } else if (g_str_equal(cmd, "dump") && cp) {
    char **rows = lace_sidebar_get_texts(lace_connection_page_get_sidebar(cp));
    char *joined = g_strjoinv(" | ", rows);
    g_message("sidebar: %s", joined);
    g_free(joined);
    g_strfreev(rows);
    GtkWidget *v = current_view(s->app);
    if (LACE_IS_TABLE_VIEW(v)) {
      char **texts = lace_table_view_get_subview_texts(LACE_TABLE_VIEW(v));
      if (texts) {
        joined = g_strjoinv(" | ", texts);
        g_message("subview %s: %s", lace_subview_key(lace_table_view_get_subview(LACE_TABLE_VIEW(v)), lace_table_view_get_object_kind(LACE_TABLE_VIEW(v))), joined);
        g_free(joined);
        g_strfreev(texts);
      } else {
        g_message("subview data: %u rows",
                  g_list_model_get_n_items(G_LIST_MODEL(lace_data_grid_get_model(lace_table_view_get_grid(LACE_TABLE_VIEW(v))))));
      }
      g_message("locked: %s", lace_table_view_is_locked(LACE_TABLE_VIEW(v)) ? "yes" : "no");
    } else if (LACE_IS_QUERY_VIEW(v)) {
      g_message("query: %s", lace_query_view_get_status_text(LACE_QUERY_VIEW(v)));
      g_message("locked: %s", lace_query_view_is_locked(LACE_QUERY_VIEW(v)) ? "yes" : "no");
    }
  } else if (g_str_equal(cmd, "ddl") && arg && LACE_IS_TABLE_VIEW(current_view(s->app))) {
    lace_table_view_run_action(LACE_TABLE_VIEW(current_view(s->app)), arg);
    delay = 300;
  } else if (g_str_equal(cmd, "list-row") && arg && LACE_IS_TABLE_VIEW(current_view(s->app))) {
    if (!lace_table_view_select_list_row(LACE_TABLE_VIEW(current_view(s->app)), (guint)atoi(arg)))
      g_warning("script: no list row %s", arg);
  } else if (g_str_equal(cmd, "field") && arg) {
    char **parts = g_strsplit(arg, " ", 2);
    if (!parts[0] || !lace_ddl_editor_set_field(parts[0], parts[1] ? parts[1] : ""))
      g_warning("script: no editor field %s", parts[0] ? parts[0] : "");
    g_strfreev(parts);
  } else if (g_str_equal(cmd, "save")) {
    if (!lace_ddl_editor_save())
      g_warning("script: no editor is open");
    delay = 300;
  } else if (g_str_equal(cmd, "cancel") && cp) {
    lace_connection_cancel(lace_connection_page_get_connection(cp));
    delay = 500;
  } else if (g_str_equal(cmd, "answer") && arg) {
    char **parts = g_strsplit(arg, " ", 2);
    if (!lace_debug_answer_alert(parts[0], parts[1]))
      g_warning("script: no alert is open");
    g_strfreev(parts);
    delay = 500;
  } else if (g_str_equal(cmd, "query") && cp) {
    lace_connection_page_new_query(cp, arg ? arg : "");
  } else if ((g_str_equal(cmd, "run") || g_str_equal(cmd, "run-all")) && LACE_IS_QUERY_VIEW(current_view(s->app))) {
    lace_query_view_run(LACE_QUERY_VIEW(current_view(s->app)),
                        g_str_equal(cmd, "run") ? LACE_RUN_AT_CURSOR : LACE_RUN_ALL);
  } else if (g_str_equal(cmd, "action") && arg) {
    if (g_str_has_prefix(arg, "app."))
      g_action_group_activate_action(G_ACTION_GROUP(s->app), arg + 4, NULL);
    else if (w)
      gtk_widget_activate_action(GTK_WIDGET(w), arg, NULL);
  } else if (g_str_equal(cmd, "cell") && arg) {
    LaceDataGrid *g = current_grid(s->app);
    int row = 0, col = 0;
    sscanf(arg, "%d %d", &row, &col);
    if (g)
      lace_data_grid_focus_cell(g, (gsize)row, (gsize)col);
  } else if (g_str_equal(cmd, "edit")) {
    LaceDataGrid *g = current_grid(s->app);
    gsize row, col;
    if (g && lace_data_grid_get_focus_cell(g, &row, &col)) {
      gboolean null = arg && g_str_equal(arg, "NULL");
      g_signal_emit_by_name(g, "edit-cell", (guint)row, (guint)col, null ? NULL : (arg ? arg : ""), null);
    }
  } else if (g_str_equal(cmd, "select") && arg) {
    LaceDataGrid *g = current_grid(s->app);
    int from = 0, to = 0;
    sscanf(arg, "%d %d", &from, &to);
    if (g) {
      GtkSelectionModel *sel = gtk_column_view_get_model(lace_data_grid_get_column_view(g));
      gtk_selection_model_select_range(sel, (guint)from, (guint)(to - from + 1), TRUE);
    }
  } else if (g_str_equal(cmd, "delete")) {
    LaceDataGrid *g = current_grid(s->app);
    if (g)
      g_signal_emit_by_name(g, "delete-rows");
  } else if (g_str_equal(cmd, "filter") && arg) {
    GtkWidget *v = current_view(s->app);
    char **parts = g_strsplit(arg, " ", 3);
    if (LACE_IS_TABLE_VIEW(v) && parts[0] && parts[1]) {
      LaceDataGrid *g = lace_table_view_get_grid(LACE_TABLE_VIEW(v));
      (void)g;
      /* Through the grid's signal, as the context menu would */
      g_signal_emit_by_name(g, "filter-column", parts[0], parts[2] ? parts[2] : "");
    }
    g_strfreev(parts);
  } else if (g_str_equal(cmd, "search") && arg) {
    GtkWidget *v = current_view(s->app);
    if (LACE_IS_TABLE_VIEW(v)) {
      gsize n;
      const LaceFilter *f = lace_table_view_get_filters(LACE_TABLE_VIEW(v), &n);
      gsize ns;
      LaceSortSpec *sorts = lace_table_view_get_sorts(LACE_TABLE_VIEW(v), &ns);
      lace_table_view_restore(LACE_TABLE_VIEW(v), f, n, arg, sorts, ns, TRUE);
      for (gsize i = 0; i < ns; i++)
        g_free(sorts[i].column);
      g_free(sorts);
    }
  } else if (g_str_equal(cmd, "sort") && arg) {
    GtkWidget *v = current_view(s->app);
    char **parts = g_strsplit(arg, " ", 2);
    if (LACE_IS_TABLE_VIEW(v) && parts[0]) {
      LaceSortSpec spec = {parts[0], parts[1] && g_str_equal(parts[1], "desc")};
      lace_data_grid_set_sorts(lace_table_view_get_grid(LACE_TABLE_VIEW(v)), &spec, 1);
      g_signal_emit_by_name(lace_table_view_get_grid(LACE_TABLE_VIEW(v)), "sort-changed");
    }
    g_strfreev(parts);
  } else if (g_str_equal(cmd, "connect") && w && arg) {
    lace_window_connect_to(w, arg, NULL, NULL); /* through lace_app_open: an open one is used */
  } else if (g_str_equal(cmd, "shot") && arg) {
    const char *dir = g_getenv("LACE_GTK_SHOTS");
    char *path = g_strdup_printf("%s/%s.png", dir ? dir : ".", arg);
    char *line = g_strdup_printf("import -window root %s", path);
    g_spawn_command_line_sync(line, NULL, NULL, NULL, NULL);
    g_free(line);
    g_free(path);
  } else if (g_str_equal(cmd, "heap")) {
#ifdef __GLIBC__
    struct mallinfo2 mi = mallinfo2();
    g_message("heap %s: in use %.1f MB (sbrk %.1f MB, mmap %.1f MB, free %.1f MB)",
              arg ? arg : "", (double)mi.uordblks / 1048576.0, (double)mi.arena / 1048576.0,
              (double)mi.hblkhd / 1048576.0, (double)mi.fordblks / 1048576.0);
    LaceDataGrid *grid = current_grid(s->app);
    if (grid) {
      guint cells = 0, rows = 0;
      lace_data_grid_debug_counts(grid, &cells, &rows);
      g_message("grid: %u bound cells, %u row widgets, %u model items",
                cells, rows, g_list_model_get_n_items(G_LIST_MODEL(lace_data_grid_get_model(grid))));
    }
#endif
  } else if (g_str_has_prefix(cmd, "form-")) {
    LaceFormPage *form = w ? lace_window_current_form(w) : NULL;
    if (!form) {
      g_warning("script: unknown or inapplicable command '%s' (no form)", cmd);
    } else if (g_str_equal(cmd, "form-uri")) {
      lace_form_page_set_uri(form, arg ? arg : "");
    } else if (g_str_equal(cmd, "form-field") && arg) {
      char **kv = g_strsplit(arg, " ", 2);
      if (!lace_form_page_set_field(form, kv[0], kv[1] ? kv[1] : ""))
        g_warning("script: unknown or inapplicable command '%s %s'", cmd, kv[0]);
      g_strfreev(kv);
    } else if (g_str_equal(cmd, "form-pick") && arg) {
      if (!lace_form_page_pick(form, arg))
        g_warning("script: unknown or inapplicable command '%s %s' (not listed)", cmd, arg);
      delay = 300; /* a question may come up */
    } else if (g_str_equal(cmd, "form-action") && arg) {
      lace_form_page_action(form, arg);
      delay = 300;
    } else if (g_str_equal(cmd, "form-menu") && arg) {
      if (!lace_form_page_menu_at(form, arg))
        g_warning("script: unknown or inapplicable command '%s %s' (not listed)", cmd, arg);
    } else if (g_str_equal(cmd, "form-search")) {
      lace_form_page_search(form, arg);
    } else if (g_str_equal(cmd, "form-tree") && arg) {
      lace_form_page_tree_action(form, arg);
      delay = 300;
    } else {
      g_warning("script: unknown or inapplicable command '%s'", cmd);
    }
  } else if (g_str_has_prefix(cmd, "expect-")) {
    char seen[256] = "";
    gboolean ok, waits = TRUE;
    LaceFormPage *form = w ? lace_window_current_form(w) : NULL;
    if (g_str_equal(cmd, "expect-windows") && arg) {
      g_snprintf(seen, sizeof seen, "%u", n_windows(s->app));
      ok = (int)n_windows(s->app) == atoi(arg);
    } else if (g_str_equal(cmd, "expect-pages") && arg) {
      int n = w ? adw_tab_view_get_n_pages(lace_window_get_tab_view(w)) : -1;
      g_snprintf(seen, sizeof seen, "%d", n);
      ok = n == atoi(arg);
    } else if (g_str_equal(cmd, "expect-page") && arg) {
      g_snprintf(seen, sizeof seen, "%s", page_kind(w));
      ok = g_str_equal(page_kind(w), arg);
    } else if (g_str_equal(cmd, "expect-title") && arg) {
      AdwTabPage *page = w ? adw_tab_view_get_selected_page(lace_window_get_tab_view(w)) : NULL;
      const char *t = page ? adw_tab_page_get_title(page) : NULL;
      g_snprintf(seen, sizeof seen, "%s", t ? t : "(no tab)");
      ok = t && g_str_equal(t, arg);
    } else if (g_str_equal(cmd, "expect-tables")) {
      ok = cp && lace_connection_tables_loaded(lace_connection_page_get_connection(cp));
      g_snprintf(seen, sizeof seen, "%s", cp ? "tables not loaded" : "no connection tab");
    } else if (g_str_equal(cmd, "expect-connections") && arg) {
      g_snprintf(seen, sizeof seen, "%u", n_connections(s->app));
      ok = (int)n_connections(s->app) == atoi(arg);
    } else if (g_str_equal(cmd, "expect-field") && form) {
      char **kv = g_strsplit(arg ? arg : "", " ", 2);
      char *v = kv[0] ? lace_form_page_get_field(form, kv[0]) : NULL;
      g_snprintf(seen, sizeof seen, "'%s'", v ? v : "(unknown field)");
      ok = v && g_str_equal(v, kv[1] ? kv[1] : "");
      g_free(v);
      g_strfreev(kv);
    } else if (g_str_equal(cmd, "expect-uri-masked") && form) {
      const char *u = lace_form_page_get_uri(form);
      char *pw = lace_form_page_get_field(form, "password");
      g_snprintf(seen, sizeof seen, "'%s'", u ? u : "");
      ok = u && strstr(u, "\xe2\x80\xa2") && (!pw || !*pw || !strstr(u, pw));
      g_free(pw);
      waits = FALSE;
    } else if (g_str_equal(cmd, "expect-message") && form && arg) {
      const char *m = lace_form_page_get_message(form);
      g_snprintf(seen, sizeof seen, "'%s'", m ? m : "");
      ok = m && strstr(m, arg);
    } else if (g_str_equal(cmd, "expect-saved") && arg) {
      char *err = NULL;
      ConnectionManager *mgr = connmgr_load(&err);
      free(err);
      ok = FALSE;
      for (size_t i = 0; mgr && !ok && i < connmgr_count_filtered(mgr, arg); i++) {
        ConnectionItem *it = connmgr_get_filtered_item(mgr, arg, i);
        ok = it && it->type == CONN_ITEM_CONNECTION && g_strcmp0(it->connection.name, arg) == 0;
      }
      g_snprintf(seen, sizeof seen, "no saved connection called %s", arg);
      if (mgr)
        connmgr_free(mgr);
    } else if (g_str_equal(cmd, "expect-dialog") && arg) {
      AdwDialog *d = w ? adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w)) : NULL;
      const char *heading = ADW_IS_ALERT_DIALOG(d) ? adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(d)) : NULL;
      const char *body = ADW_IS_ALERT_DIALOG(d) ? adw_alert_dialog_get_body(ADW_ALERT_DIALOG(d)) : NULL;
      g_snprintf(seen, sizeof seen, "%s", d ? (body ? body : "a dialog") : "no dialog");
      ok = (heading && strstr(heading, arg)) || (body && strstr(body, arg));
    } else if (g_str_equal(cmd, "expect-indicator") && arg) {
      int n = 0;
      AdwTabView *tv = w ? lace_window_get_tab_view(w) : NULL;
      for (int i = 0; tv && i < adw_tab_view_get_n_pages(tv); i++)
        n += adw_tab_page_get_indicator_icon(adw_tab_view_get_nth_page(tv, i)) != NULL;
      g_snprintf(seen, sizeof seen, "%d", n);
      ok = n == atoi(arg);
    } else if (g_str_equal(cmd, "expect-no-dialog")) {
      AdwDialog *d = w ? adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w)) : NULL;
      g_snprintf(seen, sizeof seen, "%s", d ? adw_dialog_get_title(d) : "none");
      ok = d == NULL;
      waits = FALSE;
    } else {
      g_warning("script: unknown or inapplicable command '%s'", cmd);
      g_free(cmd);
      schedule(s, delay);
      return G_SOURCE_REMOVE;
    }
    gboolean go_on = expect(s, ok, waits, cmd, arg, seen);
    g_free(cmd);
    if (go_on)
      schedule(s, delay);
    return G_SOURCE_REMOVE;
  } else if (g_str_equal(cmd, "window") && arg) {
    LaceWindow *pick = window_number(s->app, atoi(arg));
    if (pick) {
      g_weak_ref_set(&g_picked, pick);
      g_picked_count = n_windows(s->app);
      gtk_window_present(GTK_WINDOW(pick));
    } else {
      g_warning("script: no window %s", arg);
    }
  } else if (g_str_equal(cmd, "transfer-tab") && arg && w) {
    LaceWindow *to = window_number(s->app, atoi(arg));
    AdwTabPage *page = adw_tab_view_get_selected_page(lace_window_get_tab_view(w));
    if (to && page && to != w)
      adw_tab_view_transfer_page(lace_window_get_tab_view(w), page, lace_window_get_tab_view(to),
                                 adw_tab_view_get_n_pages(lace_window_get_tab_view(to)));
    else
      g_warning("script: unknown or inapplicable command '%s %s'", cmd, arg);
  } else if (g_str_equal(cmd, "close-window") && w) {
    gtk_window_close(GTK_WINDOW(w));
  } else if (g_str_equal(cmd, "quit")) {
    g_free(cmd);
    g_action_group_activate_action(G_ACTION_GROUP(s->app), "quit", NULL);
    return G_SOURCE_REMOVE;
  } else if (*cmd) {
    g_warning("script: unknown or inapplicable command '%s'", cmd);
  }
  g_free(cmd);
  schedule(s, delay);
  return G_SOURCE_REMOVE;
}

void lace_debug_script_start(LaceApp *app) {
  const char *text = g_getenv("LACE_GTK_SCRIPT");
  if (!text || !*text)
    return;
  static gboolean started;
  if (started)
    return;
  started = TRUE;
  Script *s = g_new0(Script, 1);
  s->app = app;
  s->commands = g_strsplit(text, ";", -1);
  schedule(s, 300);
}
