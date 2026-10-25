/*
 * Lace GTK frontend
 * LaceCatalogList: the list behind a sub-view of a table tab (Structure,
 * Indexes, Foreign keys, Information, a Definition): a GtkColumnView over
 * the rows core/catalog_rows.c built, which it only draws. Return or a
 * double click on a row signals its tag.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_CATALOG_LIST_H
#define LACE_GTK_CATALOG_LIST_H

#include "core/catalog_rows.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define LACE_TYPE_CATALOG_LIST (lace_catalog_list_get_type())
G_DECLARE_FINAL_TYPE(LaceCatalogList, lace_catalog_list, LACE, CATALOG_LIST, GtkWidget)

LaceCatalogList *lace_catalog_list_new(gboolean monospace);
/* Show the rows (copied); NULL with a message shows only the message */
void lace_catalog_list_show(LaceCatalogList *l, const CatalogRows *rows, const char *message);
/* The rows' text, one line each (the script driver); caller g_strfreev's */
char **lace_catalog_list_get_texts(LaceCatalogList *l);
void lace_catalog_list_focus(LaceCatalogList *l);

/* Signal: activate-row (int tag) */

G_END_DECLS

#endif /* LACE_GTK_CATALOG_LIST_H */
