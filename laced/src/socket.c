/*
 * laced - Lace Database Daemon
 * Socket utilities for Unix and TCP socket communication
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "server.h"
#include "log.h"
#include "constants.h"
#include "../../liblace/include/lace.h" /* Default socket and token paths */
#include <util/mem.h>
#include <util/str.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ==========================================================================
 * Socket Path Resolution
 * ========================================================================== */


char *laced_get_default_socket_path(void) {
  char *path = lace_get_default_socket_path();
  if (!path) {
    return NULL;
  }
  /* ~/.cache/lace or /tmp/lace-<uid> may not exist yet: create it private.
   * An existing directory must be ours and closed to others. */
  char *slash = strrchr(path, '/');
  if (slash && slash != path) {
    *slash = '\0';
    /* Every missing component (~/.cache on a fresh account, too); the ones
     * created here are private */
    for (char *p = path + 1; *p; p++) {
      if (*p == '/') {
        *p = '\0';
        mkdir(path, 0700);
        *p = '/';
      }
    }
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
      LOG_WARN("Cannot create socket directory %s: %s", path, strerror(errno));
    }
    struct stat st;
    bool is_private_dir = strstr(path, "/.cache/lace") || strstr(path, "/tmp/lace-");
    if (is_private_dir && stat(path, &st) == 0 &&
        (st.st_uid != geteuid() || (st.st_mode & 077) != 0)) {
      LOG_ERROR("Socket directory %s is not private to this user; refusing it",
                path);
      free(path);
      return NULL;
    }
    *slash = '/';
  }
  return path;
}

bool laced_remove_stale_socket(const char *path) {
  if (!path) {
    return false;
  }

  struct stat st;
  if (stat(path, &st) < 0) {
    /* Socket doesn't exist, that's fine */
    if (errno == ENOENT) {
      return true;
    }
    return false;
  }

  /* Check if it's a socket */
  if (!S_ISSOCK(st.st_mode)) {
    return false;
  }

  /* Try to connect to see if a daemon is running */
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  /* A longer path would be cut short silently: refuse it rather than
   * bind or connect to a different (truncated) path */
  if (strlen(path) >= sizeof(addr.sun_path)) {
    close(fd);
    errno = ENAMETOOLONG;
    return true; /* Nothing to remove; binding reports the error */
  }
  memcpy(addr.sun_path, path, strlen(path) + 1);

  int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  close(fd);

  if (result == 0) {
    /* Daemon is running */
    return false;
  }

  /* Socket exists but no daemon - remove stale socket */
  if (unlink(path) < 0) {
    return false;
  }

  return true;
}

/* ==========================================================================
 * Peer Authorization
 * ========================================================================== */

bool laced_peer_allowed(int fd, gid_t group) {
  uid_t uid = (uid_t)-1;
  gid_t gid = (gid_t)-1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
  if (getpeereid(fd, &uid, &gid) != 0) {
    return false;
  }
#elif defined(SO_PEERCRED)
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return false;
  }
  uid = cred.uid;
  gid = cred.gid;
#else
  (void)fd;
  (void)group;
  return true; /* Filesystem permissions are the only control here */
#endif
  if (uid == geteuid() || uid == 0) {
    return true;
  }
  if (group == (gid_t)-1) {
    return false;
  }
  if (gid == group) {
    return true;
  }
  /* Supplementary groups of the peer's user */
  struct passwd *pw = getpwuid(uid);
  if (!pw) {
    return false;
  }
  int ngroups = 64;
#ifdef __APPLE__
  int groups[64];
  if (getgrouplist(pw->pw_name, (int)pw->pw_gid, groups, &ngroups) < 0) {
    ngroups = 64;
  }
#else
  gid_t groups[64];
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
    ngroups = 64;
  }
#endif
  for (int i = 0; i < ngroups; i++) {
    if ((gid_t)groups[i] == group) {
      return true;
    }
  }
  return false;
}

/* ==========================================================================
 * Socket Creation
 * ========================================================================== */

