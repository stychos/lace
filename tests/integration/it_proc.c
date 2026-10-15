/*
 * Lace
 * Integration test helpers: private daemons and a raw JSON-RPC client
 * (for misbehaving on purpose: floods, half-read replies, many clients)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>

const char *it_driver_name(const ItDb *db) {
  switch (db->driver) {
  case LACE_DRIVER_SQLITE:
    return "sqlite";
  case LACE_DRIVER_POSTGRES:
    return "postgres";
  case LACE_DRIVER_MARIADB:
    return "mariadb";
  default:
    return "mysql";
  }
}

/* ---- Private directory and daemons ------------------------------------- */

char *it_tmpdir(void) {
  /* Short: socket paths must fit sun_path */
  const char *base = getenv("TMPDIR");
  if (!base || strlen(base) > 60)
    base = "/tmp";
  char *tmpl = str_printf("%s/lace-itd-XXXXXX", base);
  if (!mkdtemp(tmpl)) {
    free(tmpl);
    return NULL;
  }
  return tmpl;
}

void it_rmdir(const char *dir) {
  if (!dir)
    return;
  char *cmd = str_printf("rm -rf '%s'", dir);
  if (system(cmd) != 0)
    fprintf(stderr, "  could not remove %s\n", dir);
  free(cmd);
}

pid_t it_laced_start(const char *socket_path, const char *const *extra) {
  const char *laced = getenv("LACE_IT_LACED");
  if (!laced)
    return -1;
  const char *argv[24];
  int n = 0;
  argv[n++] = laced;
  if (socket_path) {
    argv[n++] = "--unix";
    argv[n++] = socket_path;
  }
  argv[n++] = "--quiet";
  for (size_t i = 0; extra && extra[i] && n < 22; i++)
    argv[n++] = extra[i];
  argv[n] = NULL;

  pid_t pid = fork();
  if (pid == 0) {
    execv(laced, (char *const *)argv);
    _exit(127);
  }
  if (pid < 0 || !socket_path)
    return pid;
  /* Ready once the socket exists */
  for (int i = 0; i < 200; i++) {
    struct stat st;
    if (stat(socket_path, &st) == 0)
      return pid;
    int status;
    if (waitpid(pid, &status, WNOHANG) == pid)
      return -1;
    usleep(25 * 1000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  return -1;
}

bool it_laced_exited(pid_t pid, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 50) {
    if (waitpid(pid, NULL, WNOHANG) == pid)
      return true;
    usleep(50 * 1000);
  }
  return false;
}

void it_laced_stop(pid_t pid) {
  if (pid <= 0)
    return;
  kill(pid, SIGTERM);
  if (!it_laced_exited(pid, 8000)) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
}

lace_client_t *it_client_at(const char *socket_path) {
  LaceClientConfig cfg = {0};
  cfg.mode = LACE_CONN_UNIX;
  cfg.socket_path = socket_path;
  return lace_client_create_with_config(&cfg);
}

/* ---- Raw JSON-RPC client ------------------------------------------------ */

struct ItRaw {
  int fd;
  char *buf;
  size_t len, cap;
  int next_id;
};

ItRaw *it_raw_open(const char *socket_path, const char *client_id) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return NULL;
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return NULL;
  }
  ItRaw *r = calloc(1, sizeof(*r));
  r->fd = fd;
  if (client_id) {
    char *p = str_printf("{\"client_id\":\"%s\"}", client_id);
    cJSON *res = it_raw_call(r, "hello", p, 5000);
    free(p);
    cJSON_Delete(res);
  }
  return r;
}

ItRaw *it_raw_open_tcp(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return NULL;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return NULL;
  }
  ItRaw *r = calloc(1, sizeof(*r));
  r->fd = fd;
  return r;
}

void it_raw_close(ItRaw *r) {
  if (!r)
    return;
  close(r->fd);
  free(r->buf);
  free(r);
}

int it_raw_fd(const ItRaw *r) { return r->fd; }

int it_raw_send(ItRaw *r, const char *method, const char *params_json) {
  int id = ++r->next_id;
  char *line = str_printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"%s%s}\n",
                          id, method, params_json ? ",\"params\":" : "",
                          params_json ? params_json : "");
  size_t n = strlen(line), off = 0;
  while (off < n) {
    ssize_t w = write(r->fd, line + off, n - off);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      break;
    off += (size_t)w;
  }
  free(line);
  return off == n ? id : -1;
}

cJSON *it_raw_read(ItRaw *r, int timeout_ms) {
  int64_t deadline = it_now_ms() + timeout_ms;
  for (;;) {
    char *nl = r->buf ? memchr(r->buf, '\n', r->len) : NULL;
    if (nl) {
      size_t n = (size_t)(nl - r->buf);
      char *line = str_ndup(r->buf, n);
      memmove(r->buf, nl + 1, r->len - n - 1);
      r->len -= n + 1;
      cJSON *j = cJSON_Parse(line);
      free(line);
      return j;
    }
    int left = (int)(deadline - it_now_ms());
    if (left <= 0)
      return NULL;
    struct pollfd p = {.fd = r->fd, .events = POLLIN};
    if (poll(&p, 1, left) <= 0)
      return NULL;
    if (r->len + 65536 > r->cap) {
      r->cap = r->len + 65536 * 2;
      r->buf = realloc(r->buf, r->cap);
    }
    ssize_t got = read(r->fd, r->buf + r->len, r->cap - r->len);
    if (got <= 0)
      return NULL; /* EOF or error */
    r->len += (size_t)got;
  }
}

bool it_raw_eof(ItRaw *r, int timeout_ms) {
  int64_t deadline = it_now_ms() + timeout_ms;
  char tmp[65536];
  for (;;) {
    int left = (int)(deadline - it_now_ms());
    if (left <= 0)
      return false;
    struct pollfd p = {.fd = r->fd, .events = POLLIN};
    if (poll(&p, 1, left) <= 0)
      return false;
    ssize_t got = read(r->fd, tmp, sizeof(tmp));
    if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN))
      return true;
  }
}

cJSON *it_raw_call(ItRaw *r, const char *method, const char *params_json,
                   int timeout_ms) {
  int id = it_raw_send(r, method, params_json);
  if (id < 0)
    return NULL;
  for (;;) {
    cJSON *j = it_raw_read(r, timeout_ms);
    if (!j)
      return NULL;
    cJSON *jid = cJSON_GetObjectItem(j, "id");
    if (jid && cJSON_IsNumber(jid) && jid->valueint == id)
      return j;
    cJSON_Delete(j);
  }
}

int it_raw_error_code(const cJSON *reply) {
  const cJSON *err = reply ? cJSON_GetObjectItem(reply, "error") : NULL;
  const cJSON *code = err ? cJSON_GetObjectItem(err, "code") : NULL;
  return code && cJSON_IsNumber(code) ? code->valueint : 0;
}
