/*
 * Lace
 * liblace under fire: worker threads query their own databases through one
 * client while the daemon is killed, respawned, or swapped between a shared
 * and a private one; a cancel thread interrupts them at random. Built with
 * ThreadSanitizer by tests/stress/run.sh.
 *
 *   stress <scenario> <laced> <workdir> <seconds>
 *   scenario: unix    shared Unix-socket daemon, killed again and again
 *             detect  detect mode: private daemon killed, shared one
 *                     started and stopped in turns
 *             stdio   one private daemon, no kills: the single link and
 *                     cancels sent as notifications
 *
 * Every reply must come from the asking thread's own database (each holds
 * its thread number) and echo the thread's counter; a call may fail, but
 * never with someone else's answer, and no thread may stall. Exit 0 on
 * success, 1 on a wrong answer or a stall.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "lace.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 6
#define STALL_S 45

static lace_client_t *g_client;
static const char *g_workdir;
static _Atomic bool g_stop;
static _Atomic bool g_settling; /* Disruption over: everyone must recover */
static _Atomic int g_conn[WORKERS];
static _Atomic long g_progress[WORKERS]; /* Last finished call, seconds */
static _Atomic long g_ok[WORKERS], g_ok_settled[WORKERS];
static _Atomic long g_lost, g_cancelled, g_other, g_reconnects, g_disruptions;
static _Atomic int g_last_other_code;
static _Atomic int g_finished; /* Threads that left their loop */

static long now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec;
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "stress: FAIL: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  fflush(stderr);
  _exit(1);
}

static char *db_url(int i) {
  char *url = malloc(512);
  snprintf(url, 512, "sqlite://%s/w%d.sqlite", g_workdir, i);
  return url;
}

/* Each worker: its own database, its own counter in every statement */
static void *worker(void *arg) {
  int i = (int)(intptr_t)arg;
  char *url = db_url(i);
  int conn = 0;
  long counter = 0;
  unsigned seed = (unsigned)i * 7919u + 1u;
  while (!g_stop) {
    if (!conn) {
      int c = 0;
      int rc = lace_connect(g_client, url, NULL, &c);
      g_progress[i] = now_s();
      if (rc != LACE_OK) {
        sleep_ms(20);
        continue;
      }
      conn = c;
      g_conn[i] = c;
      lace_expect_generation(g_client, lace_client_generation(g_client));
      g_reconnects++;
    }
    counter++;
    char sql[256];
    if (rand_r(&seed) % 8 == 0) {
      /* A slower statement, for the cancel thread to hit */
      snprintf(sql, sizeof(sql),
               "WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s "
               "WHERE n < 300000) SELECT (SELECT v FROM marker), %ld + 0 * "
               "(SELECT COUNT(*) FROM s)",
               counter);
    } else {
      snprintf(sql, sizeof(sql), "SELECT v, %ld FROM marker", counter);
    }
    LaceResult *r = NULL;
    int rc = lace_exec(g_client, conn, sql, &r);
    g_progress[i] = now_s();
    if (rc == LACE_OK) {
      if (!r || r->num_rows != 1 || !r->rows || r->rows[0].num_cells != 2)
        fail("worker %d: malformed reply to call %ld", i, counter);
      long v = (long)r->rows[0].cells[0].int_val;
      long echo = (long)r->rows[0].cells[1].int_val;
      if (v != i)
        fail("worker %d: answer came from worker %ld's database (call %ld)", i, v,
             counter);
      if (echo != counter)
        fail("worker %d: answer to call %ld carries counter %ld", i, counter, echo);
      g_ok[i]++;
      if (g_settling)
        g_ok_settled[i]++;
    } else if (rc == LACE_ERR_QUERY_CANCELLED) {
      g_cancelled++;
    } else if (rc == LACE_ERR_CONNECTION_LOST || rc == LACE_ERR_INVALID_CONN_ID ||
               rc == LACE_ERR_CONNECTION_CLOSED || rc == LACE_ERR_TIMEOUT ||
               rc == LACE_ERR_PIPE_ERROR) {
      g_lost++;
      conn = 0; /* Open it again (on whatever daemon there is now) */
      g_conn[i] = 0;
    } else {
      g_other++;
      g_last_other_code = rc;
      conn = 0;
      g_conn[i] = 0;
    }
    lace_result_free(r);
  }
  free(url);
  g_finished++;
  return NULL;
}

