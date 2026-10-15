/*
 * Lace
 * Integration tests: statements and the shape of their results
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdlib.h>
#include <string.h>

/* A SELECT comes back as columns and rows */
static void test_select_shape(ItDb *db) {
  LaceResult *r = it_q(db, "SELECT 1 AS a, 'x' AS b");
  if (!r)
    return;
  CHECK_SIZE(r->num_columns, 2);
  CHECK_SIZE(r->num_rows, 1);
  CHECK_STR(r->columns[0].name, "a");
  CHECK_STR(r->columns[1].name, "b");
  CHECK(it_int(r, 0, 0) == 1);
  char *b = it_text(r, 0, 1);
  CHECK_STR(b, "x");
  free(b);
  CHECK(!r->truncated);
  lace_result_free(r);
}

/* A statement without rows reports its affected count */
static void test_exec_shape(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_shape");
  it_ex(db, "CREATE TABLE it_shape (id INTEGER PRIMARY KEY, v INTEGER)");
  LaceResult *r = it_q(db, "INSERT INTO it_shape VALUES (1, 10), (2, 20), (3, 30)");
  if (r) {
    CHECK_SIZE(r->num_columns, 0);
    CHECK(r->rows_affected == 3);
  }
  lace_result_free(r);
  r = it_q(db, "UPDATE it_shape SET v = v + 1 WHERE id >= 2");
  CHECK(r && r->rows_affected == 2);
  lace_result_free(r);
  it_ex(db, "DROP TABLE it_shape");
}

/* RETURNING has rows although the statement is an INSERT: the reply goes
 * by the result's shape, not the keyword */
static void test_returning(ItDb *db) {
  if (db->driver == LACE_DRIVER_MYSQL)
    return; /* MySQL has no RETURNING (MariaDB has) */
  it_ex(db, "DROP TABLE IF EXISTS it_ret");
  it_ex(db, "CREATE TABLE it_ret (id INTEGER PRIMARY KEY, v INTEGER)");
  LaceResult *r = it_q(db, "INSERT INTO it_ret VALUES (7, 70) RETURNING id, v");
  if (r) {
    CHECK_SIZE(r->num_rows, 1);
    CHECK(it_int(r, 0, 0) == 7 && it_int(r, 0, 1) == 70);
  }
  lace_result_free(r);
  it_ex(db, "DROP TABLE it_ret");
}

/* Several statements in one string all run; the reply is the last result
 * with rows. A failing statement stops the run and is reported, and the
 * connection stays usable. */
static void test_multi_statement(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_multi");
  it_ex(db, "CREATE TABLE it_multi (id INTEGER PRIMARY KEY)");
  LaceResult *r = it_q(db, "INSERT INTO it_multi VALUES (1); INSERT INTO it_multi "
                           "VALUES (2); SELECT COUNT(*) FROM it_multi");
  CHECK(r && r->num_rows == 1 && it_int(r, 0, 0) == 2);
  lace_result_free(r);

  int rc = it_fail(db, "INSERT INTO it_multi VALUES (3); SELECT * FROM "
                       "it_no_such_table; INSERT INTO it_multi VALUES (4)");
  CHECK(rc != LACE_OK);
  /* The statement after the failure never runs. The one before it stays
   * (autocommit), except on PostgreSQL, which runs the whole string as one
   * implicit transaction and rolls it all back */
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_multi") ==
        (db->driver == LACE_DRIVER_POSTGRES ? 2 : 3));
  CHECK(it_scalar(db, "SELECT 5") == 5);
  it_ex(db, "DROP TABLE it_multi");
}

/* Errors come back as codes with the server's message */
static void test_errors(ItDb *db) {
  int rc = it_fail(db, "SELEC 1");
  CHECK(rc == LACE_ERR_QUERY_FAILED);
  CHECK(strlen(lace_client_error(db->client)) > 0);
  rc = it_fail(db, "SELECT * FROM it_definitely_missing");
  CHECK(rc == LACE_ERR_QUERY_FAILED);
  CHECK(it_scalar(db, "SELECT 1") == 1); /* Still usable */

  LaceResult *r = NULL;
  CHECK(lace_exec(db->client, 999999, "SELECT 1", &r) == LACE_ERR_INVALID_CONN_ID);
  CHECK(r == NULL);
}

