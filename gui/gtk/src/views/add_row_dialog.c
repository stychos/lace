/*
 * Lace GTK frontend
 * Add-row dialog
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "add_row_dialog.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  GtkWidget *row;        /* AdwEntryRow */
  GtkWidget *null_toggle;
  GtkWidget *empty_toggle;
  gboolean omitted;      /* Auto/generated: never part of the INSERT */
} ColumnEntry;

typedef struct {
  AdwDialog *dialog;
  LaceConnection *conn;
  char *table;
  LaceSchema *schema; /* Deep copy: the view's schema may be replaced */
  ColumnEntry *entries;
  size_t n;
  GtkWidget *banner;
  GtkWidget *insert_button;
  LaceAddRowDone done;
  gpointer user_data;
  gboolean inserted;
} AddRow;

static LaceSchema *schema_copy(const LaceSchema *s) {
  LaceSchema *c = g_new0(LaceSchema, 1);
  c->name = g_strdup(s->name);
  c->num_columns = s->num_columns;
  c->columns = g_new0(LaceColumn, s->num_columns ? s->num_columns : 1);
  for (size_t i = 0; i < s->num_columns; i++) {
    c->columns[i] = s->columns[i];
    c->columns[i].name = g_strdup(s->columns[i].name);
    c->columns[i].type_name = g_strdup(s->columns[i].type_name);
    c->columns[i].default_val = g_strdup(s->columns[i].default_val);
    c->columns[i].foreign_key = g_strdup(s->columns[i].foreign_key);
  }
  return c;
}

static void schema_copy_free(LaceSchema *c) {
  if (!c)
    return;
  for (size_t i = 0; i < c->num_columns; i++) {
    g_free(c->columns[i].name);
    g_free(c->columns[i].type_name);
    g_free(c->columns[i].default_val);
    g_free(c->columns[i].foreign_key);
  }
  g_free(c->columns);
  g_free(c->name);
  g_free(c);
}

static gboolean col_required(const LaceColumn *c) {
  return !c->auto_increment && !c->generated && !c->nullable &&
         !(c->default_val && c->default_val[0]);
}

static void on_toggle(GtkToggleButton *b, ColumnEntry *e) {
  gboolean active = gtk_toggle_button_get_active(b);
  GtkWidget *other = GTK_WIDGET(b) == e->null_toggle ? e->empty_toggle : e->null_toggle;
  if (active) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(e->row), "");
  }
  gtk_editable_set_editable(GTK_EDITABLE(e->row), !active);
}

static void on_text_changed(GtkEditable *ed, ColumnEntry *e) {
  const char *t = gtk_editable_get_text(ed);
  if (t && *t) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->null_toggle), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->empty_toggle), FALSE);
  }
}

static void add_row_free(AddRow *a) {
  g_clear_object(&a->conn);
  g_free(a->table);
  schema_copy_free(a->schema);
  g_free(a->entries);
  g_free(a);
}

static void on_closed(AdwDialog *d, AddRow *a) {
  (void)d;
  if (a->done)
    a->done(a->inserted, a->user_data);
  add_row_free(a);
}

static void insert_done(LaceJob *job, gpointer data) {
  AddRow *a = data;
  gtk_widget_set_sensitive(a->insert_button, TRUE);
  if (job->error) {
    adw_banner_set_title(ADW_BANNER(a->banner), lace_job_error(job));
    adw_banner_set_revealed(ADW_BANNER(a->banner), TRUE);
    return;
  }
  a->inserted = TRUE;
  adw_dialog_close(a->dialog);
}

static void on_insert(GtkButton *b, AddRow *a) {
  (void)b;
  const char **columns = g_new0(const char *, a->n + 1);
  LaceValue *values = g_new0(LaceValue, a->n + 1);
  size_t n = 0;
  for (size_t i = 0; i < a->n; i++) {
    ColumnEntry *e = &a->entries[i];
    const LaceColumn *c = &a->schema->columns[i];
    if (e->omitted)
      continue;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(e->row));
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->null_toggle))) {
      columns[n] = c->name;
      values[n++] = lace_value_null();
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->empty_toggle))) {
      columns[n] = c->name;
      values[n++] = lace_value_text("");
    } else if (text && *text) {
      char *err = NULL;
      LaceValue v;
      if (!lace_value_from_edit_text(text, c->type, &v, &err)) {
        char *msg = g_strdup_printf("%s: %s", c->name, err ? err : "invalid value");
        adw_banner_set_title(ADW_BANNER(a->banner), msg);
        adw_banner_set_revealed(ADW_BANNER(a->banner), TRUE);
        g_free(msg);
        free(err);
        for (size_t k = 0; k < n; k++)
          lace_value_free(&values[k]);
        g_free(columns);
        g_free(values);
        return;
      }
      columns[n] = c->name;
      values[n++] = v;
    }
  }
  adw_banner_set_revealed(ADW_BANNER(a->banner), FALSE);
  gtk_widget_set_sensitive(a->insert_button, FALSE);
  LaceJob *job = lace_job_insert(a->table, columns, values, n);
  for (size_t k = 0; k < n; k++)
    lace_value_free(&values[k]);
  g_free(columns);
  g_free(values);
  lace_connection_submit(a->conn, job, G_OBJECT(a->dialog), insert_done, a);
}

