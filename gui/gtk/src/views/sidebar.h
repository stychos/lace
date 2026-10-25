/*
 * Lace GTK frontend
 * LaceSidebar: a connection's tables, then its views, functions and
 * triggers in sections that fold, searchable
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_SIDEBAR_H
#define LACE_GTK_SIDEBAR_H

#include "conn/connection.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_SIDEBAR (lace_sidebar_get_type())
G_DECLARE_FINAL_TYPE(LaceSidebar, lace_sidebar, LACE, SIDEBAR, GtkWidget)

/* An entry of the list: a table, or an object as lace_list_objects names
 * it (a trigger's table, a routine's kind) */
typedef struct {
  LaceObjectKind kind;
  char *name;
  char *table;
  char *detail;
} LaceSidebarEntry;

LaceSidebar *lace_sidebar_new(LaceConnection *conn);
void lace_sidebar_focus_search(LaceSidebar *s);
void lace_sidebar_focus_list(LaceSidebar *s);
/* Fold or unfold the section of that kind (VIEWS, FUNCTIONS, TRIGGERS) */
void lace_sidebar_toggle_section(LaceSidebar *s, LaceObjectKind kind);
/* The rows' text, top to bottom ("▾ VIEWS", a name); caller g_strfreev's */
char **lace_sidebar_get_texts(LaceSidebar *s);
/* Open the listed entry (scripts); false when it is not listed */
gboolean lace_sidebar_open(LaceSidebar *s, LaceObjectKind kind, const char *name);

/* Signals (the entry is a const LaceSidebarEntry *, valid during the emission):
 *   preview-object (gpointer entry)   a single click: show in the preview tab
 *   open-object (gpointer entry)      Enter or a double click: keep it
 *   show-structure (gpointer entry)
 *   new-query ()
 */

G_END_DECLS

#endif /* LACE_GTK_SIDEBAR_H */
