/*
 * Lace GTK frontend
 * Table structure dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "schema_dialog.h"
#include "util.h"

static char *join_names(char **names, size_t n) {
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < n; i++) {
    if (i)
      g_string_append(s, ", ");
    g_string_append(s, names[i] ? names[i] : "");
  }
  return g_string_free(s, FALSE);
}

void lace_schema_dialog_show(GtkWidget *parent, const char *table,
                             const LaceSchema *schema) {
  AdwDialog *d = adw_dialog_new();
  adw_dialog_set_title(d, table);
  adw_dialog_set_content_width(d, 620);
  adw_dialog_set_content_height(d, 640);
  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *page = adw_preferences_page_new();

  GtkWidget *cols = adw_preferences_group_new();
  char *title = g_strdup_printf("%zu columns", schema ? schema->num_columns : 0);
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(cols), title);
  g_free(title);
  for (size_t i = 0; schema && i < schema->num_columns; i++) {
    const LaceColumn *c = &schema->columns[i];
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), c->name ? c->name : "");
    GString *sub = g_string_new(c->type_name && *c->type_name ? c->type_name : lace_type_short(c->type));
    if (!c->nullable)
      g_string_append(sub, " · NOT NULL");
    if (c->auto_increment)
      g_string_append(sub, " · auto-increment");
    if (c->generated)
      g_string_append(sub, " · generated");
    if (c->default_val && *c->default_val)
      g_string_append_printf(sub, " · default %s", c->default_val);
    if (c->foreign_key && *c->foreign_key)
      g_string_append_printf(sub, " · → %s", c->foreign_key);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub->str);
    g_string_free(sub, TRUE);
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
    if (c->primary_key) {
      GtkWidget *key = gtk_image_new_from_icon_name("lace-key-symbolic");
      gtk_widget_set_tooltip_text(key, "Primary key");
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), key);
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(cols), row);
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(cols));

  if (schema && schema->num_indexes > 0) {
    GtkWidget *g = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Indexes");
    for (size_t i = 0; i < schema->num_indexes; i++) {
      const LaceIndex *ix = &schema->indexes[i];
      GtkWidget *row = adw_action_row_new();
      adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), ix->name ? ix->name : "");
      char *names = join_names(ix->columns, ix->num_columns);
      char *sub = g_strdup_printf("%s%s%s", names, ix->unique ? " · unique" : "",
                                  ix->primary ? " · primary" : "");
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
      g_free(sub);
      g_free(names);
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), row);
    }
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));
  }

  if (schema && schema->num_foreign_keys > 0) {
    GtkWidget *g = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Foreign keys");
    for (size_t i = 0; i < schema->num_foreign_keys; i++) {
      const LaceForeignKey *fk = &schema->foreign_keys[i];
      GtkWidget *row = adw_action_row_new();
      char *from = join_names(fk->columns, fk->num_columns);
      char *to = join_names(fk->ref_columns, fk->num_ref_columns);
      char *t = g_strdup_printf("%s → %s (%s)", from, fk->ref_table ? fk->ref_table : "?", to);
      adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), t);
      char *sub = g_strdup_printf("%s%s%s%s%s", fk->name ? fk->name : "",
                                  fk->on_delete ? " · on delete " : "", fk->on_delete ? fk->on_delete : "",
                                  fk->on_update ? " · on update " : "", fk->on_update ? fk->on_update : "");
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
      g_free(sub);
      g_free(t);
      g_free(from);
      g_free(to);
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), row);
    }
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));
  }

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), scrolled);
  adw_dialog_set_child(d, tv);
  adw_dialog_present(d, parent);
}
