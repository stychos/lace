/*
 * Lace GTK frontend
 * Small UI helpers shared by the views
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_UTIL_H
#define LACE_GTK_UTIL_H

#include <adwaita.h>
#include <lace.h>

G_BEGIN_DECLS

/* A toast on the nearest AdwToastOverlay above widget (the window's) */
void lace_toast(GtkWidget *widget, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* An error dialog (AdwAlertDialog) over widget's window */
void lace_error_dialog(GtkWidget *widget, const char *heading, const char *body);

/* Ask before an action. cb(TRUE) when the user confirmed. */
typedef void (*LaceConfirmCb)(gboolean confirmed, gpointer user_data);
void lace_confirm(GtkWidget *widget, const char *heading, const char *body,
                  const char *action_label, gboolean destructive,
                  LaceConfirmCb cb, gpointer user_data);

/* One-line text prompt (names). cb(text or NULL when cancelled); text is
 * valid during the callback only. */
typedef void (*LacePromptCb)(const char *text, gpointer user_data);
void lace_prompt(GtkWidget *widget, const char *heading, const char *body,
                 const char *initial, const char *action_label, gboolean password,
                 LacePromptCb cb, gpointer user_data);

/* The script driver: answer the alert dialog opened last by lace_confirm
 * or lace_prompt ("go" or "cancel"; text fills a prompt's entry first);
 * false when none is open */
gboolean lace_debug_answer_alert(const char *response, const char *text);

/* Put text on the clipboard */
void lace_copy_text(GtkWidget *widget, const char *text);

/* "1,234,567" (allocated with g_malloc) */
char *lace_format_count(gsize n);

/* "12 ms", "1.3 s" */
char *lace_format_elapsed(double ms);

/* CSS class for a value's type: "num", "bool", "blob", "text" */
const char *lace_type_css_class(LaceValueType t);

/* Short type label for headers and status: "INT", "TEXT", "DECIMAL" */
const char *lace_type_short(LaceValueType t);

/* Return freed heap to the OS (after a page of rows was parsed and the
 * transient JSON tree freed, glibc keeps the arena otherwise) */
void lace_trim_heap(void);

/* The window that holds widget, or NULL */
GtkWindow *lace_widget_window(GtkWidget *widget);

/* A compact icon button with a tooltip and an action name */
GtkWidget *lace_icon_button(const char *icon, const char *tooltip,
                            const char *action_name);

G_END_DECLS

#endif /* LACE_GTK_UTIL_H */
