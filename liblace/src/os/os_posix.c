/*
 * liblace - Lace Client Library
 * Operating-system layer for POSIX systems (macOS, Linux, the BSDs)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef _WIN32

#include "os.h"
#include "../../include/util/str.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define FD(h) ((int)(h))

/* ---- Time ---------------------------------------------------------------- */

int64_t lace_os_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void lace_os_sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

/* ---- Files and paths ----------------------------------------------------- */

char *lace_os_default_socket_path(void) {
  /* One canonical location per user. The client never searches shared
   * places such as /tmp: another local user could plant a socket there and
   * collect the connection strings (passwords) sent to it. */
#ifdef __APPLE__
  const char *home = getenv("HOME");
  if (home && home[0])
    return str_printf("%s/Library/Caches/laced.sock", home);
#endif

  /* A system daemon run by root */
  if (geteuid() == 0)
    return str_dup("/run/laced.sock");

  /* The user's private runtime directory, if it really is private */
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  struct stat st;
  if (xdg && xdg[0] && stat(xdg, &st) == 0 && S_ISDIR(st.st_mode) &&
      st.st_uid == geteuid() && (st.st_mode & 077) == 0)
    return str_printf("%s/laced.sock", xdg);

  /* A 0700 directory in the user's home (the daemon creates it) */
  const char *home_dir = getenv("HOME");
  if (home_dir && home_dir[0])
    return str_printf("%s/.cache/lace/laced.sock", home_dir);

  /* Last resort: a private per-user directory under /tmp (the daemon
   * creates it 0700; the client verifies who owns the socket) */
  return str_printf("/tmp/lace-%d/laced.sock", (int)geteuid());
}

char *lace_os_exe_dir(void) {
  char exe_path[4096];
#ifdef __APPLE__
  uint32_t size = sizeof(exe_path);
  if (_NSGetExecutablePath(exe_path, &size) != 0)
    return NULL;
  char *resolved = realpath(exe_path, NULL);
  if (resolved) {
    snprintf(exe_path, sizeof(exe_path), "%s", resolved);
    free(resolved);
  }
#else
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len <= 0)
    return NULL;
  exe_path[len] = '\0';
#endif
  char *last_slash = strrchr(exe_path, '/');
  if (!last_slash)
    return NULL;
  *last_slash = '\0';
  return str_dup(exe_path);
}

bool lace_os_is_executable(const char *path) {
  struct stat st;
  return path && access(path, X_OK) == 0 && stat(path, &st) == 0 &&
         S_ISREG(st.st_mode);
}

char *lace_os_real_path(const char *path) {
  return path ? realpath(path, NULL) : NULL;
}

char *lace_os_current_dir(void) {
  char cwd[4096];
  return getcwd(cwd, sizeof(cwd)) ? str_dup(cwd) : NULL;
}

bool lace_os_path_is_absolute(const char *path) {
  return path && path[0] == '/';
}

const char *lace_os_daemon_name(void) { return "laced"; }

char lace_os_path_list_separator(void) { return ':'; }

char *lace_os_read_private_file(const char *path, size_t max_len) {
  if (!path || max_len == 0)
    return NULL;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return NULL;
  struct stat st;
  char *buf = NULL;
  ssize_t n = -1;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
      (st.st_mode & 077) == 0) {
    buf = malloc(max_len + 1);
    if (buf)
      n = read(fd, buf, max_len);
  }
  close(fd);
  if (n <= 0) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  return buf;
}

bool lace_os_random(void *buf, size_t len) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  bool ok = read(fd, buf, len) == (ssize_t)len;
  close(fd);
  return ok;
}

uint64_t lace_os_process_id(void) { return (uint64_t)getpid(); }

/* ---- Sockets ------------------------------------------------------------- */

/* A stream socket not inherited by exec'd processes, whose writes fail with
 * EPIPE instead of raising SIGPIPE when the daemon end has closed */
static int stream_socket(int domain) {
  int fd = socket(domain, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return fd;
}

/* connect() with a timeout; the socket is blocking again afterwards */
static bool connect_timeout(int fd, const struct sockaddr *addr, socklen_t len,
                            int timeout_ms) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = connect(fd, addr, len);
  if (r < 0 && errno != EINPROGRESS)
    return false;
  if (r < 0) {
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    if (poll(&p, 1, timeout_ms) <= 0)
      return false;
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
      return false;
  }
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags);
  return true;
}

