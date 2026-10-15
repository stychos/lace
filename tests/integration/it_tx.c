/*
 * Lace
 * Integration tests: transactions and the state replies carry
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdlib.h>
#include <string.h>

/* Every reply says whether a transaction is open */
static void test_tx_state(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_tx");
  it_ex(db, "CREATE TABLE it_tx (id INTEGER PRIMARY KEY)");

  LaceResult *r = it_q(db, "BEGIN");
  CHECK(r && r->in_transaction);
  lace_result_free(r);
  r = it_q(db, "INSERT INTO it_tx VALUES (1)");
  CHECK(r && r->in_transaction);
  lace_result_free(r);
  r = it_q(db, "ROLLBACK");
  CHECK(r && !r->in_transaction);
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_tx") == 0);

  it_ex(db, "BEGIN");
  it_ex(db, "INSERT INTO it_tx VALUES (2)");
  r = it_q(db, "COMMIT");
  CHECK(r && !r->in_transaction);
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_tx") == 1);

  /* A transaction belongs to its connection */
  int other = it_connect_again(db);
  if (other > 0) {
    it_ex(db, "BEGIN");
    it_ex(db, "INSERT INTO it_tx VALUES (3)");
    LaceResult *o = NULL;
    CHECK_OK(db, lace_exec(db->client, other, "SELECT COUNT(*) FROM it_tx", &o));
    CHECK(o && !o->in_transaction);
    lace_result_free(o);
    it_ex(db, "ROLLBACK");
    lace_disconnect(db->client, other);
  }
  it_ex(db, "DROP TABLE it_tx");
}

/* PostgreSQL: after an error only ROLLBACK works, and a COMMIT that the
 * server turns into a rollback is an error, not a success */
static void test_pg_failed_tx(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_txf");
  it_ex(db, "CREATE TABLE it_txf (id INTEGER PRIMARY KEY)");
  it_ex(db, "BEGIN");
  it_ex(db, "INSERT INTO it_txf VALUES (1)");
  CHECK(it_fail(db, "INSERT INTO it_txf VALUES (1)") == LACE_ERR_QUERY_FAILED);
  CHECK(lace_last_transaction_state() == 2); /* In a failed transaction */
  CHECK(it_fail(db, "SELECT 1") != LACE_OK);
  int rc = it_fail(db, "COMMIT");
  CHECK(rc != LACE_OK);
  CHECK(strstr(lace_client_error(db->client), "did not commit") != NULL);
  CHECK(lace_last_transaction_state() == 0);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_txf") == 0);
  it_ex(db, "DROP TABLE it_txf");
}

const ItTest it_tx_tests[] = {
    {"tx_state", test_tx_state, IT_ALL, false},
    {"pg_failed_tx", test_pg_failed_tx, IT_POSTGRES | IT_TUNNEL, false},
    {NULL, NULL, 0, false},
};
