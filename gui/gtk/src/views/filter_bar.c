/*
 * Lace GTK frontend
 * LaceFilterBar
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "filter_bar.h"
#include <stdlib.h>
#include <string.h>

struct _LaceFilterBar {
  GtkWidget parent_instance;
  GtkWidget *box;      /* AdwWrapBox */
  GtkWidget *search;
  GtkWidget *add_button;
  GtkWidget *clear_button;
  GPtrArray *chips;    /* GtkWidget*, parallel to filters */
  GArray *filters;     /* Filter */
  const LaceSchema *schema;
  char *search_text;

  /* Editor popover */
  GtkWidget *popover;
  GtkWidget *column_drop;
  GtkWidget *op_drop;
  GtkWidget *value_entry;
  GtkWidget *value2_entry;
  GtkWidget *raw_check;
  GtkWidget *raw_entry;
  GtkWidget *remove_button;
  GtkWidget *apply_button;
  GtkWidget *grid_fields;
  int editing; /* Index being edited, -1 for new */
  guint search_timeout;
};

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceFilterBar, lace_filter_bar, GTK_TYPE_WIDGET)

static void rebuild_chips(LaceFilterBar *bar);
static void open_editor(LaceFilterBar *bar, int index, GtkWidget *anchor);

static void emit_changed(LaceFilterBar *bar) {
  gtk_widget_set_visible(bar->clear_button, !lace_filter_bar_is_empty(bar));
  g_signal_emit(bar, signals[SIG_CHANGED], 0);
}

static char *chip_text(const Filter *f) {
  if (!f->column || f->op == FILTER_RAW)
    return g_strdup_printf("SQL: %s", f->value ? f->value : "");
  if (!filter_op_needs_value(f->op))
    return g_strdup_printf("%s %s", f->column, filter_op_label(f->op));
  if (filter_op_two_values(f->op))
    return g_strdup_printf("%s %s %s and %s", f->column, filter_op_label(f->op),
                           f->value ? f->value : "", f->value2 ? f->value2 : "");
  return g_strdup_printf("%s %s %s", f->column, filter_op_label(f->op), f->value ? f->value : "");
}

static void on_chip_clicked(GtkButton *b, LaceFilterBar *bar) {
  int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index"));
  open_editor(bar, index, GTK_WIDGET(b));
}

static void on_chip_remove(GtkButton *b, LaceFilterBar *bar) {
  int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index"));
  if (index < 0 || (guint)index >= bar->filters->len)
    return;
  filter_clear(&g_array_index(bar->filters, Filter, index));
  g_array_remove_index(bar->filters, index);
  rebuild_chips(bar);
  emit_changed(bar);
}

static void rebuild_chips(LaceFilterBar *bar) {
  for (guint i = 0; i < bar->chips->len; i++)
    adw_wrap_box_remove(ADW_WRAP_BOX(bar->box), g_ptr_array_index(bar->chips, i));
  g_ptr_array_set_size(bar->chips, 0);
  for (guint i = 0; i < bar->filters->len; i++) {
    const Filter *f = &g_array_index(bar->filters, Filter, i);
    GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(chip, "chip");
    gtk_widget_add_css_class(chip, "linked");
    char *text = chip_text(f);
    GtkWidget *label = gtk_button_new_with_label(text);
    g_free(text);
    gtk_widget_set_tooltip_text(label, "Edit filter");
    g_object_set_data(G_OBJECT(label), "index", GINT_TO_POINTER((int)i));
    g_signal_connect(label, "clicked", G_CALLBACK(on_chip_clicked), bar);
    GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(x, "Remove filter");
    g_object_set_data(G_OBJECT(x), "index", GINT_TO_POINTER((int)i));
    g_signal_connect(x, "clicked", G_CALLBACK(on_chip_remove), bar);
    gtk_box_append(GTK_BOX(chip), label);
    gtk_box_append(GTK_BOX(chip), x);
    adw_wrap_box_insert_child_after(ADW_WRAP_BOX(bar->box), chip,
                                    bar->chips->len ? g_ptr_array_index(bar->chips, bar->chips->len - 1)
                                                    : bar->search);
    g_ptr_array_add(bar->chips, chip);
  }
}

/* ==========================================================================
 * Editor popover
 * ========================================================================== */

