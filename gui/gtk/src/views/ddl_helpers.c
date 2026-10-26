/*
 * Lace GTK frontend
 * The structure editors' pure parts
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ddl_helpers.h"
#include <string.h>

char **lace_ddl_split_names(const char *text) {
  GPtrArray *out = g_ptr_array_new();
  if (text) {
    char **parts = g_strsplit(text, ",", -1);
    for (char **p = parts; *p; p++) {
      char *t = g_strstrip(g_strdup(*p));
      if (*t)
        g_ptr_array_add(out, t);
      else
        g_free(t);
    }
    g_strfreev(parts);
  }
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

char *lace_ddl_column_uses(const LaceSchema *schema, const char *column) {
  GString *s = g_string_new(NULL);
  for (size_t i = 0; schema && column && i < schema->num_indexes; i++) {
    const LaceIndex *ix = &schema->indexes[i];
    for (size_t k = 0; k < ix->num_columns; k++)
      if (ix->columns[k] && g_str_equal(ix->columns[k], column)) {
        g_string_append_printf(s, "%s%s %s", s->len ? ", " : "",
                               ix->kind == LACE_INDEX_PRIMARY ? "the primary key"
                               : ix->kind == LACE_INDEX_CHECK ? "check"
                                                              : "index",
                               ix->name ? ix->name : "");
        break;
      }
  }
  for (size_t i = 0; schema && column && i < schema->num_foreign_keys; i++) {
    const LaceForeignKey *fk = &schema->foreign_keys[i];
    for (size_t k = 0; k < fk->num_columns; k++)
      if (fk->columns[k] && g_str_equal(fk->columns[k], column)) {
        g_string_append_printf(s, "%sforeign key %s", s->len ? ", " : "", fk->name ? fk->name : "");
        break;
      }
  }
  return g_string_free(s, FALSE);
}

gboolean lace_ddl_index_covers(const LaceSchema *schema, char *const *columns, size_t n) {
  for (size_t i = 0; schema && n && i < schema->num_indexes; i++) {
    const LaceIndex *ix = &schema->indexes[i];
    if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION || ix->num_columns < n)
      continue;
    gboolean same = TRUE;
    for (size_t k = 0; k < n && same; k++)
      same = ix->columns[k] && columns[k] && g_str_equal(ix->columns[k], columns[k]);
    if (same)
      return TRUE;
  }
  return FALSE;
}

const char *lace_ddl_primary_key_name(const LaceSchema *schema) {
  for (size_t i = 0; schema && i < schema->num_indexes; i++) {
    const LaceIndex *ix = &schema->indexes[i];
    if (ix->kind == LACE_INDEX_PRIMARY)
      return ix->constraint && *ix->constraint ? ix->constraint : ix->name;
  }
  return NULL;
}

static char *commas(gint64 n) {
  char digits[32];
  g_snprintf(digits, sizeof digits, "%" G_GINT64_FORMAT, n);
  size_t len = strlen(digits);
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0 && digits[i - 1] != '-')
      g_string_append_c(s, ',');
    g_string_append_c(s, digits[i]);
  }
  return g_string_free(s, FALSE);
}

char *lace_ddl_rows_text(const char *table, gint64 total, gboolean known, gboolean approximate) {
  if (!known || total < 0)
    return g_strdup(table ? table : "");
  char *n = commas(total);
  char *out = g_strdup_printf("%s (%s%s %s)", table ? table : "", approximate ? "≈" : "", n,
                              total == 1 ? "row" : "rows");
  g_free(n);
  return out;
}

char *lace_ddl_banner_text(const char *what, gint64 elapsed_ms, gsize max_chars) {
  char *first = g_strdup(what ? what : "");
  char *nl = strpbrk(first, "\r\n");
  if (nl)
    *nl = '\0';
  for (char *c = first; *c; c++)
    if (*c == '\t')
      *c = ' ';
  if (max_chars && g_utf8_strlen(first, -1) > (glong)max_chars) {
    char *end = g_utf8_offset_to_pointer(first, (glong)max_chars - 1);
    *end = '\0';
    char *cut = g_strdup_printf("%s…", first);
    g_free(first);
    first = cut;
  }
  char *out = elapsed_ms >= 60000
                  ? g_strdup_printf("%s · %" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, first, elapsed_ms / 60000,
                                    elapsed_ms / 1000 % 60)
                  : g_strdup_printf("%s · %" G_GINT64_FORMAT ".%" G_GINT64_FORMAT " s", first, elapsed_ms / 1000,
                                    elapsed_ms / 100 % 10);
  g_free(first);
  return out;
}

char *lace_ddl_key_columns(const LaceSchema *schema) {
  GString *s = g_string_new(NULL);
  size_t n = schema ? schema->num_columns : 0;
  for (int pos = 1; pos <= (int)n; pos++)
    for (size_t i = 0; i < n; i++)
      if (schema->columns[i].key_position == pos)
        g_string_append_printf(s, "%s%s", s->len ? ", " : "", schema->columns[i].name ? schema->columns[i].name : "");
  return g_string_free(s, FALSE);
}
