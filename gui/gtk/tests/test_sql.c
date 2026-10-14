/*
 * Lace GTK frontend
 * Unit tests: SQL builders, statement scanning, row keys
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "test.h"
#include "sql/row_key.h"
#include "sql/sql_builder.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void check_free(char *got, const char *expected) {
  CHECK_STR(got, expected);
  free(got);
}

static void test_display(void) {
  LaceValue v = lace_value_null();
  check_free(sql_display_text(&v, 0), "NULL");
  v = lace_value_int(-42);
  check_free(sql_display_text(&v, 0), "-42");
  v = lace_value_float(0.1);
  check_free(sql_display_text(&v, 0), "0.1");
  v = lace_value_bool(true);
  check_free(sql_display_text(&v, 0), "true");
  v = lace_value_text("a\nb\r\nc");
  check_free(sql_display_text(&v, 0), "a⏎b⏎c");
  check_free(sql_display_text(&v, 2), "a⏎…");
  lace_value_free(&v);
  v = lace_value_text("héllo wörld");
  check_free(sql_display_text(&v, 5), "héllo…");
  lace_value_free(&v);
  uint8_t bytes[3] = {0xde, 0xad, 0x01};
  v = lace_value_blob(bytes, 3);
  check_free(sql_display_text(&v, 0), "0xdead01 (3 bytes)");
  lace_value_free(&v);
  v = lace_value_blob(bytes, 0);
  check_free(sql_display_text(&v, 0), "0x (0 bytes)");
  lace_value_free(&v);
  CHECK(sql_type_is_numeric(LACE_TYPE_DECIMAL));
  CHECK(!sql_type_is_numeric(LACE_TYPE_TEXT));
}

static void test_edit_value(void) {
  LaceValue out;
  bool unchanged;
  char *err = NULL;
  LaceValue orig = lace_value_text("abc");
  CHECK(sql_edit_value(&orig, LACE_TYPE_TEXT, "abc", &out, &unchanged, &err) && unchanged);
  CHECK(sql_edit_value(&orig, LACE_TYPE_TEXT, "", &out, &unchanged, &err) && !unchanged);
  CHECK(out.type == LACE_TYPE_TEXT && !out.is_null && out.text.len == 0);
  lace_value_free(&out);
  CHECK(sql_edit_value(&orig, LACE_TYPE_TEXT, NULL, &out, &unchanged, &err) && !unchanged);
  CHECK(out.is_null);
  lace_value_free(&orig);
  orig = lace_value_null();
  CHECK(sql_edit_value(&orig, LACE_TYPE_INT, "", &out, &unchanged, &err) && unchanged);
  CHECK(sql_edit_value(&orig, LACE_TYPE_INT, NULL, &out, &unchanged, &err) && unchanged);
  CHECK(sql_edit_value(&orig, LACE_TYPE_INT, "42", &out, &unchanged, &err) && !unchanged);
  CHECK(out.type == LACE_TYPE_INT && out.int_val == 42);
  uint8_t b[1] = {1};
  orig = lace_value_blob(b, 1);
  CHECK(!sql_edit_value(&orig, LACE_TYPE_BLOB, "zz", &out, &unchanged, &err));
  CHECK(err != NULL);
  free(err);
  lace_value_free(&orig);
}

static void test_shape(void) {
  SqlShape s;
  sql_shape("SELECT 1; -- c", LACE_DRIVER_SQLITE, &s);
  CHECK_STR(s.first_keyword, "SELECT");
  CHECK_STR(s.verb, "SELECT");
  CHECK(s.end == 8);
  CHECK(sql_shape_pageable(&s));
  CHECK(!sql_shape_needs_wrap(&s));

  sql_shape("WITH u AS (UPDATE t SET a=1 RETURNING *) SELECT * FROM u",
            LACE_DRIVER_POSTGRES, &s);
  CHECK_STR(s.first_keyword, "WITH");
  CHECK_STR(s.verb, "SELECT");
  CHECK(s.nested_write);
  CHECK(!sql_shape_pageable(&s));

  sql_shape("SELECT * FROM t LIMIT 5", LACE_DRIVER_SQLITE, &s);
  CHECK(s.top_level_limit);
  CHECK(sql_shape_needs_wrap(&s));
  check_free(sql_page_of_statement("SELECT * FROM t LIMIT 5", true, 10, 20),
             "SELECT * FROM (\nSELECT * FROM t LIMIT 5\n) AS lace_page LIMIT 10 OFFSET 20");
  check_free(sql_page_of_statement("SELECT 1", false, 10, 0),
             "SELECT 1\nLIMIT 10 OFFSET 0");
  check_free(sql_count_of_statement("SELECT 1"),
             "SELECT COUNT(*) FROM (\nSELECT 1\n) AS lace_count");

  sql_shape("SELECT * FROM (SELECT 1 LIMIT 1) x", LACE_DRIVER_SQLITE, &s);
  CHECK(!s.top_level_limit);

  sql_shape("VALUES (1), (2)", LACE_DRIVER_SQLITE, &s);
  CHECK_STR(s.verb, "VALUES");
  CHECK(sql_shape_pageable(&s));
  CHECK(sql_shape_needs_wrap(&s));

  sql_shape("INSERT INTO t VALUES (1)", LACE_DRIVER_SQLITE, &s);
  CHECK_STR(s.verb, "INSERT");
  CHECK(!sql_shape_pageable(&s));

  sql_shape("SELECT * INTO new_t FROM t", LACE_DRIVER_POSTGRES, &s);
  CHECK(s.top_level_into);
  CHECK(!sql_shape_pageable(&s));

  sql_shape("select merge from t", LACE_DRIVER_SQLITE, &s);
  CHECK(sql_shape_pageable(&s));

  sql_shape("PRAGMA table_info(t)", LACE_DRIVER_SQLITE, &s);
  CHECK(!sql_shape_pageable(&s));

  /* A string holding a semicolon and a comment keeps the end past it */
  sql_shape("SELECT 'a;b' -- x\n", LACE_DRIVER_SQLITE, &s);
  CHECK(s.end == 12);
}

