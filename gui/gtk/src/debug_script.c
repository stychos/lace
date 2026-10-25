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
 *   dump                log the sidebar's rows and the sub-view's rows
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
 *   quit                close every window
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "app.h"
#include "views/connection_page.h"
#include "views/filter_bar.h"
#include "views/query_view.h"
#include "views/sidebar.h"
#include "views/table_view.h"
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
} Script;

static LaceWindow *active_window(LaceApp *app) {
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

static gboolean step(gpointer data) {
  Script *s = data;
  char *cmd = s->commands[s->index];
  if (!cmd) {
    g_strfreev(s->commands);
    g_free(s);
    return G_SOURCE_REMOVE;
  }
  s->index++;
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
    }
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
    lace_window_connect_to(w, arg, NULL, NULL);
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
  } else if (g_str_equal(cmd, "quit")) {
    g_action_group_activate_action(G_ACTION_GROUP(s->app), "quit", NULL);
    return G_SOURCE_REMOVE;
  } else if (*cmd) {
    g_warning("script: unknown or inapplicable command '%s'", cmd);
  }
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