/* Several connections to one database are independent */
static void test_two_connections(ItDb *db) {
  int other = it_connect_again(db);
  CHECK(other > 0 && other != db->conn);
  if (other <= 0)
    return;
  LaceResult *r = NULL;
  CHECK_OK(db, lace_exec(db->client, other, "SELECT 42", &r));
  CHECK(r && it_int(r, 0, 0) == 42);
  lace_result_free(r);
  CHECK_OK(db, lace_disconnect(db->client, other));
  CHECK(lace_exec(db->client, other, "SELECT 1", NULL) == LACE_ERR_INVALID_CONN_ID);
}

/* Counting: exact, and an estimate allowed only where asked for */
static void test_count(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_count");
  it_ex(db, "CREATE TABLE it_count (id INTEGER PRIMARY KEY)");
  it_ex(db, "INSERT INTO it_count VALUES (1), (2), (3), (4), (5)");
  size_t n = 0;
  bool approx = false;
  CHECK_OK(db, lace_count(db->client, db->conn, "it_count", &n, &approx));
  CHECK_SIZE(n, 5);
  CHECK(!approx);
  /* A small table is counted exactly even when an estimate is allowed */
  approx = true;
  CHECK_OK(db, lace_count(db->client, db->conn, "it_count", &n, &approx));
  CHECK_SIZE(n, 5);
  CHECK(!approx);
  it_ex(db, "DROP TABLE it_count");
}

/* Results stop at the daemon's row limit and say so */
static void test_row_limit(ItDb *db) {
  size_t over = (size_t)LACE_MAX_RESULT_ROWS + 1000;
  /* Recursion caps: MySQL 1000 levels, MariaDB 11 1000 iterations */
  if (db->driver == LACE_DRIVER_MYSQL)
    it_ex(db, "SET SESSION cte_max_recursion_depth = 4294967295");
  if (db->driver == LACE_DRIVER_MARIADB)
    it_ex(db, "SET SESSION max_recursive_iterations = 4294967295");
  LaceResult *r = it_q(
      db,
      db->driver == LACE_DRIVER_POSTGRES
          ? "SELECT g FROM generate_series(1, %zu) g"
          : "WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s "
            "WHERE n < %zu) SELECT n FROM s",
      over);
  if (r) {
    CHECK(r->truncated);
    CHECK_SIZE(r->num_rows, LACE_MAX_RESULT_ROWS);
  }
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT 1") == 1); /* The rest was drained or stopped */
}

/* A big result in the middle of a string: truncated, and the other
 * statements still run (no cancel that would skip or roll them back) */
static void test_row_limit_multi(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_lim");
  it_ex(db, "CREATE TABLE it_lim (id INTEGER PRIMARY KEY)");
  size_t over = (size_t)LACE_MAX_RESULT_ROWS + 1000;
  const char *cap = db->driver == LACE_DRIVER_MYSQL
                        ? "SET SESSION cte_max_recursion_depth = 4294967295; "
                    : db->driver == LACE_DRIVER_MARIADB
                        ? "SET SESSION max_recursive_iterations = 4294967295; "
                        : "";
  int rc = it_fail(db,
                   db->driver == LACE_DRIVER_POSTGRES
                       ? "%sINSERT INTO it_lim VALUES (1); SELECT g FROM "
                         "generate_series(1, %zu) g; INSERT INTO it_lim VALUES (2)"
                       : "%sINSERT INTO it_lim VALUES (1); WITH RECURSIVE s(n) AS "
                         "(SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < %zu) "
                         "SELECT n FROM s; INSERT INTO it_lim VALUES (2)",
                   cap, over);
  CHECK(rc == LACE_OK);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_lim") == 2);

  /* PostgreSQL: inside a transaction, a truncated result leaves it usable */
  if (db->driver == LACE_DRIVER_POSTGRES) {
    it_ex(db, "BEGIN");
    LaceResult *r = it_q(db, "SELECT g FROM generate_series(1, %zu) g", over);
    CHECK(r && r->truncated && r->in_transaction);
    lace_result_free(r);
    CHECK(lace_last_transaction_state() == 1); /* Open, not failed */
    CHECK(it_ex(db, "INSERT INTO it_lim VALUES (3)"));
    it_ex(db, "COMMIT");
    CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_lim") == 3);
  }
  it_ex(db, "DROP TABLE it_lim");
}