static void test_statement_at(void) {
  const char *t = "SELECT 1;\n\nSELECT 2;  \n-- tail\n";
  size_t s, e;
  CHECK(sql_statement_at(t, 0, LACE_DRIVER_SQLITE, &s, &e));
  CHECK(s == 0 && e == 8);
  CHECK(sql_statement_at(t, 8, LACE_DRIVER_SQLITE, &s, &e)); /* at ';' */
  CHECK(s == 0 && e == 8);
  CHECK(sql_statement_at(t, 10, LACE_DRIVER_SQLITE, &s, &e)); /* blank gap */
  CHECK(s == 0 && e == 8);
  CHECK(sql_statement_at(t, 13, LACE_DRIVER_SQLITE, &s, &e));
  CHECK(s == 11 && e == 19);
  CHECK(sql_statement_at(t, strlen(t), LACE_DRIVER_SQLITE, &s, &e));
  CHECK(s == 11 && e == 19);
  CHECK(!sql_statement_at("  -- only\n", 3, LACE_DRIVER_SQLITE, &s, &e));
  CHECK(!sql_statement_at("", 0, LACE_DRIVER_SQLITE, &s, &e));

  /* Iteration */
  size_t next = 0;
  int n = 0;
  while (sql_next_statement(t, next, LACE_DRIVER_SQLITE, &s, &e, &next)) {
    n++;
    if (n == 1)
      CHECK(s == 0 && e == 8);
    if (n == 2)
      CHECK(s == 11 && e == 19);
  }
  CHECK(n == 2);

  /* A ';' inside a dollar-quoted body does not split on PostgreSQL */
  const char *pg = "CREATE FUNCTION f() RETURNS int AS $$ SELECT 1; $$ LANGUAGE sql; SELECT 2";
  next = 0;
  n = 0;
  while (sql_next_statement(pg, next, LACE_DRIVER_POSTGRES, &s, &e, &next))
    n++;
  CHECK(n == 2);
}

static LaceSchema *make_schema(void) {
  LaceSchema *sc = calloc(1, sizeof(*sc));
  sc->num_columns = 3;
  sc->columns = calloc(3, sizeof(LaceColumn));
  sc->columns[0].name = strdup("id");
  sc->columns[0].type = LACE_TYPE_INT;
  sc->columns[0].primary_key = true;
  sc->columns[1].name = strdup("name");
  sc->columns[1].type = LACE_TYPE_TEXT;
  sc->columns[2].name = strdup("note");
  sc->columns[2].type = LACE_TYPE_TEXT;
  return sc;
}

static void free_schema(LaceSchema *sc) {
  for (size_t i = 0; i < sc->num_columns; i++)
    free(sc->columns[i].name);
  free(sc->columns);
  free(sc);
}

