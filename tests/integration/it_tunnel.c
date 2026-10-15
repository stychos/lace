/*
 * Lace
 * Integration tests: an SSH tunnel that dies under a connection is
 * restarted on the same local port before the next statement
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The ssh jump host's port in a postgres+ssh:// URL ("...@host:PORT/...") */
static int ssh_port(const ItDb *db) {
  const char *at = strchr(db->url, '@');
  const char *colon = at ? strchr(at, ':') : NULL;
  return colon ? atoi(colon + 1) : 0;
}

/* Kill this run's tunnel: its ssh forwards a local socket to postgres:5432
 * through the jump host's port ("ssh -N -L <socket>:postgres:5432 ... -p N") */
static bool kill_tunnel(const ItDb *db) {
  if (system("command -v pkill >/dev/null 2>&1") != 0)
    return false;
  char *cmd = str_printf("pkill -f 'ssh -N -L [^ ]+:postgres:5432 .*-p %d'",
                         ssh_port(db));
  int rc = system(cmd);
  free(cmd);
  usleep(500 * 1000);
  return rc == 0;
}

static int64_t backend_pid(ItDb *db) {
  return it_scalar(db, "SELECT pg_backend_pid()");
}

/* No transaction open: the next statements run through a new tunnel and a
 * new server session (at most the first one fails) */
static void test_tunnel_restart(ItDb *db) {
  int64_t before = backend_pid(db);
  if (!kill_tunnel(db)) {
    fprintf(stderr, "  [%s] could not find the tunnel's ssh (no pkill?)\n", db->name);
    CHECK(false);
    return;
  }
  int failures = 0;
  int64_t after = -1;
  for (int i = 0; i < 3 && after < 0; i++) {
    LaceResult *r = NULL;
    if (lace_exec(db->client, db->conn, "SELECT pg_backend_pid()", &r) == LACE_OK)
      after = it_int(r, 0, 0);
    else
      failures++;
    lace_result_free(r);
  }
  CHECK(failures <= 1);
  CHECK(after > 0 && after != before);
}

/* A transaction open when the tunnel died is gone: refused until ROLLBACK */
static void test_tunnel_restart_tx(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_tun");
  it_ex(db, "CREATE TABLE it_tun (id INTEGER PRIMARY KEY)");
  it_ex(db, "BEGIN");
  it_ex(db, "INSERT INTO it_tun VALUES (1)");
  CHECK(kill_tunnel(db));
  CHECK(it_fail(db, "INSERT INTO it_tun VALUES (2)") == LACE_ERR_CONNECTION_LOST);
  CHECK(strstr(lace_client_error(db->client), "ROLLBACK") != NULL);
  LaceResult *r = it_q(db, "ROLLBACK");
  CHECK(r && !r->in_transaction);
  lace_result_free(r);
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_tun") == 0);
  it_ex(db, "DROP TABLE it_tun");
}

/* The pg alias through a tunnel (its socket must be named for libpq) */
static void test_tunnel_pg_alias(ItDb *db) {
  if (strncmp(db->url, "postgres+ssh://", 15) != 0)
    return;
  char *url = str_printf("pg+ssh://%s", db->url + 15);
  int conn = 0;
  CHECK_OK(db, lace_connect(db->client, url, NULL, &conn));
  LaceResult *r = NULL;
  CHECK(lace_exec(db->client, conn, "SELECT 1", &r) == LACE_OK && r && r->num_rows == 1);
  lace_result_free(r);
  lace_disconnect(db->client, conn);
  free(url);
}

/* Key authentication: the ssh_key option (a key the server accepts, no
 * password anywhere); a key file that is not there is reported clearly */
static void test_tunnel_ssh_key(ItDb *db) {
  const char *url = getenv("LACE_IT_SSH_KEY_POSTGRES");
  if (!url || strncmp(db->url, "postgres+ssh://", 15) != 0)
    return; /* Set by run.py when it could make a key */
  int conn = 0;
  CHECK_OK(db, lace_connect(db->client, url, NULL, &conn));
  LaceResult *r = NULL;
  CHECK(lace_exec(db->client, conn, "SELECT 1", &r) == LACE_OK && r && r->num_rows == 1);
  lace_result_free(r);
  lace_disconnect(db->client, conn);

  char *missing = str_dup(url);
  char *q = strstr(missing, "?ssh_key=");
  if (q) {
    *q = '\0';
    char *bad = str_printf("%s?ssh_key=%%2Fnonexistent%%2Fid_none", missing);
    CHECK(lace_connect(db->client, bad, NULL, &conn) != LACE_OK);
    CHECK(strstr(lace_client_error(db->client), "SSH key") != NULL);
    free(bad);
  }
  free(missing);
}

const ItTest it_tunnel_tests[] = {
    {"tunnel_restart", test_tunnel_restart, IT_TUNNEL, false},
    {"tunnel_restart_tx", test_tunnel_restart_tx, IT_TUNNEL, false},
    {"tunnel_pg_alias", test_tunnel_pg_alias, IT_TUNNEL, false},
    {"tunnel_ssh_key", test_tunnel_ssh_key, IT_TUNNEL, false},
    {NULL, NULL, 0, false},
};
