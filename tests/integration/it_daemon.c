/*
 * Lace
 * Integration tests: the daemon itself. Each test starts its own laced in
 * a private directory, so they run once (on the SQLite pass of the socket
 * transport) and never touch the daemon the other tests use.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <util/sha256.h>

/* A private directory and daemon for one test */
typedef struct {
  char *dir;
  char *sock;
  pid_t pid;
} Daemon;

static bool daemon_up(Daemon *d, const char *const *extra) {
  d->dir = it_tmpdir();
  d->sock = d->dir ? str_printf("%s/d.sock", d->dir) : NULL;
  d->pid = d->sock ? it_laced_start(d->sock, extra) : -1;
  CHECK(d->pid > 0);
  return d->pid > 0;
}

static void daemon_down(Daemon *d) {
  it_laced_stop(d->pid);
  it_rmdir(d->dir);
  free(d->sock);
  free(d->dir);
}

/* A database URL the daemon can open on its own (the test's SQLite file) */
static const char *sqlite_url(const ItDb *db) { return db->url; }

/* After the daemon behind a socket is replaced, a connection id from the
 * old one is refused (the new daemon numbers its own from 1) */
static void test_restart_generation(ItDb *db) {
  Daemon d = {0};
  if (!daemon_up(&d, NULL))
    return;
  lace_client_t *c = it_client_at(d.sock);
  int conn = 0;
  CHECK(lace_connect(c, sqlite_url(db), NULL, &conn) == LACE_OK);
  uint64_t gen = lace_client_generation(c);
  lace_expect_generation(c, gen);

  it_laced_stop(d.pid);
  const char *const none[] = {NULL};
  d.pid = it_laced_start(d.sock, none);
  CHECK(d.pid > 0);

  LaceResult *r = NULL;
  CHECK(lace_exec(c, conn, "SELECT 1", &r) == LACE_ERR_CONNECTION_LOST);
  CHECK(r == NULL);
  CHECK(lace_client_generation(c) > gen);
  /* Opening it again on the new daemon works */
  int again = 0;
  CHECK(lace_connect(c, sqlite_url(db), NULL, &again) == LACE_OK);
  lace_expect_generation(c, lace_client_generation(c));
  CHECK(lace_exec(c, again, "SELECT 1", &r) == LACE_OK);
  lace_result_free(r);
  lace_client_destroy(c);
  daemon_down(&d);
}

static int free_tcp_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  int port = -1;
  if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
      getsockname(fd, (struct sockaddr *)&a, &len) == 0)
    port = ntohs(a.sin_port);
  if (fd >= 0)
    close(fd);
  return port;
}

/* Each link proves it knows the token (and checks the daemon does) */
static lace_client_t *tcp_client(int port, const char *token) {
  LaceClientConfig cfg = {0};
  cfg.mode = LACE_CONN_TCP;
  cfg.host = "127.0.0.1";
  cfg.port = port;
  lace_client_t *c = lace_client_create_with_config(&cfg);
  lace_client_set_token(c, token);
  return c;
}

static const char *str_of(const cJSON *o, const char *a, const char *b) {
  const cJSON *x = o ? cJSON_GetObjectItem(o, a) : NULL;
  x = x && b ? cJSON_GetObjectItem(x, b) : x;
  return x && cJSON_IsString(x) ? x->valuestring : NULL;
}

/* The handshake on the wire: the raw token is refused, the daemon's proof
 * checks out, one proof try per challenge, nothing runs before it */