static GtkStringList *column_names(LaceFilterBar *bar) {
  GtkStringList *list = gtk_string_list_new(NULL);
  for (size_t i = 0; bar->schema && i < bar->schema->num_columns; i++)
    gtk_string_list_append(list, bar->schema->columns[i].name ? bar->schema->columns[i].name : "");
  return list;
}

static void sync_editor_fields(LaceFilterBar *bar) {
  gboolean raw = gtk_check_button_get_active(GTK_CHECK_BUTTON(bar->raw_check));
  gtk_widget_set_visible(bar->grid_fields, !raw);
  gtk_widget_set_visible(bar->raw_entry, raw);
  guint op = gtk_drop_down_get_selected(GTK_DROP_DOWN(bar->op_drop));
  gtk_widget_set_visible(bar->value_entry, filter_op_needs_value((FilterOp)op));
  gtk_widget_set_visible(bar->value2_entry, filter_op_two_values((FilterOp)op));
}

static void on_editor_field_changed(GObject *o, GParamSpec *p, LaceFilterBar *bar) {
  (void)o;
  (void)p;
  sync_editor_fields(bar);
}

static void on_raw_toggled(GtkCheckButton *c, LaceFilterBar *bar) {
  (void)c;
  sync_editor_fields(bar);
}

static void on_apply(GtkButton *b, LaceFilterBar *bar) {
  (void)b;
  Filter f = {0};
  gboolean raw = gtk_check_button_get_active(GTK_CHECK_BUTTON(bar->raw_check));
  if (raw) {
    const char *t = gtk_editable_get_text(GTK_EDITABLE(bar->raw_entry));
    if (!t || !*t)
      return;
    f.op = FILTER_RAW;
    f.value = g_strdup(t);
  } else {
    guint ci = gtk_drop_down_get_selected(GTK_DROP_DOWN(bar->column_drop));
    if (!bar->schema || ci >= bar->schema->num_columns)
      return;
    f.column = g_strdup(bar->schema->columns[ci].name);
    f.op = (FilterOp)gtk_drop_down_get_selected(GTK_DROP_DOWN(bar->op_drop));
    f.value = g_strdup(gtk_editable_get_text(GTK_EDITABLE(bar->value_entry)));
    f.value2 = g_strdup(gtk_editable_get_text(GTK_EDITABLE(bar->value2_entry)));
    if (filter_op_needs_value(f.op) && !*f.value) {
      filter_clear(&f);
      return;
    }
  }
  if (bar->editing >= 0 && (guint)bar->editing < bar->filters->len) {
    filter_clear(&g_array_index(bar->filters, Filter, bar->editing));
    g_array_index(bar->filters, Filter, bar->editing) = f;
  } else {
    g_array_append_val(bar->filters, f);
  }
  gtk_popover_popdown(GTK_POPOVER(bar->popover));
  rebuild_chips(bar);
  emit_changed(bar);
}

static void on_remove(GtkButton *b, LaceFilterBar *bar) {
  (void)b;
  if (bar->editing >= 0 && (guint)bar->editing < bar->filters->len) {
    filter_clear(&g_array_index(bar->filters, Filter, bar->editing));
    g_array_remove_index(bar->filters, bar->editing);
  }
  gtk_popover_popdown(GTK_POPOVER(bar->popover));
  rebuild_chips(bar);
  emit_changed(bar);
}

static void on_entry_activate(GtkEntry *e, LaceFilterBar *bar) {
  (void)e;
  on_apply(NULL, bar);
}

static gboolean on_popover_key(GtkEventControllerKey *c, guint keyval, guint code,
                               GdkModifierType state, LaceFilterBar *bar) {
  (void)c; (void)code; (void)state;
  if (keyval == GDK_KEY_Escape) {
    gtk_popover_popdown(GTK_POPOVER(bar->popover));
    return TRUE;
  }
  return FALSE;
}

