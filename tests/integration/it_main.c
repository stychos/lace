/*
 * Lace
 * Integration test runner: one private daemon per database, every test in
 * every table on each database it applies to.
 *
 *   test_integration [filter...]   run tests whose "db/name" contains a filter
 *
 * Databases come from the environment (run.py sets them):
 *   LACE_IT_LACED     laced binary
 *   LACE_IT_TRANSPORT "socket" (a daemon run.py started at LACE_IT_SOCKET)
 *                     or "stdio" (a private daemon per database; default)
 *   LACE_IT_SQLITE    LACE_IT_POSTGRES  LACE_IT_MARIADB  LACE_IT_MYSQL
 *   LACE_IT_SSH_POSTGRES, LACE_IT_SSH_MARIADB (through an SSH tunnel)
 *   LACE_IT_SLOW=1    also run slow tests
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_test_checks = 0;
int g_test_failures = 0;

/* ---- Helpers ------------------------------------------------------------- */

static char *vformat(const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  char *s = malloc((size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  return s;
}

static LaceResult *run_sql(ItDb *db, const char *sql, int *rc_out) {
  LaceResult *r = NULL;
  int rc = lace_exec(db->client, db->conn, sql, &r);
  if (rc_out)
    *rc_out = rc;
  return rc == LACE_OK ? r : (lace_result_free(r), NULL);
}

LaceResult *it_q(ItDb *db, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *sql = vformat(fmt, ap);
  va_end(ap);
  int rc = 0;
  LaceResult *r = run_sql(db, sql, &rc);
  if (!r) {
    fprintf(stderr, "  [%s] SQL failed (%d): %s\n    %s\n", db->name, rc,
            lace_client_error(db->client), sql);
  }
  CHECK(r != NULL);
  free(sql);
  return r;
}

bool it_ex(ItDb *db, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *sql = vformat(fmt, ap);
  va_end(ap);
  LaceResult *r = it_q(db, "%s", sql);
  free(sql);
  lace_result_free(r);
  return r != NULL;
}

int it_fail(ItDb *db, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *sql = vformat(fmt, ap);
  va_end(ap);
  int rc = 0;
  lace_result_free(run_sql(db, sql, &rc));
  free(sql);
  return rc;
}

const char *it_dialect(const ItDb *db, const char *sqlite, const char *pg,
                       const char *mysql) {
  switch (db->driver) {
  case LACE_DRIVER_SQLITE:
    return sqlite;
  case LACE_DRIVER_POSTGRES:
    return pg;
  default:
    return mysql;
  }
}

char *it_text(const LaceResult *r, size_t row, size_t col) {
  if (!r || !r->rows || row >= r->num_rows || col >= r->rows[row].num_cells)
    return NULL;
  const LaceValue *v = &r->rows[row].cells[col];
  return v->is_null ? NULL : lace_value_to_edit_text(v);
}

int64_t it_int(const LaceResult *r, size_t row, size_t col) {
  char *t = it_text(r, row, col);
  int64_t v = t ? strtoll(t, NULL, 10) : 0;
  free(t);
  return v;
}

int64_t it_scalar(ItDb *db, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *sql = vformat(fmt, ap);
  va_end(ap);
  LaceResult *r = it_q(db, "%s", sql);
  free(sql);
  int64_t v = r && r->num_rows > 0 ? it_int(r, 0, 0) : -1;
  lace_result_free(r);
  return v;
}

int it_connect_again(ItDb *db) {
  int id = 0;
  if (lace_connect(db->client, db->url, NULL, &id) != LACE_OK) {
    fprintf(stderr, "  [%s] second connection failed: %s\n", db->name,
            lace_client_error(db->client));
    return 0;
  }
  return id;
}

int64_t it_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Runner -------------------------------------------------------------- */

static const ItTest *const g_tables[] = {
    it_basic_tests,    it_values_tests, it_schema_tests,   it_mutation_tests,
    it_keyset_tests,   it_tx_tests,     it_recovery_tests, it_paging_tests,
    it_tunnel_tests,   it_daemon_tests,   it_catalog_tests,
    it_ddl_tests,
};

typedef struct {
  const char *env;
  const char *name;
  unsigned kind;
  LaceDriver driver;
  bool mariadb;
} DbSpec;

static const DbSpec g_dbs[] = {
    {"LACE_IT_SQLITE", "sqlite", IT_SQLITE, LACE_DRIVER_SQLITE, false},
    {"LACE_IT_POSTGRES", "postgres", IT_POSTGRES, LACE_DRIVER_POSTGRES, false},
    {"LACE_IT_MARIADB", "mariadb", IT_MYSQL, LACE_DRIVER_MARIADB, true},
    {"LACE_IT_MYSQL", "mysql", IT_MYSQL, LACE_DRIVER_MYSQL, false},
    {"LACE_IT_SSH_POSTGRES", "tunnel", IT_TUNNEL, LACE_DRIVER_POSTGRES, false},
    /* MariaDB through the same jump host: every MySQL-family test */
    {"LACE_IT_SSH_MARIADB", "tunnel-mariadb", IT_MYSQL, LACE_DRIVER_MARIADB, true},
};

static bool selected(const char *label, int nfilters, char **filters) {
  if (nfilters == 0)
    return true;
  for (int i = 0; i < nfilters; i++) {
    if (strstr(label, filters[i]))
      return true;
  }
  return false;
}

int main(int argc, char **argv) {
  const char *laced = getenv("LACE_IT_LACED");
  const char *socket_path = getenv("LACE_IT_SOCKET");
  const char *transport = getenv("LACE_IT_TRANSPORT");
  bool stdio = !transport || strcmp(transport, "socket") != 0;
  if (!stdio && (!socket_path || !*socket_path)) {
    fprintf(stderr, "LACE_IT_TRANSPORT=socket needs LACE_IT_SOCKET\n");
    return 2;
  }
  bool slow = getenv("LACE_IT_SLOW") && strcmp(getenv("LACE_IT_SLOW"), "1") == 0;
  int ran = 0, failed = 0, skipped_dbs = 0;

  for (size_t d = 0; d < sizeof(g_dbs) / sizeof(g_dbs[0]); d++) {
    const char *url = getenv(g_dbs[d].env);
    if (!url || !*url) {
      printf("-- %s: skipped (%s not set)\n", g_dbs[d].name, g_dbs[d].env);
      skipped_dbs++;
      continue;
    }
    ItDb db = {0};
    db.name = g_dbs[d].name;
    db.url = url;
    db.kind = g_dbs[d].kind;
    db.driver = g_dbs[d].driver;
    db.mariadb = g_dbs[d].mariadb;
    db.stdio = stdio;

    /* A private daemon: over pipes, or the socket daemon run.py started;
     * never the user's own daemon */
    LaceClientConfig cfg = {0};
    if (stdio) {
      cfg.mode = LACE_CONN_AUTO;
      cfg.spawn_mode = LACE_SPAWN_STDIO;
      cfg.daemon_path = laced;
    } else {
      cfg.mode = LACE_CONN_UNIX;
      cfg.socket_path = socket_path;
    }
    db.client = lace_client_create_with_config(&cfg);
    if (!lace_client_connected(db.client) ||
        lace_connect(db.client, url, NULL, &db.conn) != LACE_OK) {
      printf("FAIL %s: cannot connect: %s\n", db.name,
             lace_client_error(db.client));
      g_test_failures++;
      failed++;
      lace_client_destroy(db.client);
      continue;
    }
    printf("-- %s (%s)\n", db.name, stdio ? "stdio" : "socket");

    for (size_t t = 0; t < sizeof(g_tables) / sizeof(g_tables[0]); t++) {
      for (const ItTest *test = g_tables[t]; test->name; test++) {
        if (!(test->kinds & db.kind) || (test->slow && !slow) ||
            ((test->kinds & IT_SOCKET_ONLY) && db.stdio))
          continue;
        char label[128];
        snprintf(label, sizeof(label), "%s/%s", db.name, test->name);
        if (!selected(label, argc - 1, argv + 1))
          continue;
        int before = g_test_failures;
        int64_t t0 = it_now_ms();
        test->fn(&db);
        int64_t ms = it_now_ms() - t0;
        bool ok = g_test_failures == before;
        printf("%s %s (%lld ms)\n", ok ? "  ok  " : "  FAIL", label,
               (long long)ms);
        fflush(stdout);
        ran++;
        failed += !ok;
      }
    }
    lace_client_destroy(db.client);
  }

  printf("integration: %d tests, %d failed, %d checks, %d databases skipped\n",
         ran, failed, g_test_checks, skipped_dbs);
  return failed ? 1 : 0;
}