/* sockaddr for a Unix socket path. A path longer than sun_path would be cut
 * short silently: refuse it rather than reach a different socket. */
static bool unix_addr(const char *path, struct sockaddr_un *addr) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr->sun_path)) {
    errno = ENAMETOOLONG;
    return false;
  }
  memcpy(addr->sun_path, path, strlen(path) + 1);
  return true;
}

LaceOsHandle lace_os_connect_unix(const char *path, int timeout_ms,
                                  bool verify_owner) {
  struct sockaddr_un addr;
  if (!path || !unix_addr(path, &addr))
    return LACE_OS_INVALID;
  if (verify_owner && !lace_os_socket_owned_by_me(path)) {
    errno = EPERM;
    return LACE_OS_INVALID;
  }
  int fd = stream_socket(AF_UNIX);
  if (fd < 0)
    return LACE_OS_INVALID;
  if (!connect_timeout(fd, (struct sockaddr *)&addr, sizeof(addr), timeout_ms) ||
      (verify_owner && !lace_os_peer_is_me(fd))) {
    close(fd);
    return LACE_OS_INVALID;
  }
  return fd;
}

LaceOsHandle lace_os_connect_tcp(const char *host, int port, int timeout_ms) {
  char service[16];
  snprintf(service, sizeof(service), "%d", port);
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host && host[0] ? host : "127.0.0.1", service, &hints, &res) != 0)
    return LACE_OS_INVALID;
  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = stream_socket(ai->ai_family);
    if (fd < 0)
      continue;
    if (connect_timeout(fd, ai->ai_addr, ai->ai_addrlen, timeout_ms))
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd >= 0 ? fd : LACE_OS_INVALID;
}

bool lace_os_unix_listening(const char *path) {
  struct stat st;
  struct sockaddr_un addr;
  if (!path || stat(path, &st) < 0 || !S_ISSOCK(st.st_mode) ||
      !unix_addr(path, &addr))
    return false;
  int fd = stream_socket(AF_UNIX);
  if (fd < 0)
    return false;
  bool ok = connect_timeout(fd, (struct sockaddr *)&addr, sizeof(addr), 100);
  close(fd);
  return ok;
}

bool lace_os_socket_owned_by_me(const char *path) {
  struct stat st;
  return path && lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) &&
         st.st_uid == geteuid();
}

