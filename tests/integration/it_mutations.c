/*
 * Lace
 * Integration tests: lace_update / lace_delete / lace_insert addressing
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdlib.h>
#include <string.h>

/* An update or delete must hit exactly one row */
static void test_row_counts(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_mut");
  it_ex(db, "CREATE TABLE it_mut (id INTEGER PRIMARY KEY, v INTEGER)");
  it_ex(db, "INSERT INTO it_mut VALUES (1, 10), (2, 20)");

  LacePkValue pk = {"id", lace_value_int(1)};
  LaceValue v = lace_value_int(11);
  CHECK_OK(db, lace_update(db->client, db->conn, "it_mut", &pk, 1, "v", &v));
  CHECK(it_scalar(db, "SELECT v FROM it_mut WHERE id = 1") == 11);

  /* Writing the value the row already has still finds the row (MySQL
   * counts matched rows: CLIENT_FOUND_ROWS) */
  CHECK_OK(db, lace_update(db->client, db->conn, "it_mut", &pk, 1, "v", &v));

  LacePkValue missing = {"id", lace_value_int(99)};
  CHECK(lace_update(db->client, db->conn, "it_mut", &missing, 1, "v", &v) ==
        LACE_ERR_ROW_NOT_FOUND);
  CHECK(lace_delete(db->client, db->conn, "it_mut", &missing, 1) ==
        LACE_ERR_ROW_NOT_FOUND);

  CHECK_OK(db, lace_delete(db->client, db->conn, "it_mut", &pk, 1));
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_mut") == 1);
  it_ex(db, "DROP TABLE it_mut");
}

/* A delete whose foreign keys cascade is one row: the children the database
 * removes with it are not the statement's rows (SQLite's total_changes
 * counted them, and the one-row check refused a delete that had happened) */
static void test_cascade_delete(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_casc_c");
  it_ex(db, "DROP TABLE IF EXISTS it_casc_p");
  if (!it_ex(db, "CREATE TABLE it_casc_p (id INTEGER PRIMARY KEY)") ||
      !it_ex(db, "CREATE TABLE it_casc_c (id INTEGER PRIMARY KEY, p INTEGER, "
                 "FOREIGN KEY (p) REFERENCES it_casc_p(id) ON DELETE CASCADE)"))
    return;
  it_ex(db, "INSERT INTO it_casc_p VALUES (1), (2)");
  it_ex(db, "INSERT INTO it_casc_c VALUES (10, 1), (11, 1), (20, 2)");
  LacePkValue pk = {"id", lace_value_int(1)};
  CHECK_OK(db, lace_delete(db->client, db->conn, "it_casc_p", &pk, 1));
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_casc_c") == 1);
  LaceResult *r = NULL;
  CHECK_OK(db, lace_exec(db->client, db->conn, "DELETE FROM it_casc_p WHERE id = 2", &r));
  CHECK(r && r->rows_affected == 1); /* not the child it took with it */
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_casc_c") == 0);
  it_ex(db, "DROP TABLE it_casc_c");
  it_ex(db, "DROP TABLE it_casc_p");
}

/* A "key" that matches several rows is reported */
static void test_multiple_rows(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_nokey");
  it_ex(db, "CREATE TABLE it_nokey (k INTEGER, v INTEGER)");
  it_ex(db, "INSERT INTO it_nokey VALUES (1, 1), (1, 2)");
  LacePkValue pk = {"k", lace_value_int(1)};
  LaceValue v = lace_value_int(5);
  CHECK(lace_update(db->client, db->conn, "it_nokey", &pk, 1, "v", &v) ==
        LACE_ERR_MULTIPLE_ROWS);
  /* lace_select_by_pk shows the caller beforehand: two rows */
  const char *cols[] = {"v"};
  LaceResult *r = NULL;
  CHECK_OK(db, lace_select_by_pk(db->client, db->conn, "it_nokey", &pk, 1, cols, 1, &r));
  CHECK(r && r->num_rows == 2);
  lace_result_free(r);
  it_ex(db, "DROP TABLE it_nokey");
}