static void build_popover(LaceFilterBar *bar) {
  bar->popover = gtk_popover_new();
  gtk_widget_set_parent(bar->popover, GTK_WIDGET(bar));
  gtk_popover_set_position(GTK_POPOVER(bar->popover), GTK_POS_BOTTOM);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request(box, 320, -1);

  bar->grid_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  bar->column_drop = gtk_drop_down_new(NULL, NULL);
  gtk_drop_down_set_enable_search(GTK_DROP_DOWN(bar->column_drop), TRUE);
  GtkExpression *expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
  gtk_drop_down_set_expression(GTK_DROP_DOWN(bar->column_drop), expr);
  gtk_expression_unref(expr);
  const char *ops[FILTER_OP_COUNT + 1];
  int n = 0;
  for (int i = 0; i < FILTER_OP_COUNT; i++) {
    if (i == FILTER_RAW)
      continue;
    ops[n++] = filter_op_label((FilterOp)i);
  }
  ops[n] = NULL;
  bar->op_drop = gtk_drop_down_new_from_strings(ops);
  bar->value_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(bar->value_entry), "Value");
  bar->value2_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(bar->value2_entry), "and value");
  gtk_box_append(GTK_BOX(bar->grid_fields), bar->column_drop);
  gtk_box_append(GTK_BOX(bar->grid_fields), bar->op_drop);
  gtk_box_append(GTK_BOX(bar->grid_fields), bar->value_entry);
  gtk_box_append(GTK_BOX(bar->grid_fields), bar->value2_entry);
  gtk_box_append(GTK_BOX(box), bar->grid_fields);

  bar->raw_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(bar->raw_entry), "e.g. price > 10 AND stock = 0");
  gtk_widget_add_css_class(bar->raw_entry, "monospace");
  gtk_box_append(GTK_BOX(box), bar->raw_entry);

  bar->raw_check = gtk_check_button_new_with_label("Raw SQL condition");
  gtk_box_append(GTK_BOX(box), bar->raw_check);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  bar->remove_button = gtk_button_new_with_label("Remove");
  gtk_widget_add_css_class(bar->remove_button, "destructive-action");
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  bar->apply_button = gtk_button_new_with_label("Apply");
  gtk_widget_add_css_class(bar->apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(buttons), bar->remove_button);
  gtk_box_append(GTK_BOX(buttons), spacer);
  gtk_box_append(GTK_BOX(buttons), bar->apply_button);
  gtk_box_append(GTK_BOX(box), buttons);
  gtk_popover_set_child(GTK_POPOVER(bar->popover), box);

  g_signal_connect(bar->op_drop, "notify::selected", G_CALLBACK(on_editor_field_changed), bar);
  g_signal_connect(bar->raw_check, "toggled", G_CALLBACK(on_raw_toggled), bar);
  g_signal_connect(bar->apply_button, "clicked", G_CALLBACK(on_apply), bar);
  g_signal_connect(bar->remove_button, "clicked", G_CALLBACK(on_remove), bar);
  g_signal_connect(bar->value_entry, "activate", G_CALLBACK(on_entry_activate), bar);
  g_signal_connect(bar->value2_entry, "activate", G_CALLBACK(on_entry_activate), bar);
  g_signal_connect(bar->raw_entry, "activate", G_CALLBACK(on_entry_activate), bar);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_popover_key), bar);
  gtk_widget_add_controller(bar->popover, keys);
}

static guint op_to_index(FilterOp op) { return op > FILTER_RAW ? 0 : (guint)op; }

static void open_editor(LaceFilterBar *bar, int index, GtkWidget *anchor) {
  bar->editing = index;
  GtkStringList *names = column_names(bar);
  gtk_drop_down_set_model(GTK_DROP_DOWN(bar->column_drop), G_LIST_MODEL(names));
  g_object_unref(names);
  const Filter *f = index >= 0 && (guint)index < bar->filters->len
                        ? &g_array_index(bar->filters, Filter, index) : NULL;
  gboolean raw = f && (f->op == FILTER_RAW || !f->column);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(bar->raw_check), raw);
  gtk_editable_set_text(GTK_EDITABLE(bar->raw_entry), raw && f->value ? f->value : "");
  gtk_editable_set_text(GTK_EDITABLE(bar->value_entry), f && !raw && f->value ? f->value : "");
  gtk_editable_set_text(GTK_EDITABLE(bar->value2_entry), f && !raw && f->value2 ? f->value2 : "");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(bar->op_drop), f && !raw ? op_to_index(f->op) : 0);
  guint ci = 0;
  if (f && f->column && bar->schema) {
    for (size_t i = 0; i < bar->schema->num_columns; i++)
      if (bar->schema->columns[i].name && strcmp(bar->schema->columns[i].name, f->column) == 0)
        ci = (guint)i;
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(bar->column_drop), ci);
  gtk_widget_set_visible(bar->remove_button, f != NULL);
  sync_editor_fields(bar);

  graphene_rect_t b;
  GdkRectangle rect = {0, 0, 1, 1};
  if (anchor && gtk_widget_compute_bounds(anchor, GTK_WIDGET(bar), &b)) {
    rect.x = (int)b.origin.x;
    rect.y = (int)b.origin.y;
    rect.width = (int)b.size.width;
    rect.height = (int)b.size.height;
  }
  gtk_popover_set_pointing_to(GTK_POPOVER(bar->popover), &rect);
  gtk_popover_popup(GTK_POPOVER(bar->popover));
  gtk_widget_grab_focus(raw ? bar->raw_entry : bar->value_entry);
}