bool lace_os_peer_is_me(LaceOsHandle h) {
  uid_t peer = (uid_t)-1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
  gid_t gid;
  if (getpeereid(FD(h), &peer, &gid) != 0)
    return false;
#elif defined(SO_PEERCRED)
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(FD(h), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
    return false;
  peer = cred.uid;
#else
  (void)h;
  return true; /* No way to ask: rely on the socket's ownership check */
#endif
  return peer == geteuid();
}

/* ---- Streams ------------------------------------------------------------- */

int lace_os_wait(LaceOsHandle h, bool is_socket, bool for_write, int timeout_ms) {
  (void)is_socket;
  struct pollfd p = {.fd = FD(h), .events = for_write ? POLLOUT : POLLIN};
  for (;;) {
    int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR)
      continue;
    if (r < 0)
      return -1;
    if (r == 0)
      return 0;
    if (for_write && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
      return -1;
    return 1; /* Readable: data, or EOF the next read reports */
  }
}

int64_t lace_os_read(LaceOsHandle h, bool is_socket, void *buf, size_t len) {
  (void)is_socket;
  ssize_t n = read(FD(h), buf, len);
  if (n < 0)
    return (errno == EINTR || errno == EAGAIN) ? -2 : -1;
  return n;
}

int64_t lace_os_write(LaceOsHandle h, bool is_socket, const void *buf, size_t len) {
  (void)is_socket;
  ssize_t n = write(FD(h), buf, len);
  if (n < 0)
    return (errno == EINTR || errno == EAGAIN) ? -2 : -1;
  return n == 0 ? -1 : n;
}

bool lace_os_hung_up(LaceOsHandle h, bool is_socket) {
  struct pollfd p = {.fd = FD(h), .events = POLLIN};
  if (poll(&p, 1, 0) <= 0)
    return false; /* Nothing pending: healthy idle stream */
  if (p.revents & (POLLERR | POLLNVAL))
    return true;
  if (!is_socket) {
    /* A pipe: hung up once the writer is gone and nothing is left to read */
    return (p.revents & POLLHUP) && !(p.revents & POLLIN);
  }
  char c;
  ssize_t n = recv(FD(h), &c, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n == 0)
    return true; /* Orderly shutdown */
  return n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

void lace_os_close(LaceOsHandle h, bool is_socket) {
  (void)is_socket;
  if (h != LACE_OS_INVALID)
    close(FD(h));
}

/* ---- Processes ----------------------------------------------------------- */

/* Upper bound for descriptors a child must not inherit, computed before
 * fork: the child of a multithreaded process may only make
 * async-signal-safe calls until it execs */
static int inherited_fd_limit(void) {
  long max_fd = sysconf(_SC_OPEN_MAX);
  return (int)(max_fd < 0 || max_fd > 65536 ? 65536 : max_fd);
}

/* In the child: close everything above stderr. A long-lived daemon holding
 * the frontend's other links (or an embedder's sockets) keeps them open
 * after the frontend closes them, so their peers never see EOF. */
static void close_inherited_fds(int max_fd) {
  for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++)
    close(fd);
}

/* A daemon that outlives us must not be our child: when it exits, a child
 * stays a zombie until we reap it (a frontend whose shared daemon was
 * replaced again and again collected one per daemon). So fork twice: the
 * middle process starts the daemon in a new session and exits at once, the
 * daemon is adopted by init, and its pid comes back through a pipe. Only
 * async-signal-safe calls between fork and exec. */
LaceOsProcess lace_os_spawn_detached(const char *exe, const char *const *argv) {
  int max_fd = inherited_fd_limit();
  int pfd[2];
  if (pipe(pfd) < 0)
    return 0;
  pid_t mid = fork();
  if (mid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return 0;
  }
  if (mid == 0) {
    setsid(); /* Off the terminal: no SIGHUP when it closes */
    pid_t daemon_pid = fork();
    if (daemon_pid != 0) {
      /* The daemon's pid (or failure), then leave it to init */
      (void)!write(pfd[1], &daemon_pid, sizeof(daemon_pid));
      _exit(daemon_pid > 0 ? 0 : 1);
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    close_inherited_fds(max_fd); /* The pipe too */
    execv(exe, (char *const *)argv);
    _exit(127);
  }
  close(pfd[1]);
  pid_t daemon_pid = 0;
  ssize_t n;
  do {
    n = read(pfd[0], &daemon_pid, sizeof(daemon_pid));
  } while (n < 0 && errno == EINTR);
  close(pfd[0]);
  while (waitpid(mid, NULL, 0) < 0 && errno == EINTR) {
  }
  return n == (ssize_t)sizeof(daemon_pid) && daemon_pid > 0 ? daemon_pid : 0;
}

LaceOsProcess lace_os_spawn_piped(const char *exe, const char *const *argv,
                                  LaceOsHandle *to_child,
                                  LaceOsHandle *from_child) {
  int in[2], out[2];
  if (pipe(in) < 0)
    return 0;
  if (pipe(out) < 0) {
    close(in[0]);
    close(in[1]);
    return 0;
  }
  int max_fd = inherited_fd_limit();
  pid_t pid = fork();
  if (pid < 0) {
    close(in[0]);
    close(in[1]);
    close(out[0]);
    close(out[1]);
    return 0;
  }
  if (pid == 0) {
    dup2(in[0], STDIN_FILENO);
    dup2(out[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0)
      dup2(devnull, STDERR_FILENO);
    close_inherited_fds(max_fd);
    execv(exe, (char *const *)argv);
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  fcntl(in[1], F_SETFD, FD_CLOEXEC);
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  *to_child = in[1];
  *from_child = out[0];
  return pid;
}

bool lace_os_process_exited(LaceOsProcess p) {
  if (p <= 0)
    return true;
  pid_t r = waitpid((pid_t)p, NULL, WNOHANG);
  if (r == (pid_t)p)
    return true; /* Our child, now reaped */
  if (r < 0 && errno == ECHILD) {
    /* Not our child (a detached daemon, adopted by init): gone or not */
    return kill((pid_t)p, 0) != 0 && errno == ESRCH;
  }
  return false;
}

void lace_os_process_stop(LaceOsProcess p, bool force) {
  if (p <= 0)
    return;
  kill((pid_t)p, force ? SIGKILL : SIGTERM);
  waitpid((pid_t)p, NULL, 0);
}

#endif /* !_WIN32 */