static void tcp_handshake_raw(int port, const char *token) {
  ItRaw *r = it_raw_open_tcp(port);
  CHECK(r != NULL);
  if (!r)
    return;
  char *p = str_printf("{\"client_id\":\"x\",\"token\":\"%s\"}", token);
  cJSON *rep = it_raw_call(r, "hello", p, 3000);
  CHECK(it_raw_error_code(rep) == LACE_ERR_AUTH_FAILED);
  cJSON_Delete(rep);
  free(p);
  it_raw_close(r);

  r = it_raw_open_tcp(port);
  rep = it_raw_call(r, "ping", NULL, 3000);
  CHECK(it_raw_error_code(rep) == LACE_ERR_AUTH_REQUIRED);
  cJSON_Delete(rep);

  const char *nc = "00112233445566778899aabbccddeeff";
  p = str_printf("{\"client_id\":\"x\",\"client_nonce\":\"%s\"}", nc);
  rep = it_raw_call(r, "hello", p, 3000);
  cJSON *res = rep ? cJSON_GetObjectItem(rep, "result") : NULL;
  const char *nd = str_of(res, "challenge", "daemon_nonce");
  const char *proof = str_of(res, "challenge", "proof");
  CHECK(nd && proof);
  if (nd && proof) {
    char want[2 * LACE_SHA256_LEN + 1];
    lace_auth_proof(token, "laced-daemon", nc, nd, want);
    CHECK(strcmp(proof, want) == 0);
    /* The daemon's own proof does not pass as the client's */
    char *a = str_printf("{\"proof\":\"%s\"}", proof);
    cJSON *bad = it_raw_call(r, "auth", a, 3000);
    CHECK(it_raw_error_code(bad) == LACE_ERR_AUTH_FAILED);
    cJSON_Delete(bad);
    free(a);
  }
  cJSON_Delete(rep);
  /* That was the third strike (ping, hello, bad proof): closed */
  CHECK(it_raw_eof(r, 3000));
  it_raw_close(r);

  r = it_raw_open_tcp(port);
  rep = it_raw_call(r, "hello", p, 3000);
  res = rep ? cJSON_GetObjectItem(rep, "result") : NULL;
  nd = str_of(res, "challenge", "daemon_nonce");
  CHECK(nd != NULL);
  if (nd) {
    char mine[2 * LACE_SHA256_LEN + 1];
    lace_auth_proof(token, "lace-client", nc, nd, mine);
    char *a = str_printf("{\"proof\":\"%s\"}", mine);
    cJSON *ok = it_raw_call(r, "auth", a, 3000);
    CHECK(ok && it_raw_error_code(ok) == 0);
    cJSON_Delete(ok);
    cJSON *pong = it_raw_call(r, "ping", NULL, 3000);
    CHECK(pong && it_raw_error_code(pong) == 0);
    cJSON_Delete(pong);
    free(a);
  }
  cJSON_Delete(rep);
  free(p);
  it_raw_close(r);
}

/* Connections that never authenticate cannot fill the daemon: past
 * LACED_MAX_UNAUTHED_CLIENTS (8) pending, the oldest is dropped, and a
 * real client still gets in */
static void tcp_unauthed_flood(int port, const char *token) {
  ItRaw *idle[12];
  for (int i = 0; i < 12; i++) {
    idle[i] = it_raw_open_tcp(port);
    usleep(20 * 1000); /* Accepted in order: the oldest is idle[0] */
  }
  CHECK(it_raw_eof(idle[0], 3000));
  lace_client_t *good = tcp_client(port, token);
  CHECK(lace_ping(good) == LACE_OK);
  lace_client_destroy(good);
  for (int i = 0; i < 12; i++)
    it_raw_close(idle[i]);
}

/* A process squatting on the port, answering every request with success:
 * it records what it was sent */
typedef struct {
  int listen_fd;
  volatile bool stop;
  char got[65536];
  size_t got_len;
} Impostor;

static void *impostor_run(void *arg) {
  Impostor *im = arg;
  int fds[8];
  int n = 0;
  while (!im->stop) {
    struct pollfd pf[9];
    pf[0] = (struct pollfd){im->listen_fd, POLLIN, 0};
    for (int i = 0; i < n; i++)
      pf[i + 1] = (struct pollfd){fds[i], POLLIN, 0};
    if (poll(pf, (nfds_t)n + 1, 50) <= 0)
      continue;
    if ((pf[0].revents & POLLIN) && n < 8) {
      int c = accept(im->listen_fd, NULL, NULL);
      if (c >= 0)
        fds[n++] = c;
    }
    for (int i = 0; i < n; i++) {
      if (!(pf[i + 1].revents & (POLLIN | POLLHUP)))
        continue;
      char buf[4096];
      ssize_t k = read(fds[i], buf, sizeof(buf) - 1);
      if (k <= 0)
        continue;
      if (im->got_len + (size_t)k < sizeof(im->got)) {
        memcpy(im->got + im->got_len, buf, (size_t)k);
        im->got_len += (size_t)k;
      }
      buf[k] = '\0';
      /* Answer each line with an empty success, as a lax daemon would */
      for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
          *nl = '\0';
        cJSON *req = cJSON_Parse(line);
        cJSON *id = req ? cJSON_GetObjectItem(req, "id") : NULL;
        if (id && cJSON_IsNumber(id)) {
          char *out = str_printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
                                 "\"challenge\":{\"daemon_nonce\":\"00\",\"proof\":\"00\"},"
                                 "\"conn_id\":1}}\n",
                                 id->valueint);
          if (write(fds[i], out, strlen(out)) < 0) {
          }
          free(out);
        }
        cJSON_Delete(req);
        line = nl ? nl + 1 : NULL;
      }
    }
  }
  for (int i = 0; i < n; i++)
    close(fds[i]);
  return NULL;
}

