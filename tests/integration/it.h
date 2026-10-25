/*
 * Lace
 * Integration test harness: tests run against real databases through a
 * private laced (LACE_IT_TRANSPORT: "socket" at LACE_IT_SOCKET, or
 * "stdio"), one connection per database named in LACE_IT_*
 * (see run.py). A test that needs a feature some databases lack says so in
 * its driver mask; databases without a URL are skipped.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_IT_H
#define LACE_IT_H

#define TEST_COUNTERS_EXTERN
#include "../test.h"
#include "lace.h"
#include "util/str.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* Which databases a test runs on */
#define IT_SQLITE 0x01u
#define IT_POSTGRES 0x02u
#define IT_MYSQL 0x04u   /* MySQL and MariaDB */
#define IT_TUNNEL 0x08u  /* PostgreSQL through an SSH tunnel */
#define IT_NET (IT_POSTGRES | IT_MYSQL | IT_TUNNEL)
#define IT_ALL (IT_SQLITE | IT_NET)
#define IT_SOCKET_ONLY 0x100u /* Not in the stdio pass (runs once per run) */

typedef struct {
  const char *name;  /* "sqlite", "postgres", "mariadb", "mysql", "tunnel" */
  const char *url;
  unsigned kind;     /* One IT_* bit */
  LaceDriver driver; /* SQL dialect */
  bool mariadb;      /* MySQL family: MariaDB rather than MySQL */
  bool stdio;        /* Through a stdio daemon (calls take turns on one
                        link) rather than a socket daemon */
  lace_client_t *client;
  int conn;          /* The connection tests use */
} ItDb;

typedef void (*ItFn)(ItDb *db);

typedef struct {
  const char *name;
  ItFn fn;
  unsigned kinds; /* IT_* mask */
  bool slow;      /* Only with LACE_IT_SLOW=1 (run.py --slow) */
} ItTest;

/* Each it_*.c defines one NULL-terminated table */
extern const ItTest it_basic_tests[];
extern const ItTest it_values_tests[];
extern const ItTest it_schema_tests[];
extern const ItTest it_mutation_tests[];
extern const ItTest it_keyset_tests[];
extern const ItTest it_tx_tests[];
extern const ItTest it_recovery_tests[];
extern const ItTest it_daemon_tests[];
extern const ItTest it_tunnel_tests[];
extern const ItTest it_paging_tests[];
extern const ItTest it_catalog_tests[];
extern const ItTest it_ddl_tests[];

/* ---- Helpers (it_main.c) ------------------------------------------------ */

/* Run SQL (printf format); a failure is a test failure (printed with the
 * SQL and error) and returns NULL. Free the result with lace_result_free. */
LaceResult *it_q(ItDb *db, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* it_q and free the result; true on success */
bool it_ex(ItDb *db, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Run SQL expecting it to fail: returns the error code (0 if it worked) */
int it_fail(ItDb *db, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* The SQL text for this database's dialect */
const char *it_dialect(const ItDb *db, const char *sqlite, const char *pg,
                       const char *mysql);

/* Cell (row, col) as display text (NULL stays NULL); caller frees */
char *it_text(const LaceResult *r, size_t row, size_t col);

/* Cell as an integer (0 for NULL or a missing cell) */
int64_t it_int(const LaceResult *r, size_t row, size_t col);

/* Run SQL returning a single integer (first cell); -1 on failure */
int64_t it_scalar(ItDb *db, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A second connection to the same database (0 on failure) */
int it_connect_again(ItDb *db);

/* Milliseconds since an arbitrary point */
int64_t it_now_ms(void);

/* CHECK that also prints the database's last error when it fails */
#define CHECK_OK(db, expr)                                                     \
  do {                                                                         \
    int rc_ = (expr);                                                          \
    if (rc_ != LACE_OK)                                                        \
      fprintf(stderr, "  [%s] %s -> %d: %s\n", (db)->name, #expr, rc_,         \
              lace_client_error((db)->client));                                \
    CHECK(rc_ == LACE_OK);                                                     \
  } while (0)

#undef CHECK_STR
#define CHECK_STR(got, want)                                                   \
  do {                                                                         \
    const char *g_ = (got), *w_ = (want);                                      \
    bool same_ = (!g_ || !w_) ? g_ == w_ : strcmp(g_, w_) == 0;                \
    if (!same_)                                                                \
      fprintf(stderr, "  got:  %s\n  want: %s\n", g_ ? g_ : "(null)",          \
              w_ ? w_ : "(null)");                                             \
    CHECK(same_);                                                              \
  } while (0)

/* ---- Processes and raw protocol (it_proc.c) ------------------------------ */

#include <cjson/cJSON.h>
#include <sys/types.h>

/* The TUI's driver name for this database ("sqlite", "postgres", ...) */
const char *it_driver_name(const ItDb *db);

/* A new private directory (short enough for socket paths); free it, and
 * remove it with it_rmdir */
char *it_tmpdir(void);
void it_rmdir(const char *dir);

/* Start $LACE_IT_LACED listening on socket_path (NULL: options only), with
 * extra options (NULL-terminated); returns once the socket exists, or -1 */
pid_t it_laced_start(const char *socket_path, const char *const *extra);
bool it_laced_exited(pid_t pid, int timeout_ms); /* Reaped within timeout */
void it_laced_stop(pid_t pid);                   /* SIGTERM, then SIGKILL */

/* A liblace client of the daemon at socket_path */
lace_client_t *it_client_at(const char *socket_path);

/* A raw client: one JSON object per line, no liblace in between */
typedef struct ItRaw ItRaw;
ItRaw *it_raw_open(const char *socket_path, const char *client_id);
ItRaw *it_raw_open_tcp(int port); /* No hello: the test does the handshake */
void it_raw_close(ItRaw *r);
int it_raw_fd(const ItRaw *r);
int it_raw_send(ItRaw *r, const char *method, const char *params_json); /* id */
cJSON *it_raw_read(ItRaw *r, int timeout_ms); /* Next line, NULL on timeout/EOF */
cJSON *it_raw_call(ItRaw *r, const char *method, const char *params_json,
                   int timeout_ms); /* The reply to this request */
bool it_raw_eof(ItRaw *r, int timeout_ms); /* The daemon closed it in time */
int it_raw_error_code(const cJSON *reply); /* 0 if not an error */

#endif /* LACE_IT_H */
