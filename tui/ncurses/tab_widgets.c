/*
 * Lace
 * Tab widget lifecycle management
 *
 * Handles creation, destruction, and sync of widgets for UITabState.
 * This bridges the legacy flat-field state with the new widget architecture.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "tui.h"
#include "tui_internal.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Widget creation helpers
 * ============================================================================
 */

/* Create a TableWidget for a table tab */
static TableWidget *create_table_widget(TuiState *state, Tab *tab) {
  if (!state || !tab)
    return NULL;

  TableWidget *tw = table_widget_create(state->app, tab);
  if (!tw)
    return NULL;

  /* Sync from tab data */
  table_widget_sync_from_tab(tw);

  return tw;
}

/* Create a FiltersWidget for a tab */
static FiltersWidget *create_filters_widget(Tab *tab) {
  if (!tab)
    return NULL;

  FiltersWidget *fw = filters_widget_create();
  if (!fw)
    return NULL;

  /* Bind to tab's filters and schema */
  filters_widget_bind(fw, &tab->filters, tab->schema);

  return fw;
}

/* Create a QueryWidget for a query tab */
static QueryWidget *create_query_widget(TuiState *state, Tab *tab) {
  if (!state || !tab)
    return NULL;

  QueryWidget *qw = query_widget_create(state->app, tab);
  if (!qw)
    return NULL;

  return qw;
}

/* ============================================================================
 * Widget initialization for tabs
 * ============================================================================
 */

bool tui_init_table_tab_widgets(TuiState *state, UITabState *ui, Tab *tab) {
  if (!state || !ui || !tab)
    return false;

  /* Skip if already initialized */
  if (ui->table_widget)
    return true;

  /* Create table widget */
  ui->table_widget = create_table_widget(state, tab);
  if (!ui->table_widget)
    return false;

  /* Create filters widget */
  ui->filters_widget = create_filters_widget(tab);
  if (!ui->filters_widget) {
    table_widget_destroy(ui->table_widget);
    ui->table_widget = NULL;
    return false;
  }

  /* Initialize FocusManager and register widgets */
  focus_manager_init(&ui->focus_mgr);
  focus_manager_add_widget(&ui->focus_mgr, &ui->table_widget->base);
  focus_manager_add_widget(&ui->focus_mgr, &ui->filters_widget->base);
  /* Sidebar will be added later when sidebar_widget is created */

  /* Set initial focus to table */
  focus_manager_set_focus(&ui->focus_mgr, &ui->table_widget->base);

  return true;
}

bool tui_init_query_tab_widgets(TuiState *state, UITabState *ui, Tab *tab) {
  if (!state || !ui || !tab)
    return false;

  /* Skip if already initialized */
  if (ui->query_widget)
    return true;

  /* Create query widget */
  ui->query_widget = create_query_widget(state, tab);
  if (!ui->query_widget)
    return false;

  /* Initialize FocusManager and register widgets */
  focus_manager_init(&ui->focus_mgr);
  focus_manager_add_widget(&ui->focus_mgr, &ui->query_widget->base);
  /* Sidebar will be added later when sidebar_widget is created */

  /* Set initial focus to query editor */
  focus_manager_set_focus(&ui->focus_mgr, &ui->query_widget->base);

  return true;
}

void tui_cleanup_tab_widgets(UITabState *ui) {
  if (!ui)
    return;

  /* Clear FocusManager */
  focus_manager_clear(&ui->focus_mgr);

  /* Destroy widgets */
  if (ui->table_widget) {
    table_widget_destroy(ui->table_widget);
    ui->table_widget = NULL;
  }

  if (ui->filters_widget) {
    filters_widget_destroy(ui->filters_widget);
    ui->filters_widget = NULL;
  }

  if (ui->query_widget) {
    query_widget_destroy(ui->query_widget);
    ui->query_widget = NULL;
  }

  /* Destroy sidebar widget - each tab has its own */
  if (ui->sidebar_widget) {
    sidebar_widget_destroy(ui->sidebar_widget);
    ui->sidebar_widget = NULL;
  }
}

