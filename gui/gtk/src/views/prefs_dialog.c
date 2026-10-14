/*
 * Lace GTK frontend
 * Preferences dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "prefs_dialog.h"
#include <config/history.h>

typedef struct {
  LaceApp *app;
  GtkWidget *parent;
} PrefsCtx;

static LacePrefs *prefs(PrefsCtx *c) { return lace_app_get_prefs(c->app); }

static void save(PrefsCtx *c) { lace_app_save_prefs(c->app, c->parent); }

static void on_switch(GObject *row, GParamSpec *p, PrefsCtx *c) {
  (void)p;
  const char *key = g_object_get_data(row, "key");
  gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  LacePrefs *pr = prefs(c);
  if (g_str_equal(key, "confirm_delete")) pr->confirm_delete = on;
  else if (g_str_equal(key, "restore_session")) pr->restore_session = on;
  else if (g_str_equal(key, "approximate_counts")) pr->approximate_counts = on;
  save(c);
}

static void on_combo(GObject *row, GParamSpec *p, PrefsCtx *c) {
  (void)p;
  const char *key = g_object_get_data(row, "key");
  int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  LacePrefs *pr = prefs(c);
  if (g_str_equal(key, "color_scheme")) {
    pr->color_scheme = sel;
    lace_app_apply_color_scheme(c->app);
  } else if (g_str_equal(key, "history_mode")) {
    pr->history_mode = sel;
  } else if (g_str_equal(key, "daemon_spawn_mode")) {
    pr->daemon_spawn_mode = sel;
  } else if (g_str_equal(key, "row_numbers")) {
    pr->row_numbers = sel;
  }
  save(c);
}

static void on_spin(GObject *row, GParamSpec *p, PrefsCtx *c) {
  (void)p;
  const char *key = g_object_get_data(row, "key");
  int val = (int)adw_spin_row_get_value(ADW_SPIN_ROW(row));
  LacePrefs *pr = prefs(c);
  if (g_str_equal(key, "page_size")) pr->page_size = val;
  else if (g_str_equal(key, "history_max_size")) pr->history_max_size = val;
  else if (g_str_equal(key, "daemon_idle_timeout")) pr->daemon_idle_timeout = val;
  save(c);
}

static GtkWidget *switch_row(PrefsCtx *c, const char *title, const char *subtitle,
                             const char *key, gboolean value) {
  GtkWidget *row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), value);
  g_object_set_data(G_OBJECT(row), "key", (gpointer)key);
  g_signal_connect(row, "notify::active", G_CALLBACK(on_switch), c);
  return row;
}

static GtkWidget *combo_row(PrefsCtx *c, const char *title, const char *subtitle,
                            const char *key, const char *const *options, int value) {
  GtkWidget *row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  GtkStringList *list = gtk_string_list_new(options);
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(list));
  g_object_unref(list);
  adw_combo_row_set_selected(ADW_COMBO_ROW(row), (guint)value);
  g_object_set_data(G_OBJECT(row), "key", (gpointer)key);
  g_signal_connect(row, "notify::selected", G_CALLBACK(on_combo), c);
  return row;
}

static GtkWidget *spin_row(PrefsCtx *c, const char *title, const char *subtitle,
                           const char *key, double min, double max, double step, int value) {
  GtkWidget *row = adw_spin_row_new_with_range(min, max, step);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_spin_row_set_value(ADW_SPIN_ROW(row), value);
  g_object_set_data(G_OBJECT(row), "key", (gpointer)key);
  g_signal_connect(row, "notify::value", G_CALLBACK(on_spin), c);
  return row;
}

static void on_closed(AdwDialog *d, PrefsCtx *c) {
  (void)d;
  g_free(c);
}

void lace_prefs_dialog_show(LaceApp *app, GtkWidget *parent) {
  PrefsCtx *c = g_new0(PrefsCtx, 1);
  c->app = app;
  c->parent = parent;
  LacePrefs *pr = lace_app_get_prefs(app);

  AdwDialog *d = adw_preferences_dialog_new();
  adw_dialog_set_title(d, "Preferences");
  GtkWidget *page = adw_preferences_page_new();

  GtkWidget *g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Appearance");
  const char *schemes[] = {"Follow the system", "Light", "Dark", NULL};
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      combo_row(c, "Colour scheme", NULL, "color_scheme", schemes, pr->color_scheme));
  const char *rownums[] = {"Never", "Unless the key is an auto-increment integer", "Always", NULL};
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      combo_row(c, "Row numbers", "A row number column in grids (new tabs)", "row_numbers",
                rownums, pr->row_numbers));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));

  g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Behaviour");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      switch_row(c, "Confirm deletes", "Ask before deleting rows", "confirm_delete", pr->confirm_delete));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      switch_row(c, "Restore windows at startup", "Reopen the windows, connections and tabs of the last session",
                 "restore_session", pr->restore_session));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      switch_row(c, "Approximate row counts", "Use catalogue estimates for large PostgreSQL and MySQL tables (shown with ≈)",
                 "approximate_counts", pr->approximate_counts));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      spin_row(c, "Rows per page", "Rows fetched per page as you scroll (new tabs)", "page_size",
               100, 100000, 100, pr->page_size));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));

  g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Query history");
  const char *modes[] = {"Off", "This session only", "Saved per connection", NULL};
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      combo_row(c, "History", "Saved history is shared with the terminal frontend (new connections)",
                "history_mode", modes, pr->history_mode));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      spin_row(c, "Entries per connection", NULL, "history_max_size",
               LACE_HISTORY_SIZE_MIN, LACE_HISTORY_SIZE_MAX, 10, pr->history_max_size));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));

  g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Daemon");
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),
      "How the laced daemon is started when none is running. Takes effect at the next start.");
  const char *spawn[] = {"Unix socket (shared with other frontends)", "Private (pipes, exits with the app)",
                         "TCP on localhost", "Detect: use a running daemon, else private", NULL};
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      combo_row(c, "Daemon mode", NULL, "daemon_spawn_mode", spawn, pr->daemon_spawn_mode));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),
      spin_row(c, "Idle timeout", "Seconds a shared daemon keeps running without clients (0: forever)",
               "daemon_idle_timeout", 0, 86400, 60, pr->daemon_idle_timeout));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));

  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(d), ADW_PREFERENCES_PAGE(page));
  g_signal_connect(d, "closed", G_CALLBACK(on_closed), c);
  adw_dialog_present(d, parent);
}