int laced_create_unix_socket(const char *path, gid_t group) {
  if (!path) {
    return -1;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  /* Set non-blocking */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  /* Allow address reuse */
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  /* A longer path would be cut short silently: refuse it rather than
   * bind or connect to a different (truncated) path */
  if (strlen(path) >= sizeof(addr.sun_path)) {
    close(fd);
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(addr.sun_path, path, strlen(path) + 1);

  /* Create the socket with its final permissions (no window in which it
   * is open to others): owner only, or owner and group with a socket group */
  bool shared = group != (gid_t)-1;
  mode_t old_mask = umask(shared ? 0117 : 0177);
  int bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  umask(old_mask);
  if (bound < 0) {
    close(fd);
    return -1;
  }
  if (shared && chown(path, (uid_t)-1, group) != 0) {
    LOG_ERROR("Cannot give socket %s to the socket group: %s", path,
              strerror(errno));
    close(fd);
    unlink(path);
    return -1;
  }

  if (listen(fd, 16) < 0) {
    close(fd);
    unlink(path);
    return -1;
  }

  return fd;
}

int laced_create_tcp_socket(const char *bind_addr, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  /* Set non-blocking */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  /* Allow address reuse */
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port > 0 ? (uint16_t)port : LACED_DEFAULT_PORT);

  if (bind_addr && strlen(bind_addr) > 0) {
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) <= 0) {
      /* Try as hostname - for simplicity, only support IP addresses */
      close(fd);
      return -1;
    }
  } else {
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  if (listen(fd, 16) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

/* ==========================================================================
 * Client Management
 * ========================================================================== */

LacedClient *laced_accept_client(int listen_fd) {
  struct sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);

  int client_fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
  if (client_fd < 0) {
    return NULL;
  }

  /* Set non-blocking */
  int flags = fcntl(client_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
  }

  LacedClient *client = safe_calloc(1, sizeof(LacedClient));
  if (!client) {
    close(client_fd);
    return NULL;
  }

  client->fd = client_fd;
  client->out_fd = -1; /* Replies go back over the socket */
  return client;
}

void laced_client_free(LacedClient *client) {
  if (!client) {
    return;
  }

  if (client->fd >= 0) {
    close(client->fd);
  }
  if (client->out_fd >= 0 && client->out_fd != client->fd) {
    close(client->out_fd);
  }
  free(client->partial_buf);
  free(client->out_buf);
  free(client);
}

uint64_t laced_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

bool laced_client_flush(LacedClient *client) {
  if (!client || client->fd < 0 || client->closed)
    return false;
  while (client->out_off < client->out_len) {
    int wfd = client->out_fd >= 0 ? client->out_fd : client->fd;
    ssize_t n = write(wfd, client->out_buf + client->out_off,
                      client->out_len - client->out_off);
    if (n > 0) {
      client->out_off += (size_t)n;
      client->out_progress_ms = laced_mono_ms();
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true; /* The event loop resumes when the socket is writable */
    client->closed = true; /* EPIPE, ECONNRESET, ... */
    return false;
  }
  client->out_off = client->out_len = 0; /* All sent */
  /* One huge reply must not pin its buffer for the rest of the session */
  if (client->out_cap > LACED_OUT_BUF_KEEP) {
    free(client->out_buf);
    client->out_buf = NULL;
    client->out_cap = 0;
  }
  return true;
}

bool laced_client_write_line(LacedClient *client, const char *line) {
  if (!client || client->fd < 0 || !line || client->closed) {
    return false;
  }
  size_t len = strlen(line);
  size_t pending = client->out_len - client->out_off;
  if (pending + len + 1 > LACED_MAX_PENDING_OUTPUT) {
    LOG_WARN("Client fd=%d is not reading its replies; dropping it",
             client->fd);
    client->closed = true;
    return false;
  }
  /* Queue the line (dropping what was already sent), then send what the
   * socket takes now; the rest goes out from the event loop. A reader that
   * makes no progress for LACED_WRITE_TIMEOUT_MS is dropped there. */
  if (client->out_off > 0) {
    memmove(client->out_buf, client->out_buf + client->out_off, pending);
    client->out_off = 0;
    client->out_len = pending;
  }
  if (client->out_len + len + 1 > client->out_cap) {
    size_t cap = client->out_cap ? client->out_cap : 4096;
    while (cap < client->out_len + len + 1)
      cap *= 2;
    client->out_buf = safe_realloc(client->out_buf, cap);
    client->out_cap = cap;
  }
  if (pending == 0)
    client->out_progress_ms = laced_mono_ms(); /* The stall clock starts now */
  memcpy(client->out_buf + client->out_len, line, len);
  client->out_buf[client->out_len + len] = '\n';
  client->out_len += len + 1;
  return laced_client_flush(client);
}

/* Take the first complete line out of the client's buffer, or NULL */
static char *take_buffered_line(LacedClient *client) {
  for (size_t i = client->partial_scanned; i < client->partial_len; i++) {
    if (client->partial_buf[i] == '\n') {
      char *line = safe_malloc(i + 1);
      memcpy(line, client->partial_buf, i);
      line[i] = '\0';
      size_t remaining = client->partial_len - i - 1;
      if (remaining > 0) {
        memmove(client->partial_buf, client->partial_buf + i + 1, remaining);
      }
      client->partial_len = remaining;
      client->partial_scanned = 0;
      return line;
    }
  }
  client->partial_scanned = client->partial_len;
  return NULL;
}

char *laced_client_try_read_line(LacedClient *client) {
  if (!client || client->fd < 0) {
    return NULL;
  }

  /* A complete request may already be buffered (several can arrive in one
   * read): serve it before touching the socket, or it would wait for the
   * client's next bytes */
  char *line = take_buffered_line(client);
  if (line || client->closed) {
    if (!line && client->closed && client->partial_len > 0) {
      /* Peer closed after an unterminated last request: hand it back */
      line = client->partial_buf;
      line[client->partial_len] = '\0';
      client->partial_buf = NULL;
      client->partial_len = client->partial_cap = client->partial_scanned = 0;
    }
    return line;
  }

  char temp[65536];
  ssize_t n = read(client->fd, temp, sizeof(temp));

  if (n == 0) {
    /* EOF: the peer closed its end. Flag the client so the event loop drops
     * it now instead of spinning on a permanently-readable fd. Buffered
     * lines are still handed out by the calls that follow. */
    client->closed = true;
    return laced_client_try_read_line(client);
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return NULL; /* No data right now */
    }
    client->closed = true; /* ECONNRESET and friends */
    return NULL;
  }

  size_t needed = client->partial_len + (size_t)n + 1;
  /* Until it has authenticated, a client gets only room for a hello */
  unsigned cap = client->authed ? LACED_MAX_REQUEST_LINE : LACED_MAX_UNAUTHED_LINE;
  if (needed > cap) {
    LOG_WARN("Client fd=%d sent a request over %u bytes; dropping it",
             client->fd, cap);
    free(client->partial_buf);
    client->partial_buf = NULL;
    client->partial_len = client->partial_cap = client->partial_scanned = 0;
    client->closed = true;
    return NULL;
  }
  if (needed > client->partial_cap) {
    size_t new_cap = client->partial_cap ? client->partial_cap * 2 : 4096;
    while (new_cap < needed)
      new_cap *= 2;
    client->partial_buf = safe_realloc(client->partial_buf, new_cap);
    client->partial_cap = new_cap;
  }

  memcpy(client->partial_buf + client->partial_len, temp, (size_t)n);
  client->partial_len += (size_t)n;

  return take_buffered_line(client);
}

/* Check if client connection is still alive */
bool laced_client_is_connected(LacedClient *client) {
  /* A zero-length recv() returns 0 for both live and closed sockets, so it
   * cannot detect hangups. The read path records EOF/errors in `closed`. */
  return client && client->fd >= 0 && !client->closed;
}