/* TCP: the client authenticates the daemon too. A squatter on the port
 * never sees the token or a password, and nothing is run through it. */
static void test_tcp_impostor(ItDb *db) {
  (void)db;
  Impostor *im = calloc(1, sizeof(*im));
  im->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  CHECK(bind(im->listen_fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(im->listen_fd, (struct sockaddr *)&a, &len) == 0 &&
        listen(im->listen_fd, 8) == 0);
  int port = ntohs(a.sin_port);
  pthread_t th;
  pthread_create(&th, NULL, impostor_run, im);

  const char *token = "0123456789abcdef0123456789abcdef";
  lace_client_t *c = tcp_client(port, token);
  int conn = 0;
  int rc = lace_connect(c, "postgres://u:S3cretPw@127.0.0.1:1/db", NULL, &conn);
  CHECK(rc == LACE_ERR_AUTH_FAILED);
  CHECK(lace_ping(c) != LACE_OK);
  lace_client_destroy(c);

  im->stop = true;
  pthread_join(th, NULL);
  close(im->listen_fd);
  im->got[im->got_len] = '\0';
  CHECK(im->got_len > 0); /* It did hear from the client... */
  CHECK(strstr(im->got, "S3cretPw") == NULL);
  CHECK(strstr(im->got, token) == NULL);
  CHECK(strstr(im->got, "\"connect\"") == NULL);
  free(im);
}

/* TCP: only a client with the daemon's token gets anything done */
static void test_tcp_token(ItDb *db) {
  char *dir = it_tmpdir();
  int port = free_tcp_port();
  char *bind_to = str_printf("127.0.0.1:%d", port);
  char *tokfile = str_printf("%s/token", dir);
  const char *const extra[] = {"--tcp", bind_to, "--token-file", tokfile, NULL};
  pid_t pid = it_laced_start(NULL, extra);
  CHECK(pid > 0);
  char *token = NULL;
  for (int i = 0; i < 100 && !token; i++) {
    token = lace_read_token_file(tokfile);
    if (!token)
      usleep(30 * 1000);
  }
  CHECK(token != NULL);

  if (token) {
    lace_client_t *good = tcp_client(port, token);
    CHECK(lace_ping(good) == LACE_OK);
    int conn = 0;
    CHECK(lace_connect(good, sqlite_url(db), NULL, &conn) == LACE_OK);
    lace_client_destroy(good);

    lace_client_t *bad = tcp_client(port, "not-the-token");
    CHECK(lace_ping(bad) != LACE_OK);
    lace_client_destroy(bad);
    tcp_handshake_raw(port, token);
    tcp_unauthed_flood(port, token);
  }
  free(token);
  it_laced_stop(pid);
  /* The token file goes with the daemon */
  CHECK(access(tokfile, F_OK) != 0);
  it_rmdir(dir);
  free(dir);
  free(bind_to);
  free(tokfile);
}

/* --idle-timeout: the daemon exits once it has had no client that long */
static void test_idle_timeout(ItDb *db) {
  (void)db;
  char *dir = it_tmpdir();
  char *sock = str_printf("%s/d.sock", dir);
  const char *const extra[] = {"--idle-timeout", "1", NULL};
  pid_t pid = it_laced_start(sock, extra);
  CHECK(pid > 0);
  ItRaw *r = it_raw_open(sock, "idle");
  cJSON *pong = r ? it_raw_call(r, "ping", NULL, 3000) : NULL;
  CHECK(pong != NULL);
  cJSON_Delete(pong);
  CHECK(!it_laced_exited(pid, 1500)); /* Not while a client is there */
  it_raw_close(r);
  CHECK(it_laced_exited(pid, 5000));
  it_rmdir(dir);
  free(dir);
  free(sock);
}

/* --max-clients: one more is refused with an error, not left waiting */
static void test_max_clients(ItDb *db) {
  (void)db;
  Daemon d = {0};
  const char *const extra[] = {"--max-clients", "2", NULL};
  if (!daemon_up(&d, extra))
    return;
  ItRaw *a = it_raw_open(d.sock, "a"), *b = it_raw_open(d.sock, "b");
  CHECK(a && b);
  ItRaw *c = it_raw_open(d.sock, NULL);
  cJSON *refused = c ? it_raw_read(c, 3000) : NULL;
  CHECK(refused && it_raw_error_code(refused) != 0);
  cJSON_Delete(refused);
  CHECK(c && it_raw_eof(c, 3000));
  it_raw_close(c);
  /* The others are still served */
  cJSON *pong = a ? it_raw_call(a, "ping", NULL, 3000) : NULL;
  CHECK(pong && it_raw_error_code(pong) == 0);
  cJSON_Delete(pong);
  it_raw_close(a);
  it_raw_close(b);
  daemon_down(&d);
}

/* A client that floods requests and never reads its replies is dropped
 * once its output stalls, and slows nobody else down meanwhile */
static void test_flood(ItDb *db) {
  (void)db;
  Daemon d = {0};
  if (!daemon_up(&d, NULL))
    return;
  ItRaw *flood = it_raw_open(d.sock, "flooder");
  ItRaw *victim = it_raw_open(d.sock, "victim");
  CHECK(flood && victim);
  if (flood && victim) {
    StringBuilder *sb = sb_new(256 * 1024);
    for (int i = 0; i < 5000; i++)
      sb_printf(sb, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"version\"}\n", 10000 + i);
    char *payload = sb_finish(sb);
    size_t n = strlen(payload), off = 0;
    int fd = it_raw_fd(flood);
    /* Write what the socket takes without blocking */
    int64_t until = it_now_ms() + 2000;
    while (off < n && it_now_ms() < until) {
      ssize_t w = send(fd, payload + off, n - off, MSG_DONTWAIT);
      if (w > 0)
        off += (size_t)w;
      else
        usleep(10 * 1000);
    }
    free(payload);

    int64_t worst = 0;
    for (int i = 0; i < 10; i++) {
      int64_t t0 = it_now_ms();
      cJSON *pong = it_raw_call(victim, "ping", NULL, 3000);
      CHECK(pong != NULL);
      cJSON_Delete(pong);
      int64_t ms = it_now_ms() - t0;
      worst = ms > worst ? ms : worst;
      usleep(50 * 1000);
    }
    CHECK(worst < 500);
    /* Dropped after LACED_WRITE_TIMEOUT_MS (5 s) without progress. Reading
     * would be progress: wait without reading, then look for the EOF */
    usleep(7 * 1000 * 1000);
    CHECK(it_raw_eof(flood, 5000));
  }
  it_raw_close(flood);
  it_raw_close(victim);
  daemon_down(&d);
}

/* One client may run at most LACED_MAX_JOBS_PER_CLIENT (32) jobs at once;
 * more are refused with LACE_ERR_DAEMON_BUSY, not queued */
static void test_job_cap(ItDb *db) {
  Daemon d = {0};
  if (!daemon_up(&d, NULL))
    return;
  ItRaw *r = it_raw_open(d.sock, "capper");
  int conns[40];
  bool ok = r != NULL;
  for (int i = 0; ok && i < 40; i++) {
    char *p = str_printf("{\"connstr\":\"%s\"}", sqlite_url(db));
    cJSON *res = it_raw_call(r, "connect", p, 5000);
    free(p);
    cJSON *id = res ? cJSON_GetObjectItem(cJSON_GetObjectItem(res, "result"), "conn_id") : NULL;
    ok = id && cJSON_IsNumber(id);
    conns[i] = ok ? id->valueint : 0;
    cJSON_Delete(res);
  }
  CHECK(ok);
  int busy = 0;
  if (ok) {
    /* 40 statements that run until stopped, one per connection */
    for (int i = 0; i < 40; i++) {
      char *p = str_printf("{\"conn_id\":%d,\"sql\":\"WITH RECURSIVE c(x) AS (SELECT "
                           "1 UNION ALL SELECT x + 1 FROM c) SELECT COUNT(*) FROM c\"}",
                           conns[i]);
      it_raw_send(r, "query", p);
      free(p);
    }
    cJSON *j;
    int64_t until = it_now_ms() + 3000;
    while (it_now_ms() < until && (j = it_raw_read(r, 500)) != NULL) {
      busy += it_raw_error_code(j) == LACE_ERR_DAEMON_BUSY;
      cJSON_Delete(j);
    }
  }
  CHECK(busy == 8);
  it_raw_close(r);
  kill(d.pid, SIGKILL); /* 32 statements would take the full shutdown wait */
  waitpid(d.pid, NULL, 0);
  d.pid = 0;
  daemon_down(&d);
}

typedef struct {
  lace_client_t *c;
  int conn;
  int rc;
} Running;

static void *run_forever(void *p) {
  Running *r = p;
  r->rc = lace_exec(r->c, r->conn,
                    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 "
                    "FROM c) SELECT COUNT(*) FROM c",
                    NULL);
  return NULL;
}

/* Only the client that started a statement may cancel it */
static void test_cancel_owner(ItDb *db) {
  Daemon d = {0};
  if (!daemon_up(&d, NULL))
    return;
  lace_client_t *a = it_client_at(d.sock), *b = it_client_at(d.sock);
  Running run = {a, 0, 0};
  CHECK(lace_connect(a, sqlite_url(db), NULL, &run.conn) == LACE_OK);
  pthread_t th;
  pthread_create(&th, NULL, run_forever, &run);
  usleep(500 * 1000);
  CHECK(lace_cancel_query(b, run.conn) != LACE_OK); /* Not B's statement */
  usleep(300 * 1000);
  CHECK(lace_cancel_query(a, run.conn) == LACE_OK);
  pthread_join(th, NULL);
  CHECK(run.rc == LACE_ERR_QUERY_CANCELLED);
  lace_client_destroy(a);
  lace_client_destroy(b);
  daemon_down(&d);
}

static bool connection_listed(ItRaw *r, int conn) {
  cJSON *res = it_raw_call(r, "connections", NULL, 3000);
  bool found = false;
  cJSON *arr = res ? cJSON_GetObjectItem(res, "result") : NULL;
  cJSON *it;
  cJSON_ArrayForEach(it, arr) {
    cJSON *id = cJSON_GetObjectItem(it, "id");
    found = found || (id && cJSON_IsNumber(id) && id->valueint == conn);
  }
  cJSON_Delete(res);
  return found;
}

/* A client's database connections outlive its sockets by the grace period
 * (enough to reconnect), then are closed */
static void test_orphan_reaper(ItDb *db) {
  Daemon d = {0};
  const char *const extra[] = {"--orphan-grace", "1", NULL};
  if (!daemon_up(&d, extra))
    return;
  ItRaw *owner = it_raw_open(d.sock, "owner-x");
  char *p = str_printf("{\"connstr\":\"%s\"}", sqlite_url(db));
  cJSON *res = owner ? it_raw_call(owner, "connect", p, 5000) : NULL;
  free(p);
  cJSON *id = res ? cJSON_GetObjectItem(cJSON_GetObjectItem(res, "result"), "conn_id") : NULL;
  int conn = id && cJSON_IsNumber(id) ? id->valueint : 0;
  cJSON_Delete(res);
  CHECK(conn > 0);
  it_raw_close(owner);

  ItRaw *watcher = it_raw_open(d.sock, "watcher");
  CHECK(watcher && connection_listed(watcher, conn)); /* Within the grace */
  bool gone = false;
  for (int i = 0; i < 40 && !gone; i++) {
    usleep(150 * 1000);
    gone = !connection_listed(watcher, conn);
  }
  CHECK(gone);

  /* An owner that comes back within the grace keeps its connection */
  ItRaw *o2 = it_raw_open(d.sock, "owner-y");
  p = str_printf("{\"connstr\":\"%s\"}", sqlite_url(db));
  res = o2 ? it_raw_call(o2, "connect", p, 5000) : NULL;
  free(p);
  id = res ? cJSON_GetObjectItem(cJSON_GetObjectItem(res, "result"), "conn_id") : NULL;
  int conn2 = id && cJSON_IsNumber(id) ? id->valueint : 0;
  cJSON_Delete(res);
  it_raw_close(o2);
  ItRaw *back = it_raw_open(d.sock, "owner-y");
  usleep(1800 * 1000);
  CHECK(conn2 > 0 && connection_listed(watcher, conn2));
  it_raw_close(back);
  it_raw_close(watcher);
  daemon_down(&d);
}

/* A directory or file named name in the default socket directory (the run's
 * private HOME), optionally with an owner file holding pid */
static char *plant(const char *dir, const char *name, bool is_dir, long owner_pid,
                   time_t age) {
  char *path = str_printf("%s/%s", dir, name);
  if (is_dir) {
    mkdir(path, 0700);
    if (owner_pid) {
      char *owner = str_printf("%s/owner", path);
      FILE *f = fopen(owner, "w");
      fprintf(f, "%ld\n", owner_pid);
      fclose(f);
      free(owner);
    }
  } else {
    FILE *f = fopen(path, "w");
    fputs("secret\n", f);
    fclose(f);
  }
  if (age) {
    struct timeval tv[2] = {{time(NULL) - age, 0}, {time(NULL) - age, 0}};
    utimes(path, tv);
  }
  return path;
}

static bool exists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

/* A starting daemon removes what crashed daemons left next to the default
 * socket: tunnel directories whose daemon is gone, and old askpass files
 * of older daemons. Nothing whose owner may be alive. */
static void test_sweep_stale(ItDb *db) {
  (void)db;
  char *sock = lace_get_default_socket_path();
  char *slash = sock ? strrchr(sock, '/') : NULL;
  CHECK(slash != NULL);
  if (!slash) {
    free(sock);
    return;
  }
  *slash = '\0';
  mkdir(sock, 0700); /* The daemon would create it; the test plants first */
  /* A pid that is certainly gone: a child we started and reaped */
  pid_t gone = fork();
  if (gone == 0)
    _exit(0);
  waitpid(gone, NULL, 0);
  char *dead = plant(sock, "lace-tun-itdead", true, (long)gone, 0);
  char *alive = plant(sock, "lace-tun-italive", true, (long)getpid(), 0);
  char *unowned = plant(sock, "lace-tun-itnoowner", true, 0, 0);
  char *old_pass = plant(sock, "lace-sshpass-itold", false, 0, 3600);
  char *new_pass = plant(sock, "lace-askpass-itnew", false, 0, 0);

  Daemon d = {0};
  if (daemon_up(&d, NULL)) {
    CHECK(!exists(dead));
    CHECK(exists(alive));
    CHECK(exists(unowned));
    CHECK(!exists(old_pass));
    CHECK(exists(new_pass));
    daemon_down(&d);
  }
  it_rmdir(alive);
  it_rmdir(unowned);
  it_rmdir(dead);
  unlink(old_pass);
  unlink(new_pass);
  free(dead);
  free(alive);
  free(unowned);
  free(old_pass);
  free(new_pass);
  free(sock);
}

const ItTest it_daemon_tests[] = {
    {"daemon_restart_generation", test_restart_generation, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_tcp_token", test_tcp_token, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_tcp_impostor", test_tcp_impostor, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_sweep_stale", test_sweep_stale, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_idle_timeout", test_idle_timeout, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_max_clients", test_max_clients, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_flood", test_flood, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_job_cap", test_job_cap, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_cancel_owner", test_cancel_owner, IT_SQLITE | IT_SOCKET_ONLY, false},
    {"daemon_orphan_reaper", test_orphan_reaper, IT_SQLITE | IT_SOCKET_ONLY, false},
    {NULL, NULL, 0, false},
};
