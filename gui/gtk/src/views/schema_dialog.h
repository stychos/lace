/*
 * Lace GTK frontend
 * Table structure dialog: columns, keys, defaults, indexes, foreign keys
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_SCHEMA_DIALOG_H
#define LACE_GTK_SCHEMA_DIALOG_H

#include <adwaita.h>
#include <lace.h>

G_BEGIN_DECLS

void lace_schema_dialog_show(GtkWidget *parent, const char *table,
                             const LaceSchema *schema);

G_END_DECLS

#endif /* LACE_GTK_SCHEMA_DIALOG_H */