static void test_filters(void) {
  LaceSchema *sc = make_schema();
  Filter f[3] = {
      {strdup("id"), FILTER_GT, strdup("10"), NULL},
      {strdup("name"), FILTER_CONTAINS, strdup("o'k"), NULL},
      {NULL, FILTER_RAW, strdup(" note IS NOT NULL "), NULL},
  };
  check_free(sql_build_where(f, 3, NULL, sc, LACE_DRIVER_SQLITE),
             "\"id\" > 10 AND \"name\" LIKE '%o''k%' AND (note IS NOT NULL)");
  check_free(sql_build_where(f, 3, NULL, sc, LACE_DRIVER_MYSQL),
             "`id` > 10 AND `name` LIKE '%o''k%' AND (note IS NOT NULL)");

  /* Operators */
  Filter one = {strdup("name"), FILTER_EQ, strdup("x"), NULL};
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_POSTGRES), "\"name\" = 'x'");
  one.op = FILTER_IN;
  free(one.value);
  one.value = strdup("a, 'b c', 3");
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE),
             "\"name\" IN ('a', 'b c', 3)");
  one.op = FILTER_BETWEEN;
  free(one.value);
  one.value = strdup("1");
  one.value2 = strdup("5");
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE),
             "\"name\" BETWEEN 1 AND 5");
  free(one.value2);
  one.value2 = NULL;
  CHECK(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE) == NULL);
  one.op = FILTER_NULL;
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE), "\"name\" IS NULL");
  one.op = FILTER_STARTS;
  free(one.value);
  one.value = strdup("ab");
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE), "\"name\" LIKE 'ab%'");
  one.op = FILTER_REGEX;
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_POSTGRES), "\"name\" ~ 'ab'");
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_MARIADB), "`name` REGEXP 'ab'");
  check_free(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE), "\"name\" GLOB '*ab*'");
  /* Unknown column: skipped */
  free(one.column);
  one.column = strdup("nope");
  one.op = FILTER_EQ;
  CHECK(sql_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE) == NULL);
  filter_clear(&one);

  /* Search over text columns */
  check_free(sql_build_where(NULL, 0, "bob", sc, LACE_DRIVER_SQLITE),
             "(\"name\" LIKE '%bob%' OR \"note\" LIKE '%bob%')");
  check_free(sql_build_where(f, 1, "bob", sc, LACE_DRIVER_POSTGRES),
             "\"id\" > 10 AND (\"name\" ILIKE '%bob%' OR \"note\" ILIKE '%bob%')");
  CHECK(sql_build_where(NULL, 0, "", sc, LACE_DRIVER_SQLITE) == NULL);

  for (int i = 0; i < 3; i++)
    filter_clear(&f[i]);

  SortSpec sorts[2] = {{"name", true}, {"id", false}};
  check_free(sql_build_order_by(sorts, 2, LACE_DRIVER_MYSQL), "`name` DESC, `id` ASC");
  CHECK(sql_build_order_by(NULL, 0, LACE_DRIVER_MYSQL) == NULL);

  check_free(sql_table_page(LACE_DRIVER_POSTGRES, "Sales.orders", "\"id\" > 1",
                            "\"id\" ASC", 1000, 2000),
             "SELECT * FROM \"Sales\".\"orders\" WHERE \"id\" > 1 ORDER BY \"id\" ASC LIMIT 1000 OFFSET 2000");
  check_free(sql_table_page(LACE_DRIVER_SQLITE, "t", NULL, NULL, 10, 0),
             "SELECT * FROM \"t\" LIMIT 10 OFFSET 0");
  check_free(sql_table_count(LACE_DRIVER_MYSQL, "t", NULL), "SELECT COUNT(*) FROM `t`");
  check_free(sql_table_count(LACE_DRIVER_MYSQL, "t", "x = 1"),
             "SELECT COUNT(*) FROM `t` WHERE x = 1");

  CHECK(filter_op_from_key("contains") == FILTER_CONTAINS);
  CHECK(filter_op_from_key("bogus") == FILTER_EQ);
  CHECK_STR(filter_op_key(FILTER_NOT_NULL), "not_null");
  CHECK(filter_op_two_values(FILTER_BETWEEN));
  CHECK(!filter_op_needs_value(FILTER_EMPTY));
  free_schema(sc);
}