/* ==========================================================================
 * Search
 * ========================================================================== */

static gboolean search_apply(gpointer data) {
  LaceFilterBar *bar = data;
  bar->search_timeout = 0;
  const char *t = gtk_editable_get_text(GTK_EDITABLE(bar->search));
  if (g_strcmp0(t, bar->search_text) != 0) {
    g_free(bar->search_text);
    bar->search_text = g_strdup(t);
    emit_changed(bar);
  }
  return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *e, LaceFilterBar *bar) {
  (void)e;
  if (bar->search_timeout)
    g_source_remove(bar->search_timeout);
  bar->search_timeout = g_timeout_add(350, search_apply, bar);
}

static void on_search_activate(GtkSearchEntry *e, LaceFilterBar *bar) {
  (void)e;
  if (bar->search_timeout) {
    g_source_remove(bar->search_timeout);
    bar->search_timeout = 0;
  }
  search_apply(bar);
}

static void on_add_clicked(GtkButton *b, LaceFilterBar *bar) {
  open_editor(bar, -1, GTK_WIDGET(b));
}

static void on_clear_clicked(GtkButton *b, LaceFilterBar *bar) {
  (void)b;
  lace_filter_bar_clear(bar);
  emit_changed(bar);
}

/* ==========================================================================
 * Public
 * ========================================================================== */

void lace_filter_bar_set_schema(LaceFilterBar *bar, const LaceSchema *schema) {
  bar->schema = schema;
  gtk_widget_set_sensitive(bar->add_button, schema != NULL);
}

const Filter *lace_filter_bar_get_filters(LaceFilterBar *bar, gsize *n) {
  *n = bar->filters->len;
  return bar->filters->len ? &g_array_index(bar->filters, Filter, 0) : NULL;
}

const char *lace_filter_bar_get_search(LaceFilterBar *bar) {
  return bar->search_text ? bar->search_text : "";
}

gboolean lace_filter_bar_is_empty(LaceFilterBar *bar) {
  return bar->filters->len == 0 && (!bar->search_text || !*bar->search_text);
}

void lace_filter_bar_set_filters(LaceFilterBar *bar, const Filter *filters,
                                 gsize n, const char *search) {
  for (guint i = 0; i < bar->filters->len; i++)
    filter_clear(&g_array_index(bar->filters, Filter, i));
  g_array_set_size(bar->filters, 0);
  for (gsize i = 0; i < n; i++) {
    Filter f = {0};
    f.column = filters[i].column ? g_strdup(filters[i].column) : NULL;
    f.op = filters[i].op;
    f.value = filters[i].value ? g_strdup(filters[i].value) : NULL;
    f.value2 = filters[i].value2 ? g_strdup(filters[i].value2) : NULL;
    g_array_append_val(bar->filters, f);
  }
  g_free(bar->search_text);
  bar->search_text = g_strdup(search ? search : "");
  g_signal_handlers_block_by_func(bar->search, on_search_changed, bar);
  gtk_editable_set_text(GTK_EDITABLE(bar->search), bar->search_text);
  g_signal_handlers_unblock_by_func(bar->search, on_search_changed, bar);
  rebuild_chips(bar);
  gtk_widget_set_visible(bar->clear_button, !lace_filter_bar_is_empty(bar));
}