/* A writing statement whose RETURNING rows pass the limit: truncated, but
 * never cancelled (that would roll the write back while reporting success),
 * so every row is written */
static void test_row_limit_write(ItDb *db) {
  if (db->driver == LACE_DRIVER_MYSQL)
    return; /* MySQL has no RETURNING (MariaDB does) */
  it_ex(db, "DROP TABLE IF EXISTS it_limw");
  it_ex(db, "CREATE TABLE it_limw (id INTEGER PRIMARY KEY)");
  size_t over = (size_t)LACE_MAX_RESULT_ROWS + 1000;
  char *sql =
      db->driver == LACE_DRIVER_POSTGRES
          ? str_printf("INSERT INTO it_limw SELECT g FROM generate_series(1, %zu) g "
                       "RETURNING id", over)
      : db->driver == LACE_DRIVER_MARIADB
          ? str_printf("INSERT INTO it_limw SELECT seq FROM seq_1_to_%zu RETURNING id", over)
          : str_printf("INSERT INTO it_limw WITH RECURSIVE s(n) AS (SELECT 1 UNION "
                       "ALL SELECT n + 1 FROM s WHERE n < %zu) SELECT n FROM s "
                       "RETURNING id", over);
  LaceResult *r = it_q(db, "%s", sql);
  free(sql);
  CHECK(r && r->truncated);
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_limw") == (int64_t)over);

  /* PostgreSQL: a data-modifying WITH streams its outer SELECT, so a cancel
   * there would abort the implicit transaction and undo the INSERT */
  if (db->driver == LACE_DRIVER_POSTGRES) {
    it_ex(db, "DELETE FROM it_limw");
    r = it_q(db, "WITH ins AS (INSERT INTO it_limw SELECT g FROM generate_series(1, %zu) g "
                 "RETURNING id) SELECT * FROM ins", over);
    CHECK(r && r->truncated);
    lace_result_free(r);
    CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_limw") == (int64_t)over);
  }
  it_ex(db, "DROP TABLE it_limw");
}

/* lace_socket is the daemon's own option for its SSH tunnels: from a client
 * it is refused (it would bypass the tunnel or reach any local socket) */
static void test_lace_socket_refused(ItDb *db) {
  char *url = str_printf("%s%clace_socket=/nonexistent/.s.PGSQL.5432", db->url,
                         strchr(db->url, '?') ? '&' : '?');
  int conn = 0;
  CHECK(lace_connect(db->client, url, NULL, &conn) != LACE_OK);
  CHECK(strstr(lace_client_error(db->client), "lace_socket") != NULL);
  free(url);
}

const ItTest it_basic_tests[] = {
    {"select_shape", test_select_shape, IT_ALL, false},
    {"exec_shape", test_exec_shape, IT_ALL, false},
    {"returning", test_returning, IT_ALL, false},
    {"multi_statement", test_multi_statement, IT_ALL, false},
    {"errors", test_errors, IT_ALL, false},
    {"two_connections", test_two_connections, IT_ALL, false},
    {"count", test_count, IT_ALL, false},
    {"row_limit", test_row_limit, IT_SQLITE | IT_POSTGRES | IT_MYSQL, true},
    {"row_limit_multi", test_row_limit_multi, IT_SQLITE | IT_POSTGRES | IT_MYSQL, true},
    {"row_limit_write", test_row_limit_write, IT_SQLITE | IT_POSTGRES | IT_MYSQL, true},
    {"lace_socket_refused", test_lace_socket_refused, IT_ALL, false},
    {NULL, NULL, 0, false},
};