/* Composite keys address one row; a NULL key part uses IS NULL */
static void test_composite_and_null_keys(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_ck2");
  it_ex(db, "CREATE TABLE it_ck2 (a INTEGER NOT NULL, b VARCHAR(10) NOT NULL, "
            "v INTEGER, PRIMARY KEY (a, b))");
  it_ex(db, "INSERT INTO it_ck2 VALUES (1, 'x', 1), (1, 'y', 2), (2, 'x', 3)");
  LacePkValue pk[2] = {{"a", lace_value_int(1)}, {"b", lace_value_text("y")}};
  LaceValue v = lace_value_int(20);
  CHECK_OK(db, lace_update(db->client, db->conn, "it_ck2", pk, 2, "v", &v));
  CHECK(it_scalar(db, "SELECT v FROM it_ck2 WHERE a = 1 AND b = 'y'") == 20);
  CHECK(it_scalar(db, "SELECT SUM(v) FROM it_ck2") == 24);
  lace_value_free(&pk[1].value);

  it_ex(db, "DROP TABLE IF EXISTS it_nullk");
  it_ex(db, "CREATE TABLE it_nullk (k INTEGER, v INTEGER)");
  it_ex(db, "INSERT INTO it_nullk VALUES (NULL, 1), (2, 2)");
  LacePkValue nk = {"k", lace_value_null()};
  CHECK_OK(db, lace_update(db->client, db->conn, "it_nullk", &nk, 1, "v", &v));
  CHECK(it_scalar(db, "SELECT v FROM it_nullk WHERE k IS NULL") == 20);
  it_ex(db, "DROP TABLE it_ck2");
  it_ex(db, "DROP TABLE it_nullk");
}

/* A value too large to load is a label, never written or used as a key */
static void test_omitted_refused(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_om");
  it_ex(db, "CREATE TABLE it_om (id INTEGER PRIMARY KEY, t TEXT)");
  it_ex(db, "INSERT INTO it_om VALUES (1, 'keep')");
  LaceValue big = lace_value_text("[TEXT: 5000000 bytes]");
  big.omitted = 5000000;
  LacePkValue pk = {"id", lace_value_int(1)};
  CHECK(lace_update(db->client, db->conn, "it_om", &pk, 1, "t", &big) ==
        LACE_ERR_INVALID_PARAMS);
  LacePkValue bad = {"t", big};
  CHECK(lace_delete(db->client, db->conn, "it_om", &bad, 1) == LACE_ERR_INVALID_PARAMS);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_om WHERE t = 'keep'") == 1);
  lace_value_free(&big);
  it_ex(db, "DROP TABLE it_om");
}

/* The SQL a mutation ran is available to the caller (query history) */
static void test_last_sql(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_hist");
  it_ex(db, "CREATE TABLE it_hist (id INTEGER PRIMARY KEY, t TEXT)");
  it_ex(db, "INSERT INTO it_hist VALUES (1, 'a')");
  LacePkValue pk = {"id", lace_value_int(1)};
  LaceValue v = lace_value_text("b");
  CHECK_OK(db, lace_update(db->client, db->conn, "it_hist", &pk, 1, "t", &v));
  const char *sql = lace_last_sql();
  CHECK(sql && strstr(sql, "UPDATE") && strstr(sql, "it_hist") && strstr(sql, "'b'"));
  lace_value_free(&v);
  it_ex(db, "DROP TABLE it_hist");
}

const ItTest it_mutation_tests[] = {
    {"row_counts", test_row_counts, IT_ALL, false},
    {"multiple_rows", test_multiple_rows, IT_ALL, false},
    {"cascade_delete", test_cascade_delete, IT_ALL, false},
    {"composite_and_null_keys", test_composite_and_null_keys, IT_ALL, false},
    {"omitted_refused", test_omitted_refused, IT_ALL, false},
    {"last_sql", test_last_sql, IT_ALL, false},
    {NULL, NULL, 0, false},
};
