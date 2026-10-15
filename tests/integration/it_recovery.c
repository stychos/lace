/*
 * Lace
 * Integration tests: cancelling, busy connections, and connections the
 * server drops (with and without an open transaction)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  ItDb *db;
  const char *sql;
  int rc;
} LongQuery;

static void *run_long(void *p) {
  LongQuery *q = p;
  LaceResult *r = NULL;
  q->rc = lace_exec(q->db->client, q->db->conn, q->sql, &r);
  lace_result_free(r);
  return NULL;
}

/* A statement that runs until it is stopped. MySQL interrupts SLEEP() and
 * BENCHMARK() by making them return normally (indistinguishable from
 * finishing); a large join is aborted with an error. */
static const char *long_sql(const ItDb *db) {
  return it_dialect(db,
                    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) "
                    "SELECT COUNT(*) FROM c",
                    "SELECT pg_sleep(60)",
                    "SELECT COUNT(*) FROM information_schema.columns a, "
                    "information_schema.columns b, information_schema.columns c");
}

/* A running statement makes its connection busy, and a cancel stops it
 * quickly; the connection is usable afterwards */
static void test_cancel(ItDb *db) {
  LongQuery q = {db, long_sql(db), 0};
  pthread_t th;
  int64_t t0 = it_now_ms();
  pthread_create(&th, NULL, run_long, &q);
  usleep(700 * 1000);

  /* One statement at a time per connection (over stdio a second call
   * waits for the link instead) */
  if (!db->stdio) {
    LaceResult *r = NULL;
    CHECK(lace_exec(db->client, db->conn, "SELECT 1", &r) ==
          LACE_ERR_CONNECTION_BUSY);
    lace_result_free(r);
  }

  CHECK_OK(db, lace_cancel_query(db->client, db->conn));
  pthread_join(th, NULL);
  int64_t ms = it_now_ms() - t0;
  if (q.rc != LACE_ERR_QUERY_CANCELLED)
    fprintf(stderr, "  [%s] cancelled statement returned %d: %s\n", db->name,
            q.rc, lace_client_error(db->client));
  CHECK(q.rc == LACE_ERR_QUERY_CANCELLED);
  CHECK(ms < 10000);
  CHECK(it_scalar(db, "SELECT 7") == 7);

  /* A cancel with nothing running is harmless */
  CHECK_OK(db, lace_cancel_query(db->client, db->conn));
  CHECK(it_scalar(db, "SELECT 8") == 8);
}

/* Server-side id of the test connection, and the SQL that kills it */
static int64_t backend_id(ItDb *db) {
  return it_scalar(db, "%s", it_dialect(db, "", "SELECT pg_backend_pid()",
                                        "SELECT CONNECTION_ID()"));
}

static void kill_backend(ItDb *db, int64_t id) {
  int other = it_connect_again(db);
  CHECK(other > 0);
  if (other <= 0)
    return;
  char *sql = str_printf(db->driver == LACE_DRIVER_POSTGRES
                             ? "SELECT pg_terminate_backend(%lld)"
                             : "KILL %lld",
                         (long long)id);
  LaceResult *r = NULL;
  CHECK_OK(db, lace_exec(db->client, other, sql, &r));
  lace_result_free(r);
  free(sql);
  lace_disconnect(db->client, other);
  usleep(300 * 1000); /* Let the server finish closing it */
}

/* Lost with no transaction open: the statement that finds it fails (nobody
 * knows whether it ran), the next one reconnects */
static void test_lost_connection(ItDb *db) {
  int64_t before = backend_id(db);
  kill_backend(db, before);
  CHECK(it_fail(db, "SELECT 1") != LACE_OK);
  CHECK(it_scalar(db, "SELECT 2") == 2);
  CHECK(backend_id(db) != before);
}

/* Lost with a transaction open: its work is gone, so statements are refused
 * until ROLLBACK acknowledges it; nothing runs in autocommit meanwhile */
static void test_lost_transaction(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_lost");
  it_ex(db, "CREATE TABLE it_lost (id INTEGER PRIMARY KEY)");
  it_ex(db, "BEGIN");
  it_ex(db, "INSERT INTO it_lost VALUES (1)");
  kill_backend(db, backend_id(db));

  CHECK(it_fail(db, "INSERT INTO it_lost VALUES (2)") != LACE_OK);
  CHECK(it_fail(db, "INSERT INTO it_lost VALUES (3)") == LACE_ERR_CONNECTION_LOST);
  CHECK(strstr(lace_client_error(db->client), "ROLLBACK") != NULL);
  CHECK(lace_last_transaction_state() == 1); /* Still reported open */

  LaceResult *r = it_q(db, "ROLLBACK");
  CHECK(r && !r->in_transaction);
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_lost") == 0);
  it_ex(db, "DROP TABLE it_lost");
}

const ItTest it_recovery_tests[] = {
    {"cancel", test_cancel, IT_ALL, false},
    {"lost_connection", test_lost_connection, IT_NET, false},
    {"lost_transaction", test_lost_transaction, IT_NET, false},
    {NULL, NULL, 0, false},
};