void lace_filter_bar_clear(LaceFilterBar *bar) {
  lace_filter_bar_set_filters(bar, NULL, 0, "");
}

void lace_filter_bar_add(LaceFilterBar *bar, const char *column, FilterOp op,
                         const char *value) {
  Filter f = {0};
  f.column = g_strdup(column);
  f.op = value ? op : FILTER_NULL;
  f.value = g_strdup(value ? value : "");
  g_array_append_val(bar->filters, f);
  rebuild_chips(bar);
  emit_changed(bar);
}

void lace_filter_bar_open_add(LaceFilterBar *bar, const char *column) {
  open_editor(bar, -1, bar->add_button);
  if (column && bar->schema) {
    for (size_t i = 0; i < bar->schema->num_columns; i++)
      if (bar->schema->columns[i].name && strcmp(bar->schema->columns[i].name, column) == 0)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(bar->column_drop), (guint)i);
  }
}

void lace_filter_bar_focus_search(LaceFilterBar *bar) {
  gtk_widget_grab_focus(bar->search);
}

static void lace_filter_bar_dispose(GObject *obj) {
  LaceFilterBar *bar = LACE_FILTER_BAR(obj);
  if (bar->search_timeout) {
    g_source_remove(bar->search_timeout);
    bar->search_timeout = 0;
  }
  g_clear_pointer(&bar->popover, gtk_widget_unparent);
  g_clear_pointer(&bar->box, gtk_widget_unparent);
  if (bar->filters) {
    for (guint i = 0; i < bar->filters->len; i++)
      filter_clear(&g_array_index(bar->filters, Filter, i));
    g_clear_pointer(&bar->filters, g_array_unref);
  }
  g_clear_pointer(&bar->chips, g_ptr_array_unref);
  g_clear_pointer(&bar->search_text, g_free);
  G_OBJECT_CLASS(lace_filter_bar_parent_class)->dispose(obj);
}

static void lace_filter_bar_class_init(LaceFilterBarClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_filter_bar_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass), GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(klass), "lace-filterbar");
  signals[SIG_CHANGED] = g_signal_new("changed", LACE_TYPE_FILTER_BAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_filter_bar_init(LaceFilterBar *bar) {
  bar->filters = g_array_new(FALSE, TRUE, sizeof(Filter));
  bar->chips = g_ptr_array_new();
  bar->editing = -1;

  bar->box = adw_wrap_box_new();
  adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(bar->box), 6);
  adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(bar->box), 6);
  adw_wrap_box_set_align(ADW_WRAP_BOX(bar->box), 0);
  gtk_widget_add_css_class(bar->box, "filter-bar");
  gtk_widget_set_parent(bar->box, GTK_WIDGET(bar));

  bar->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(bar->search), "Search text columns");
  gtk_widget_set_size_request(bar->search, 220, -1);
  g_signal_connect(bar->search, "search-changed", G_CALLBACK(on_search_changed), bar);
  g_signal_connect(bar->search, "activate", G_CALLBACK(on_search_activate), bar);
  adw_wrap_box_append(ADW_WRAP_BOX(bar->box), bar->search);

  bar->add_button = gtk_button_new();
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "list-add-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "Filter");
  gtk_button_set_child(GTK_BUTTON(bar->add_button), content);
  gtk_widget_set_tooltip_text(bar->add_button, "Add a column filter");
  gtk_widget_add_css_class(bar->add_button, "flat");
  g_signal_connect(bar->add_button, "clicked", G_CALLBACK(on_add_clicked), bar);
  adw_wrap_box_append(ADW_WRAP_BOX(bar->box), bar->add_button);

  bar->clear_button = gtk_button_new_with_label("Clear all");
  gtk_widget_add_css_class(bar->clear_button, "flat");
  gtk_widget_set_visible(bar->clear_button, FALSE);
  g_signal_connect(bar->clear_button, "clicked", G_CALLBACK(on_clear_clicked), bar);
  adw_wrap_box_append(ADW_WRAP_BOX(bar->box), bar->clear_button);

  build_popover(bar);
}

LaceFilterBar *lace_filter_bar_new(void) {
  return g_object_new(LACE_TYPE_FILTER_BAR, NULL);
}