static void test_simple_select(void) {
  check_free(sql_simple_select_table("select * from \"Orders\" where x = 1", LACE_DRIVER_SQLITE), "Orders");
  check_free(sql_simple_select_table("SELECT a, b FROM main.t", LACE_DRIVER_SQLITE), "t");
  check_free(sql_simple_select_table("SELECT * FROM Orders", LACE_DRIVER_POSTGRES), "orders");
  check_free(sql_simple_select_table("SELECT * FROM sales.\"Odd.Name\" LIMIT 5", LACE_DRIVER_POSTGRES), "sales.\"Odd.Name\"");
  check_free(sql_simple_select_table("SELECT * FROM `t`", LACE_DRIVER_MYSQL), "t");
  CHECK(sql_simple_select_table("SELECT * FROM a JOIN b ON a.id = b.id", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT * FROM a UNION SELECT * FROM b", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT * FROM (SELECT 1) x", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT count(*) FROM t", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT a FROM t GROUP BY a", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT DISTINCT a FROM t", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT * FROM a, b", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT * FROM other.t", LACE_DRIVER_MYSQL) == NULL);
  CHECK(sql_simple_select_table("INSERT INTO t VALUES (1)", LACE_DRIVER_SQLITE) == NULL);
  CHECK(sql_simple_select_table("SELECT 1", LACE_DRIVER_SQLITE) == NULL);
  /* "JOIN" inside a string does not count */
  check_free(sql_simple_select_table("SELECT * FROM t WHERE n = 'JOIN'", LACE_DRIVER_SQLITE), "t");
}

static LaceResult *make_result(void) {
  /* Columns in a different order than the schema: name, id, extra */
  LaceResult *r = calloc(1, sizeof(*r));
  r->num_columns = 3;
  r->columns = calloc(3, sizeof(LaceColumn));
  r->columns[0].name = strdup("name");
  r->columns[1].name = strdup("id");
  r->columns[2].name = strdup("extra");
  r->num_rows = 2;
  r->rows = calloc(2, sizeof(LaceRow));
  for (size_t i = 0; i < 2; i++) {
    r->rows[i].num_cells = 3;
    r->rows[i].cells = calloc(3, sizeof(LaceValue));
    r->rows[i].cells[0] = lace_value_text(i == 0 ? "alice" : "bob");
    r->rows[i].cells[1] = i == 0 ? lace_value_int(7) : lace_value_null();
    r->rows[i].cells[2] = lace_value_float(1.5);
  }
  return r;
}

static void test_row_key(void) {
  LaceSchema *sc = make_schema();
  LaceResult *r = make_result();
  RowKey key;
  const char *why = NULL;
  CHECK(row_key_build(&key, r, 0, sc, true, &why) == ROW_KEY_OK);
  CHECK(key.count == 1);
  CHECK_STR(key.names[0], "id");
  CHECK(key.values[0].type == LACE_TYPE_INT && key.values[0].int_val == 7);
  LacePkValue *pk = row_key_to_pk(&key);
  CHECK_STR(pk[0].column, "id");
  free(pk);
  RowKey copy;
  row_key_copy(&copy, &key);
  CHECK(copy.count == 1 && copy.values[0].int_val == 7);
  row_key_free(&copy);
  row_key_free(&key);

  /* NULL key part */
  CHECK(row_key_build(&key, r, 1, sc, true, &why) == ROW_KEY_BAD);
  CHECK(why && strstr(why, "NULL"));

  /* Snapshot: every column of the result that belongs to the table */
  RowSnapshot snap;
  row_snapshot_build(&snap, r, 0, SIZE_MAX, sc);
  CHECK(snap.count == 2); /* name and id, not extra */
  CHECK_STR(snap.names[0], "name");
  CHECK_STR(snap.names[1], "id");
  row_snapshot_free(&snap);
  row_snapshot_build(&snap, r, 0, 2, sc);
  CHECK(snap.count == 1);
  CHECK_STR(snap.names[0], "extra");
  row_snapshot_free(&snap);

  /* Missing pk column */
  free(r->columns[1].name);
  r->columns[1].name = strdup("ident");
  CHECK(row_key_build(&key, r, 0, sc, true, &why) == ROW_KEY_NONE);
  /* Duplicate pk column with unique_names */
  free(r->columns[1].name);
  r->columns[1].name = strdup("id");
  free(r->columns[2].name);
  r->columns[2].name = strdup("id");
  CHECK(row_key_build(&key, r, 0, sc, true, &why) == ROW_KEY_NONE);
  CHECK(row_key_build(&key, r, 0, sc, false, &why) == ROW_KEY_OK);
  row_key_free(&key);

  /* No primary key in the schema */
  sc->columns[0].primary_key = false;
  CHECK(row_key_build(&key, r, 0, sc, true, &why) == ROW_KEY_NONE);

  lace_result_free(r);
  free_schema(sc);
}

int main(void) {
  test_display();
  test_edit_value();
  test_shape();
  test_statement_at();
  test_filters();
  test_simple_select();
  test_row_key();
  TEST_MAIN_END();
}
