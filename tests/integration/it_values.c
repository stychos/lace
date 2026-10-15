/*
 * Lace
 * Integration tests: values keep their exact form through insert, update,
 * read back and row lookup
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool create_values_table(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_vals");
  return it_ex(db, "%s",
               it_dialect(db,
                          "CREATE TABLE it_vals (id INTEGER PRIMARY KEY, t TEXT, "
                          "b BLOB, f REAL, big INTEGER)",
                          "CREATE TABLE it_vals (id INTEGER PRIMARY KEY, t TEXT, "
                          "d NUMERIC(40,10), b BYTEA, f DOUBLE PRECISION, big BIGINT)",
                          "CREATE TABLE it_vals (id INT PRIMARY KEY, t TEXT, "
                          "d DECIMAL(40,10), b VARBINARY(64), f DOUBLE, big BIGINT) "
                          "CHARACTER SET utf8mb4"));
}

/* Insert one column of row id, read it back, update it to `then`, read
 * again: both must equal what was written */
static LaceValue read_back(ItDb *db, int id, const char *col) {
  LaceResult *r = it_q(db, "SELECT %s FROM it_vals WHERE id = %d", col, id);
  LaceValue v = lace_value_null();
  if (r && r->num_rows == 1 && r->rows[0].num_cells == 1)
    v = lace_value_copy(&r->rows[0].cells[0]);
  else
    CHECK(!"row not found");
  lace_result_free(r);
  return v;
}

static bool same_value(const LaceValue *a, const LaceValue *b) {
  if (a->is_null || b->is_null)
    return a->is_null == b->is_null;
  if (a->type == LACE_TYPE_BLOB || b->type == LACE_TYPE_BLOB)
    return a->type == b->type && a->blob.len == b->blob.len &&
           (a->blob.len == 0 || memcmp(a->blob.data, b->blob.data, a->blob.len) == 0);
  if (a->type == LACE_TYPE_FLOAT && b->type == LACE_TYPE_FLOAT)
    return a->float_val == b->float_val;
  char *x = lace_value_to_edit_text(a), *y = lace_value_to_edit_text(b);
  bool same = x && y && strcmp(x, y) == 0;
  free(x);
  free(y);
  return same;
}

static void roundtrip(ItDb *db, int id, const char *col, LaceValue first,
                      LaceValue then) {
  LaceValue idv = lace_value_int(id);
  const char *cols[] = {"id", col};
  LaceValue vals[] = {idv, first};
  CHECK_OK(db, lace_insert(db->client, db->conn, "it_vals", cols, vals, 2));
  LaceValue got = read_back(db, id, col);
  if (!same_value(&got, &first)) {
    char *g = lace_value_to_edit_text(&got), *w = lace_value_to_edit_text(&first);
    fprintf(stderr, "  [%s] %s insert: got '%s' want '%s'\n", db->name, col,
            g ? g : "(null)", w ? w : "(null)");
    free(g);
    free(w);
    CHECK(false);
  }
  lace_value_free(&got);

  LacePkValue pk = {"id", idv};
  CHECK_OK(db, lace_update(db->client, db->conn, "it_vals", &pk, 1, col, &then));
  got = read_back(db, id, col);
  if (!same_value(&got, &then)) {
    char *g = lace_value_to_edit_text(&got), *w = lace_value_to_edit_text(&then);
    fprintf(stderr, "  [%s] %s update: got '%s' want '%s'\n", db->name, col,
            g ? g : "(null)", w ? w : "(null)");
    free(g);
    free(w);
    CHECK(false);
  }
  lace_value_free(&got);
  lace_value_free(&first);
  lace_value_free(&then);
}

/* Text that breaks naive quoting stays exactly the same */
static void test_text(ItDb *db) {
  if (!create_values_table(db))
    return;
  roundtrip(db, 1, "t", lace_value_text("it's"), lace_value_text("a\\'b OR 1=1 -- "));
  roundtrip(db, 2, "t", lace_value_text("C:\\dir\\"), lace_value_text("\"quoted\""));
  roundtrip(db, 3, "t", lace_value_text("h\xc3\xa9llo \xe2\x80\x94 \xe6\x97\xa5\xe6\x9c\xac"),
            lace_value_text("line1\nline2\ttab"));
  /* Empty text is '' (not NULL), and NULL is NULL */
  roundtrip(db, 4, "t", lace_value_text(""), lace_value_null());
  roundtrip(db, 5, "t", lace_value_null(), lace_value_text(""));
  it_ex(db, "DROP TABLE it_vals");
}

/* Integers beyond 2^53 stay integers (they travel as {"$int": "..."}) */
static void test_big_integers(ItDb *db) {
  if (!create_values_table(db))
    return;
  roundtrip(db, 1, "big", lace_value_int(9007199254740993LL),
            lace_value_int(INT64_MAX));
  roundtrip(db, 2, "big", lace_value_int(INT64_MIN), lace_value_int(-9007199254740993LL));

  /* And address rows exactly: MySQL compares a string with a BIGINT as a
   * double, which would match the neighbouring value too */
  it_ex(db, "INSERT INTO it_vals (id, big) VALUES (10, 9007199254740992), "
            "(11, 9007199254740993)");
  LacePkValue pk = {"big", lace_value_int(9007199254740993LL)};
  const char *cols[] = {"id"};
  LaceResult *r = NULL;
  CHECK_OK(db, lace_select_by_pk(db->client, db->conn, "it_vals", &pk, 1, cols, 1, &r));
  CHECK(r && r->num_rows == 1 && it_int(r, 0, 0) == 11);
  lace_result_free(r);
  it_ex(db, "DROP TABLE it_vals");
}

