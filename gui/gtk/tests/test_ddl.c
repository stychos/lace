/*
 * Lace GTK frontend
 * Unit tests: the structure editors' pure helpers (GLib only, no display)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "test.h"
#include "views/ddl_helpers.h"
#include <string.h>

int main(void) {
  char **names = lace_ddl_split_names(" a, b ,,c ");
  CHECK(g_strv_length(names) == 3);
  CHECK(g_str_equal(names[0], "a") && g_str_equal(names[1], "b") && g_str_equal(names[2], "c"));
  g_strfreev(names);
  names = lace_ddl_split_names(NULL);
  CHECK(g_strv_length(names) == 0);
  g_strfreev(names);
  names = lace_ddl_split_names("solo");
  CHECK(g_strv_length(names) == 1 && g_str_equal(names[0], "solo"));
  g_strfreev(names);

  /* a schema with a key, an index on (a, b), a check and a foreign key on b */
  char *ixcols[] = {"a", "b"}, *pkcols[] = {"id"}, *fkcols[] = {"b"}, *refcols[] = {"id"};
  LaceIndex indexes[] = {
      {.name = "t_pkey", .columns = pkcols, .num_columns = 1, .kind = LACE_INDEX_PRIMARY, .constraint = "t_pkey"},
      {.name = "t_ab", .columns = ixcols, .num_columns = 2, .kind = LACE_INDEX_PLAIN},
      {.name = "t_chk", .kind = LACE_INDEX_CHECK, .check = "a > 0"},
  };
  LaceForeignKey fks[] = {{.name = "t_b_fk", .columns = fkcols, .num_columns = 1, .ref_table = "u",
                           .ref_columns = refcols, .num_ref_columns = 1}};
  LaceColumn cols[] = {{.name = "id", .key_position = 1}, {.name = "a"}, {.name = "b"}};
  LaceSchema s = {.name = "t", .columns = cols, .num_columns = 3, .indexes = indexes, .num_indexes = 3,
                  .foreign_keys = fks, .num_foreign_keys = 1};
  char *uses = lace_ddl_column_uses(&s, "b");
  CHECK(g_str_equal(uses, "index t_ab, foreign key t_b_fk"));
  g_free(uses);
  uses = lace_ddl_column_uses(&s, "id");
  CHECK(g_str_equal(uses, "the primary key t_pkey"));
  g_free(uses);
  uses = lace_ddl_column_uses(&s, "zzz");
  CHECK(g_str_equal(uses, ""));
  g_free(uses);
  CHECK(lace_ddl_index_covers(&s, ixcols, 1));
  CHECK(lace_ddl_index_covers(&s, ixcols, 2));
  CHECK(!lace_ddl_index_covers(&s, fkcols, 1));
  CHECK(!lace_ddl_index_covers(&s, ixcols, 0));
  CHECK(g_str_equal(lace_ddl_primary_key_name(&s), "t_pkey"));
  char *key = lace_ddl_key_columns(&s);
  CHECK(g_str_equal(key, "id"));
  g_free(key);
  CHECK(lace_ddl_primary_key_name(NULL) == NULL);

  char *t = lace_ddl_rows_text("orders", 12400, TRUE, FALSE);
  CHECK(g_str_equal(t, "orders (12,400 rows)"));
  g_free(t);
  t = lace_ddl_rows_text("orders", 1, TRUE, TRUE);
  CHECK(g_str_equal(t, "orders (≈1 row)"));
  g_free(t);
  t = lace_ddl_rows_text("orders", 0, FALSE, FALSE);
  CHECK(g_str_equal(t, "orders"));
  g_free(t);

  t = lace_ddl_banner_text("SELECT 1\nFROM t", 1500, 80);
  CHECK(g_str_equal(t, "SELECT 1 · 1.5 s"));
  g_free(t);
  t = lace_ddl_banner_text("UPDATE", 61000, 80);
  CHECK(g_str_equal(t, "UPDATE · 1:01"));
  g_free(t);
  t = lace_ddl_banner_text("SELECT a_very_long_column_name FROM somewhere", 100, 12);
  CHECK(g_str_has_prefix(t, "SELECT a_ve…") && g_str_has_suffix(t, " · 0.1 s"));
  g_free(t);
  t = lace_ddl_banner_text("a\tb", 0, 0);
  CHECK(g_str_equal(t, "a b · 0.0 s"));
  g_free(t);
  TEST_MAIN_END();
}