/* ============================================================================
 * Focus management
 * ============================================================================
 */

Widget *tui_get_focused_widget(UITabState *ui) {
  if (!ui)
    return NULL;
  return focus_manager_get_focus(&ui->focus_mgr);
}

void tui_set_focused_widget(UITabState *ui, Widget *widget) {
  if (!ui)
    return;

  /* Use FocusManager for focus changes */
  focus_manager_set_focus(&ui->focus_mgr, widget);

  /* Update legacy focus flags based on which widget has focus */
  if (ui->filters_widget && widget == &ui->filters_widget->base) {
    ui->filters_focused = true;
    ui->sidebar_focused = false;
  } else if (ui->sidebar_widget && widget == &ui->sidebar_widget->base) {
    ui->sidebar_focused = true;
    ui->filters_focused = false;
  } else {
    ui->sidebar_focused = false;
    ui->filters_focused = false;
  }
}

void tui_cycle_widget_focus(UITabState *ui) {
  if (!ui)
    return;

  /* Use FocusManager to cycle focus (skips hidden widgets) */
  focus_manager_cycle_next(&ui->focus_mgr);

  /* Update legacy focus flags */
  Widget *focused = focus_manager_get_focus(&ui->focus_mgr);
  if (ui->filters_widget && focused == &ui->filters_widget->base) {
    ui->filters_focused = true;
    ui->sidebar_focused = false;
  } else if (ui->sidebar_widget && focused == &ui->sidebar_widget->base) {
    ui->sidebar_focused = true;
    ui->filters_focused = false;
  } else {
    ui->sidebar_focused = false;
    ui->filters_focused = false;
  }
}

/* ============================================================================
 * Widget Accessors
 * ============================================================================
 */

TableWidget *tui_table_widget(TuiState *state) {
  UITabState *ui = tui_current_tab_ui(state);
  if (!ui)
    return NULL;

  /* Only valid for table tabs */
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return NULL;

  return ui->table_widget;
}

FiltersWidget *tui_filters_widget(TuiState *state) {
  UITabState *ui = tui_current_tab_ui(state);
  if (!ui)
    return NULL;

  /* Only valid for table tabs */
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE)
    return NULL;

  return ui->filters_widget;
}

QueryWidget *tui_query_widget_for_tab(TuiState *state) {
  UITabState *ui = tui_current_tab_ui(state);
  if (!ui)
    return NULL;

  /* Only valid for query tabs */
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_QUERY)
    return NULL;

  return ui->query_widget;
}

SidebarWidget *tui_sidebar_widget(TuiState *state) {
  UITabState *ui = tui_current_tab_ui(state);
  if (!ui)
    return NULL;

  return ui->sidebar_widget;
}

SidebarWidget *tui_ensure_sidebar_widget(TuiState *state) {
  if (!state || !state->app)
    return NULL;

  UITabState *ui = tui_current_tab_ui(state);
  if (!ui)
    return NULL;

  /* If sidebar widget already exists, return it */
  if (ui->sidebar_widget)
    return ui->sidebar_widget;

  /* Get current connection */
  Connection *conn = TUI_TAB_CONNECTION(state);
  if (!conn || !conn->active)
    return NULL;

  /* Create sidebar widget */
  SidebarWidget *sw = sidebar_widget_create(state->app);
  if (!sw)
    return NULL;

  /* Bind to connection */
  sidebar_widget_bind(sw, conn);

  /* Store in UI state */
  ui->sidebar_widget = sw;

  /* Register with FocusManager */
  focus_manager_add_widget(&ui->focus_mgr, &sw->base);

  /* Sync initial state from legacy fields */
  sw->base.state.cursor_row = ui->sidebar_highlight;
  sw->base.state.scroll_row = ui->sidebar_scroll;
  sw->base.state.visible = ui->sidebar_visible;
  sw->base.state.focused = ui->sidebar_focused;

  /* Copy filter if any */
  if (ui->sidebar_filter_len > 0) {
    sidebar_widget_set_filter(sw, ui->sidebar_filter);
  }

  return sw;
}
