/*
 * Lace GTK frontend
 * LaceCellEditor
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "cell_editor.h"
#include "util.h"
#include <stdlib.h>

struct _LaceCellEditor {
  GtkPopover parent_instance;
  GtkWidget *title;
  GtkWidget *subtitle;
  GtkWidget *text;
  GtkWidget *null_button;
  GtkWidget *empty_button;
  GtkWidget *save_button;
  GtkWidget *hint;
  gboolean editable;
  gboolean committed;
};

enum { SIG_COMMITTED, SIG_CANCELLED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceCellEditor, lace_cell_editor, GTK_TYPE_POPOVER)

static void commit(LaceCellEditor *ed, const char *text, gboolean set_null) {
  if (!ed->editable)
    return;
  ed->committed = TRUE;
  g_signal_emit(ed, signals[SIG_COMMITTED], 0, text, set_null);
  gtk_popover_popdown(GTK_POPOVER(ed));
}

static void on_save(GtkButton *b, LaceCellEditor *ed) {
  (void)b;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed->text));
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(buf, &s, &e);
  char *t = gtk_text_buffer_get_text(buf, &s, &e, TRUE);
  commit(ed, t, FALSE);
  g_free(t);
}

static void on_null(GtkButton *b, LaceCellEditor *ed) {
  (void)b;
  commit(ed, NULL, TRUE);
}

static void on_empty(GtkButton *b, LaceCellEditor *ed) {
  (void)b;
  commit(ed, "", FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, LaceCellEditor *ed) {
  (void)c;
  (void)keycode;
  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;
  if (keyval == GDK_KEY_Escape) {
    gtk_popover_popdown(GTK_POPOVER(ed));
    return TRUE;
  }
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !shift) {
    on_save(NULL, ed);
    return TRUE;
  }
  if (ctrl && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
    on_null(NULL, ed);
    return TRUE;
  }
  if (ctrl && (keyval == GDK_KEY_d || keyval == GDK_KEY_D)) {
    on_empty(NULL, ed);
    return TRUE;
  }
  return FALSE;
}

static void on_closed(GtkPopover *p, gpointer data) {
  (void)data;
  LaceCellEditor *ed = LACE_CELL_EDITOR(p);
  if (!ed->committed)
    g_signal_emit(ed, signals[SIG_CANCELLED], 0);
}

static void lace_cell_editor_class_init(LaceCellEditorClass *klass) {
  signals[SIG_COMMITTED] = g_signal_new("committed", LACE_TYPE_CELL_EDITOR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
  signals[SIG_CANCELLED] = g_signal_new("cancelled", LACE_TYPE_CELL_EDITOR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  (void)klass;
}

static void lace_cell_editor_init(LaceCellEditor *ed) {
  gtk_widget_add_css_class(GTK_WIDGET(ed), "cell-editor");
  gtk_popover_set_autohide(GTK_POPOVER(ed), TRUE);
  gtk_popover_set_has_arrow(GTK_POPOVER(ed), FALSE);
  gtk_popover_set_position(GTK_POPOVER(ed), GTK_POS_BOTTOM);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(box, 360, -1);

  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  ed->title = gtk_label_new("");
  gtk_widget_add_css_class(ed->title, "heading");
  gtk_label_set_xalign(GTK_LABEL(ed->title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(ed->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(ed->title, TRUE);
  ed->subtitle = gtk_label_new("");
  gtk_widget_add_css_class(ed->subtitle, "dim-label");
  gtk_widget_add_css_class(ed->subtitle, "caption");
  gtk_box_append(GTK_BOX(head), ed->title);
  gtk_box_append(GTK_BOX(head), ed->subtitle);
  gtk_box_append(GTK_BOX(box), head);

  GtkWidget *sw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 60);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 320);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
  gtk_widget_add_css_class(sw, "card");
  ed->text = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ed->text), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(ed->text), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(ed->text), 6);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(ed->text), 6);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ed->text), 8);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(ed->text), 8);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(ed->text), FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), ed->text);
  gtk_box_append(GTK_BOX(box), sw);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  ed->null_button = gtk_button_new_with_label("Set NULL");
  gtk_widget_set_tooltip_text(ed->null_button, "Write NULL (Ctrl+N)");
  ed->empty_button = gtk_button_new_with_label("Set empty");
  gtk_widget_set_tooltip_text(ed->empty_button, "Write an empty string (Ctrl+D)");
  ed->save_button = gtk_button_new_with_label("Save");
  gtk_widget_add_css_class(ed->save_button, "suggested-action");
  gtk_widget_set_tooltip_text(ed->save_button, "Enter");
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(actions), ed->null_button);
  gtk_box_append(GTK_BOX(actions), ed->empty_button);
  gtk_box_append(GTK_BOX(actions), spacer);
  gtk_box_append(GTK_BOX(actions), ed->save_button);
  gtk_box_append(GTK_BOX(box), actions);

  ed->hint = gtk_label_new("Enter saves · Shift+Enter new line · Esc cancels");
  gtk_widget_add_css_class(ed->hint, "dim-label");
  gtk_widget_add_css_class(ed->hint, "caption");
  gtk_label_set_xalign(GTK_LABEL(ed->hint), 0);
  gtk_box_append(GTK_BOX(box), ed->hint);

  gtk_popover_set_child(GTK_POPOVER(ed), box);

  g_signal_connect(ed->save_button, "clicked", G_CALLBACK(on_save), ed);
  g_signal_connect(ed->null_button, "clicked", G_CALLBACK(on_null), ed);
  g_signal_connect(ed->empty_button, "clicked", G_CALLBACK(on_empty), ed);
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), ed);
  gtk_widget_add_controller(GTK_WIDGET(ed), keys);
  g_signal_connect(ed, "closed", G_CALLBACK(on_closed), NULL);
}

LaceCellEditor *lace_cell_editor_new(void) {
  return g_object_new(LACE_TYPE_CELL_EDITOR, NULL);
}

void lace_cell_editor_open(LaceCellEditor *ed, const GdkRectangle *rect,
                           const LaceValue *value, const char *column_name,
                           LaceValueType type, const char *type_name,
                           gboolean editable) {
  ed->committed = FALSE;
  ed->editable = editable;
  gtk_label_set_text(GTK_LABEL(ed->title), column_name ? column_name : "");
  char *sub = g_strdup_printf("%s%s", type_name && *type_name ? type_name : lace_type_short(type),
                              editable ? "" : " · read only");
  gtk_label_set_text(GTK_LABEL(ed->subtitle), sub);
  g_free(sub);

  char *text = value ? lace_value_to_edit_text(value) : NULL;
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed->text));
  gtk_text_buffer_set_text(buf, text ? text : "", -1);
  free(text);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(ed->text), editable);
  gtk_widget_set_visible(ed->null_button, editable);
  gtk_widget_set_visible(ed->empty_button, editable);
  gtk_widget_set_visible(ed->save_button, editable);
  gtk_widget_set_visible(ed->hint, editable);
  gboolean is_null = !value || value->is_null || value->type == LACE_TYPE_NULL;
  gtk_widget_set_sensitive(ed->null_button, !is_null);

  gtk_popover_set_pointing_to(GTK_POPOVER(ed), rect);
  gtk_popover_popup(GTK_POPOVER(ed));
  gtk_widget_grab_focus(ed->text);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  gtk_text_buffer_place_cursor(buf, &end);
}

void lace_cell_editor_set_text(LaceCellEditor *ed, const char *text) {
  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed->text));
  gtk_text_buffer_set_text(buf, text ? text : "", -1);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  gtk_text_buffer_place_cursor(buf, &end);
}