/* NUMERIC/DECIMAL keep every digit */
static void test_decimal(ItDb *db) {
  if (!create_values_table(db))
    return;
  LaceValue a, b;
  char *err = NULL;
  CHECK(lace_value_from_edit_text("12345678901234567890.0123456789", LACE_TYPE_DECIMAL, &a, &err));
  CHECK(lace_value_from_edit_text("-0.0000000001", LACE_TYPE_DECIMAL, &b, &err));
  free(err);
  roundtrip(db, 1, "d", a, b);
  LaceValue got = read_back(db, 1, "d");
  CHECK(got.type == LACE_TYPE_DECIMAL);
  lace_value_free(&got);
  it_ex(db, "DROP TABLE it_vals");
}

/* Bytes, including NUL, quote and backslash; an empty BLOB is not NULL */
static void test_blob(ItDb *db) {
  if (!create_values_table(db))
    return;
  const uint8_t bytes[] = {0x00, 0xff, 0x27, 0x5c, 0x22, 0x00, 0x41};
  roundtrip(db, 1, "b", lace_value_blob(bytes, sizeof(bytes)),
            lace_value_blob((const uint8_t *)"", 0));
  LaceValue got = read_back(db, 1, "b");
  CHECK(!got.is_null && got.type == LACE_TYPE_BLOB && got.blob.len == 0);
  lace_value_free(&got);
  it_ex(db, "DROP TABLE it_vals");
}

/* Doubles survive in their shortest exact form */
static void test_float(ItDb *db) {
  if (!create_values_table(db))
    return;
  roundtrip(db, 1, "f", lace_value_float(0.1), lace_value_float(1.0 / 3.0));
  roundtrip(db, 2, "f", lace_value_float(1e300), lace_value_float(-2.5e-300));
  LaceValue got = read_back(db, 1, "f");
  CHECK(got.type == LACE_TYPE_FLOAT && got.float_val == 1.0 / 3.0);
  lace_value_free(&got);
  it_ex(db, "DROP TABLE it_vals");
}

/* MySQL: BIT is bytes (a zero bit is a NUL byte, not ''); BIGINT UNSIGNED
 * past int64 keeps its digits and still finds its row */
static void test_mysql_bit_unsigned(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_bits");
  it_ex(db, "CREATE TABLE it_bits (id BIGINT UNSIGNED PRIMARY KEY, b BIT(8))");
  it_ex(db, "INSERT INTO it_bits VALUES (18446744073709551615, b'00000000'), "
            "(18446744073709551614, b'10000001')");
  LaceResult *r = it_q(db, "SELECT id, b FROM it_bits ORDER BY id");
  if (r && r->num_rows == 2) {
    const LaceValue *b0 = &r->rows[0].cells[1];
    CHECK(b0->type == LACE_TYPE_BLOB && b0->blob.len == 1 && b0->blob.data[0] == 0x81);
    const LaceValue *b1 = &r->rows[1].cells[1];
    CHECK(b1->type == LACE_TYPE_BLOB && b1->blob.len == 1 && b1->blob.data[0] == 0);
    char *id = it_text(r, 1, 0);
    CHECK_STR(id, "18446744073709551615");
    /* As a key: exactly that row */
    LacePkValue pk = {"id", lace_value_copy(&r->rows[1].cells[0])};
    const uint8_t ff = 0xff;
    LaceValue v = lace_value_blob(&ff, 1);
    CHECK_OK(db, lace_update(db->client, db->conn, "it_bits", &pk, 1, "b", &v));
    CHECK(it_scalar(db, "SELECT b + 0 FROM it_bits WHERE id = 18446744073709551615") == 255);
    CHECK(it_scalar(db, "SELECT b + 0 FROM it_bits WHERE id = 18446744073709551614") == 129);
    lace_value_free(&v);
    lace_value_free(&pk.value);
    free(id);
  } else {
    CHECK(false);
  }
  lace_result_free(r);
  it_ex(db, "DROP TABLE it_bits");
}

/* PostgreSQL with bytea_output = escape: still the bytes */
static void test_pg_bytea_escape(ItDb *db) {
  it_ex(db, "SET bytea_output = 'escape'");
  LaceResult *r = it_q(db, "SELECT '\\x00ff5c27'::bytea");
  CHECK(r && r->rows[0].cells[0].type == LACE_TYPE_BLOB &&
        r->rows[0].cells[0].blob.len == 4 && r->rows[0].cells[0].blob.data[1] == 0xff &&
        r->rows[0].cells[0].blob.data[2] == 0x5c);
  lace_result_free(r);
  it_ex(db, "SET bytea_output = 'hex'");
}

const ItTest it_values_tests[] = {
    {"text", test_text, IT_ALL, false},
    {"big_integers", test_big_integers, IT_ALL, false},
    {"decimal", test_decimal, IT_NET, false},
    {"blob", test_blob, IT_ALL, false},
    {"float", test_float, IT_ALL, false},
    {"mysql_bit_unsigned", test_mysql_bit_unsigned, IT_MYSQL, false},
    {"pg_bytea_escape", test_pg_bytea_escape, IT_POSTGRES | IT_TUNNEL, false},
    {NULL, NULL, 0, false},
};
