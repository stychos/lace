/*
 * Lace GTK frontend
 * Small UI helpers shared by the views
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "util.h"
#include <stdarg.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

void lace_trim_heap(void) {
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}

/* The first AdwToastOverlay in the tree under w (depth-first, shallow) */
static GtkWidget *find_overlay(GtkWidget *w, int depth) {
  if (!w || depth > 6)
    return NULL;
  if (ADW_IS_TOAST_OVERLAY(w))
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_overlay(c, depth + 1);
    if (found)
      return found;
  }
  return NULL;
}

void lace_toast(GtkWidget *widget, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  GtkWidget *overlay = widget ? gtk_widget_get_ancestor(widget, ADW_TYPE_TOAST_OVERLAY) : NULL;
  if (!overlay && widget) {
    /* Dialogs sit beside the window's content: look under the root */
    GtkRoot *root = gtk_widget_get_root(widget);
    overlay = root ? find_overlay(GTK_WIDGET(root), 0) : NULL;
  }
  if (overlay) {
    AdwToast *toast = adw_toast_new(msg);
    adw_toast_set_timeout(toast, 4);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), toast);
  } else {
    g_message("%s", msg);
  }
  g_free(msg);
}

GtkWindow *lace_widget_window(GtkWidget *widget) {
  GtkRoot *root = widget ? gtk_widget_get_root(widget) : NULL;
  return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

void lace_error_dialog(GtkWidget *widget, const char *heading, const char *body) {
  AdwDialog *d = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", "_OK");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "ok");
  adw_dialog_present(d, widget);
}

typedef struct {
  LaceConfirmCb cb;
  gpointer user_data;
} ConfirmData;

static void confirm_done(GObject *src, GAsyncResult *res, gpointer data) {
  ConfirmData *cd = data;
  const char *r = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
  cd->cb(g_strcmp0(r, "go") == 0, cd->user_data);
  g_free(cd);
}

/* The alert opened last (a weak pointer) and its entry, for lace_debug_answer_alert */
static AdwDialog *g_last_alert;
static GtkWidget *g_last_alert_entry;

static void note_alert(AdwDialog *d, GtkWidget *entry) {
  if (g_last_alert)
    g_object_remove_weak_pointer(G_OBJECT(g_last_alert), (gpointer *)&g_last_alert);
  g_last_alert = d;
  g_last_alert_entry = entry;
  g_object_add_weak_pointer(G_OBJECT(d), (gpointer *)&g_last_alert);
}

gboolean lace_debug_answer_alert(const char *response, const char *text) {
  if (!g_last_alert)
    return FALSE;
  AdwDialog *d = g_last_alert;
  if (text && g_last_alert_entry)
    gtk_editable_set_text(GTK_EDITABLE(g_last_alert_entry), text);
  g_signal_emit_by_name(d, "response", response ? response : "cancel");
  adw_dialog_close(d);
  return TRUE;
}

void lace_confirm(GtkWidget *widget, const char *heading, const char *body,
                  const char *action_label, gboolean destructive,
                  LaceConfirmCb cb, gpointer user_data) {
  AdwDialog *d = adw_alert_dialog_new(heading, body);
  note_alert(d, NULL);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "_Cancel", "go",
                                 action_label, NULL);
  adw_alert_dialog_set_response_appearance(
      ADW_ALERT_DIALOG(d), "go",
      destructive ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), destructive ? "cancel" : "go");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
  ConfirmData *cd = g_new0(ConfirmData, 1);
  cd->cb = cb;
  cd->user_data = user_data;
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(d), widget, NULL, confirm_done, cd);
}

typedef struct {
  LacePromptCb cb;
  gpointer user_data;
  GtkWidget *entry;
} PromptData;

static void prompt_done(GObject *src, GAsyncResult *res, gpointer data) {
  PromptData *pd = data;
  const char *r = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
  if (g_strcmp0(r, "go") == 0) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(pd->entry));
    pd->cb(text ? text : "", pd->user_data);
  } else {
    pd->cb(NULL, pd->user_data);
  }
  g_free(pd);
}

static void prompt_activate(GtkEntry *entry, gpointer dialog) {
  (void)entry;
  /* Enter in the entry is the default response */
  g_signal_emit_by_name(dialog, "response", "go");
  adw_dialog_close(ADW_DIALOG(dialog));
}

void lace_prompt(GtkWidget *widget, const char *heading, const char *body,
                 const char *initial, const char *action_label, gboolean password,
                 LacePromptCb cb, gpointer user_data) {
  AdwDialog *d = adw_alert_dialog_new(heading, body);
  GtkWidget *entry = password ? gtk_password_entry_new() : gtk_entry_new();
  note_alert(d, entry);
  if (password) {
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    g_object_set(entry, "activates-default", TRUE, NULL);
  } else {
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  }
  if (initial)
    gtk_editable_set_text(GTK_EDITABLE(entry), initial);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), entry);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "_Cancel", "go",
                                 action_label, NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "go",
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "go");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
  PromptData *pd = g_new0(PromptData, 1);
  pd->cb = cb;
  pd->user_data = user_data;
  pd->entry = entry;
  g_signal_connect(entry, "activate", G_CALLBACK(prompt_activate), d);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(d), widget, NULL, prompt_done, pd);
  gtk_widget_grab_focus(entry);
}

void lace_copy_text(GtkWidget *widget, const char *text) {
  GdkClipboard *cb = gtk_widget_get_clipboard(widget);
  gdk_clipboard_set_text(cb, text ? text : "");
}

char *lace_format_count(gsize n) {
  char digits[32];
  g_snprintf(digits, sizeof(digits), "%" G_GSIZE_FORMAT, n);
  size_t len = strlen(digits);
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0)
      g_string_append_c(s, ',');
    g_string_append_c(s, digits[i]);
  }
  return g_string_free(s, FALSE);
}

char *lace_format_elapsed(double ms) {
  if (ms < 1000.0)
    return g_strdup_printf("%.0f ms", ms);
  if (ms < 60000.0)
    return g_strdup_printf("%.1f s", ms / 1000.0);
  return g_strdup_printf("%.1f min", ms / 60000.0);
}

const char *lace_type_css_class(LaceValueType t) {
  switch (t) {
  case LACE_TYPE_INT:
  case LACE_TYPE_FLOAT:
  case LACE_TYPE_DECIMAL:
    return "num";
  case LACE_TYPE_BOOL:
    return "bool";
  case LACE_TYPE_BLOB:
    return "blob";
  case LACE_TYPE_DATE:
  case LACE_TYPE_TIMESTAMP:
    return "date";
  default:
    return "text";
  }
}

const char *lace_type_short(LaceValueType t) {
  switch (t) {
  case LACE_TYPE_NULL: return "NULL";
  case LACE_TYPE_INT: return "INT";
  case LACE_TYPE_FLOAT: return "FLOAT";
  case LACE_TYPE_TEXT: return "TEXT";
  case LACE_TYPE_BLOB: return "BLOB";
  case LACE_TYPE_BOOL: return "BOOL";
  case LACE_TYPE_DATE: return "DATE";
  case LACE_TYPE_TIMESTAMP: return "TIMESTAMP";
  case LACE_TYPE_DECIMAL: return "DECIMAL";
  }
  return "?";
}

GtkWidget *lace_icon_button(const char *icon, const char *tooltip,
                            const char *action_name) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(b, tooltip);
  gtk_widget_add_css_class(b, "flat");
  if (action_name)
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action_name);
  return b;
}