/* Cancels at random, as Esc does in the TUI */
static void *canceller(void *arg) {
  (void)arg;
  unsigned seed = 4242;
  while (!g_stop) {
    int conn = g_conn[rand_r(&seed) % WORKERS];
    if (conn) {
      lace_expect_generation(g_client, lace_client_generation(g_client));
      (void)lace_cancel_query(g_client, conn);
    }
    sleep_ms(5 + (int)(rand_r(&seed) % 40));
  }
  g_finished++;
  return NULL;
}

/* pids of laced processes matching a pgrep expression */
static int find_pids(const char *args, pid_t *out, int max) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "pgrep %s 2>/dev/null", args);
  FILE *p = popen(cmd, "r");
  int n = 0;
  long pid;
  while (p && n < max && fscanf(p, "%ld", &pid) == 1)
    out[n++] = (pid_t)pid;
  if (p)
    pclose(p);
  return n;
}

static char g_sock[512];
static char g_laced[1024];

static void kill_socket_daemons(int sig) {
  char args[700];
  snprintf(args, sizeof(args), "-f 'laced --unix %s'", g_sock);
  pid_t pids[16];
  int n = find_pids(args, pids, 16);
  for (int k = 0; k < n; k++)
    kill(pids[k], sig);
}

static void kill_private_daemons(void) {
  char args[64];
  snprintf(args, sizeof(args), "-P %ld laced", (long)getpid());
  pid_t pids[16];
  int n = find_pids(args, pids, 16);
  for (int k = 0; k < n; k++)
    kill(pids[k], SIGKILL);
}

static pid_t start_shared_daemon(void) {
  pid_t pid = fork();
  if (pid == 0) {
    execl(g_laced, g_laced, "--unix", g_sock, "--quiet", (char *)NULL);
    _exit(127);
  }
  for (int k = 0; k < 100 && access(g_sock, F_OK) != 0; k++)
    sleep_ms(30);
  return pid;
}