static void on_cancel(GtkButton *b, AddRow *a) {
  (void)b;
  adw_dialog_close(a->dialog);
}

void lace_add_row_dialog_show(GtkWidget *parent, LaceConnection *conn,
                              const char *table, const LaceSchema *schema,
                              LaceAddRowDone done, gpointer user_data) {
  AddRow *a = g_new0(AddRow, 1);
  a->conn = g_object_ref(conn);
  a->table = g_strdup(table);
  a->schema = schema_copy(schema);
  a->n = schema->num_columns;
  a->entries = g_new0(ColumnEntry, a->n ? a->n : 1);
  a->done = done;
  a->user_data = user_data;

  a->dialog = adw_dialog_new();
  char *title = g_strdup_printf("Add row to %s", table);
  adw_dialog_set_title(a->dialog, title);
  g_free(title);
  adw_dialog_set_content_width(a->dialog, 560);
  adw_dialog_set_content_height(a->dialog, 620);

  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), a);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  a->insert_button = gtk_button_new_with_label("Insert");
  gtk_widget_add_css_class(a->insert_button, "suggested-action");
  g_signal_connect(a->insert_button, "clicked", G_CALLBACK(on_insert), a);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), a->insert_button);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

  a->banner = adw_banner_new("");
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), a->banner);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 640);
  GtkWidget *group = adw_preferences_group_new();
  gtk_widget_set_margin_top(group, 12);
  gtk_widget_set_margin_bottom(group, 24);
  gtk_widget_set_margin_start(group, 12);
  gtk_widget_set_margin_end(group, 12);
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
      "Empty fields are left to the database. Use NULL or '' to write those explicitly.");

  for (size_t i = 0; i < a->n; i++) {
    const LaceColumn *c = &a->schema->columns[i];
    ColumnEntry *e = &a->entries[i];
    GString *t = g_string_new(c->name ? c->name : "");
    g_string_append_printf(t, "  ·  %s", c->type_name && *c->type_name ? c->type_name : lace_type_short(c->type));
    if (c->primary_key)
      g_string_append(t, "  ·  key");
    if (c->auto_increment || c->generated) {
      g_string_append(t, c->generated ? "  ·  generated" : "  ·  auto");
      e->omitted = TRUE;
    } else if (c->default_val && c->default_val[0]) {
      g_string_append_printf(t, "  ·  default %s", c->default_val);
    } else if (col_required(c)) {
      g_string_append(t, "  ·  required");
    }
    e->row = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(e->row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(e->row), t->str);
    g_string_free(t, TRUE);
    e->null_toggle = gtk_toggle_button_new_with_label("NULL");
    gtk_widget_set_tooltip_text(e->null_toggle, "Write NULL");
    gtk_widget_set_valign(e->null_toggle, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(e->null_toggle, "flat");
    e->empty_toggle = gtk_toggle_button_new_with_label("Empty");
    gtk_widget_set_tooltip_text(e->empty_toggle, "Write an empty string");
    gtk_widget_set_valign(e->empty_toggle, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(e->empty_toggle, "flat");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(e->row), e->null_toggle);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(e->row), e->empty_toggle);
    if (e->omitted) {
      gtk_widget_set_sensitive(e->row, FALSE);
    } else if (c->nullable && !(c->default_val && c->default_val[0])) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->null_toggle), TRUE);
      gtk_editable_set_editable(GTK_EDITABLE(e->row), FALSE);
    }
    g_signal_connect(e->null_toggle, "toggled", G_CALLBACK(on_toggle), e);
    g_signal_connect(e->empty_toggle, "toggled", G_CALLBACK(on_toggle), e);
    g_signal_connect(e->row, "changed", G_CALLBACK(on_text_changed), e);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), e->row);
  }
  adw_clamp_set_child(ADW_CLAMP(clamp), group);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), scrolled);
  adw_dialog_set_child(a->dialog, tv);
  adw_dialog_set_default_widget(a->dialog, a->insert_button);
  g_signal_connect(a->dialog, "closed", G_CALLBACK(on_closed), a);
  adw_dialog_present(a->dialog, parent);
  for (size_t i = 0; i < a->n; i++) {
    if (gtk_widget_get_sensitive(a->entries[i].row) &&
        gtk_editable_get_editable(GTK_EDITABLE(a->entries[i].row))) {
      gtk_widget_grab_focus(a->entries[i].row);
      break;
    }
  }
}
