/*
 * Lace GTK frontend
 * The structure editors' pure parts (GLib only, unit-tested): name lists,
 * what a column is used by, whether an index covers columns, the texts of
 * the confirmations and the lock banner.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_DDL_HELPERS_H
#define LACE_GTK_DDL_HELPERS_H

#include <glib.h>
#include <lace.h>

G_BEGIN_DECLS

/* "a, b ,,c" -> {"a", "b", "c"} (g_strfreev); NULL text gives an empty vector */
char **lace_ddl_split_names(const char *text);
/* "index a, foreign key b": what the schema says uses the column; "" when nothing */
char *lace_ddl_column_uses(const LaceSchema *schema, const char *column);
/* An index of the schema starts with these columns, in this order */
gboolean lace_ddl_index_covers(const LaceSchema *schema, char *const *columns, size_t n);
/* The primary key's constraint name (PostgreSQL) or index name ("PRIMARY" on MySQL), or NULL */
const char *lace_ddl_primary_key_name(const LaceSchema *schema);
/* "orders (12,400 rows)", "orders (≈1,200 rows)", "orders" when the total is unknown */
char *lace_ddl_rows_text(const char *table, gint64 total, gboolean known, gboolean approximate);
/* The lock banner's text: the statement's first line (cut to max_chars
 * characters), the elapsed time: "SELECT … · 12.3 s" */
char *lace_ddl_banner_text(const char *what, gint64 elapsed_ms, gsize max_chars);
/* The current key's columns in key order, joined with ", " */
char *lace_ddl_key_columns(const LaceSchema *schema);

G_END_DECLS

#endif /* LACE_GTK_DDL_HELPERS_H */