static void setup_databases(void) {
  for (int i = 0; i < WORKERS; i++) {
    char *url = db_url(i);
    char *create = malloc(strlen(url) + 16);
    sprintf(create, "%s?create=1", url);
    int conn = 0;
    if (lace_connect(g_client, create, NULL, &conn) != LACE_OK)
      fail("setup: cannot create %s: %s", url, lace_client_error(g_client));
    char sql[128];
    snprintf(sql, sizeof(sql),
             "DROP TABLE IF EXISTS marker; CREATE TABLE marker (v INTEGER); "
             "INSERT INTO marker VALUES (%d)",
             i);
    if (lace_exec(g_client, conn, sql, NULL) != LACE_OK)
      fail("setup: %s", lace_client_error(g_client));
    lace_disconnect(g_client, conn);
    free(create);
    free(url);
  }
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: stress unix|detect|stdio <laced> <workdir> <seconds>\n");
    return 2;
  }
  const char *scenario = argv[1];
  snprintf(g_laced, sizeof(g_laced), "%s", argv[2]);
  g_workdir = argv[3];
  int seconds = atoi(argv[4]);
  bool unix_mode = strcmp(scenario, "unix") == 0;
  bool detect = strcmp(scenario, "detect") == 0;
  bool stdio_mode = strcmp(scenario, "stdio") == 0;
  if (!unix_mode && !detect && !stdio_mode)
    return 2;

  /* Everything (default socket, logs) inside the work directory */
  setenv("HOME", g_workdir, 1);
  setenv("XDG_RUNTIME_DIR", "", 1);
  char *sock = lace_get_default_socket_path();
  snprintf(g_sock, sizeof(g_sock), "%s", sock ? sock : "");
  free(sock);
  signal(SIGPIPE, SIG_IGN);

  LaceClientConfig cfg = {0};
  cfg.mode = LACE_CONN_AUTO;
  cfg.daemon_path = g_laced;
  cfg.spawn_mode = unix_mode ? LACE_SPAWN_UNIX : detect ? LACE_SPAWN_DETECT
                                                         : LACE_SPAWN_STDIO;
  g_client = lace_client_create_with_config(&cfg);
  if (!lace_client_connected(g_client))
    fail("no daemon: %s", lace_client_error(g_client));
  lace_set_timeout(g_client, 10000);
  lace_set_query_timeout(g_client, 20000);
  setup_databases();

  pthread_t threads[WORKERS], cancel_thread;
  long start = now_s();
  for (int i = 0; i < WORKERS; i++) {
    g_progress[i] = start;
    pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
  }
  pthread_create(&cancel_thread, NULL, canceller, NULL);

  /* Disrupt, watching for stalls */
  pid_t shared = 0;
  unsigned seed = 99;
  long settle_at = start + seconds;
  long end_at = settle_at + 8;
  int step = 0;
  while (now_s() < end_at) {
    sleep_ms(400 + (int)(rand_r(&seed) % 600));
    long now = now_s();
    for (int i = 0; i < WORKERS; i++) {
      if (now - g_progress[i] > STALL_S)
        fail("worker %d stalled for %lds (scenario %s)", i, now - g_progress[i],
             scenario);
    }
    if (now >= settle_at) {
      if (!g_settling) {
        g_settling = true;
        /* Detect: leave a shared daemon running or not, either is fine */
      }
      continue;
    }
    if (unix_mode) {
      kill_socket_daemons(SIGKILL); /* liblace respawns it */
      g_disruptions++;
    } else if (detect) {
      switch (step++ % 3) {
      case 0: /* The private daemon dies: re-detect */
        kill_private_daemons();
        break;
      case 1: /* A shared daemon appears (used at the next loss) */
        if (!shared)
          shared = start_shared_daemon();
        kill_private_daemons();
        break;
      case 2: /* The shared daemon dies */
        if (shared) {
          kill(shared, SIGKILL);
          waitpid(shared, NULL, 0);
          unlink(g_sock);
          shared = 0;
        }
        break;
      }
      g_disruptions++;
    }
  }
  g_stop = true;
  /* Stopping must not hang either: every thread leaves within the stall
   * limit (a call stuck for good shows up here) */
  for (long deadline = now_s() + STALL_S; g_finished < WORKERS + 1;) {
    if (now_s() > deadline) {
      for (int i = 0; i < WORKERS; i++)
        fprintf(stderr, "worker %d: last progress %lds ago\n", i,
                now_s() - g_progress[i]);
      fail("%d of %d threads did not stop within %ds (scenario %s)",
           WORKERS + 1 - (int)g_finished, WORKERS + 1, STALL_S, scenario);
    }
    sleep_ms(100);
  }
  for (int i = 0; i < WORKERS; i++)
    pthread_join(threads[i], NULL);
  pthread_join(cancel_thread, NULL);

  long total = 0;
  for (int i = 0; i < WORKERS; i++) {
    total += g_ok[i];
    if (g_ok_settled[i] == 0)
      fail("worker %d never recovered after the disruptions ended (scenario %s)", i,
           scenario);
  }
  printf("stress %-6s ok: %ld calls, %ld lost, %ld cancelled, %ld other (last code "
         "%d), %ld reconnects, %ld disruptions\n",
         scenario, total, (long)g_lost, (long)g_cancelled, (long)g_other,
         (int)g_last_other_code, (long)g_reconnects, (long)g_disruptions);
  lace_client_destroy(g_client);
  if (shared) {
    kill(shared, SIGTERM);
    waitpid(shared, NULL, 0);
  }
  kill_socket_daemons(SIGTERM);
  return 0;
}
