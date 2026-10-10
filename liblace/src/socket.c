/*
 * liblace - Lace Client Library
 * Socket utilities for Unix and TCP socket communication
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/lace.h"
#include "../include/util/str.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* A write to a socket whose daemon end has closed must fail with EPIPE,
 * not kill the client process with SIGPIPE. */
static void socket_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

/* ==========================================================================
 * Socket Path Discovery
 * ========================================================================== */

/*
 * Get default socket path with platform-specific handling.
 * Priority:
 *   macOS sandbox: ~/Library/Caches/laced.sock
 *   Linux/other:   $XDG_RUNTIME_DIR/laced.sock or /tmp/laced-{uid}.sock
 *
 * This function checks if a socket exists at each candidate path and
 * returns the first one found, or the default path for spawn mode.
 */
char *lace_get_default_socket_path(void) {
  char path[512];

  /* One canonical location per user. The client never searches shared
   * places such as /tmp: another local user could plant a socket there and
   * collect the connection strings (passwords) sent to it. */
#ifdef __APPLE__
  const char *home = getenv("HOME");
  if (home && home[0]) {
    snprintf(path, sizeof(path), "%s/Library/Caches/laced.sock", home);
    return str_dup(path);
  }
#endif

  /* A system daemon run by root */
  if (geteuid() == 0) {
    return str_dup("/run/laced.sock");
  }

  /* The user's private runtime directory, if it really is private */
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  struct stat st;
  if (xdg && xdg[0] && stat(xdg, &st) == 0 && S_ISDIR(st.st_mode) &&
      st.st_uid == geteuid() && (st.st_mode & 077) == 0) {
    snprintf(path, sizeof(path), "%s/laced.sock", xdg);
    return str_dup(path);
  }

  /* A 0700 directory in the user's home (the daemon creates it) */
  const char *home_dir = getenv("HOME");
  if (home_dir && home_dir[0]) {
    snprintf(path, sizeof(path), "%s/.cache/lace/laced.sock", home_dir);
    return str_dup(path);
  }

  /* Last resort: a private per-user directory under /tmp (the daemon
   * creates it 0700; the client verifies who owns the socket) */
  snprintf(path, sizeof(path), "/tmp/lace-%d/laced.sock", (int)geteuid());
  return str_dup(path);
}

char *lace_default_token_path(int port) {
  char *sock = lace_get_default_socket_path();
  if (!sock) {
    return NULL;
  }
  char *slash = strrchr(sock, '/');
  if (slash) {
    *slash = '\0';
  }
  char *path = str_printf("%s/laced-%d.token", slash ? sock : ".",
                          port > 0 ? port : LACE_DEFAULT_PORT);
  free(sock);
  return path;
}

char *lace_read_token_file(const char *path) {
  if (!path) {
    return NULL;
  }
  /* Only a token file that is ours and private counts */
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return NULL;
  }
  struct stat st;
  char buf[129];
  ssize_t n = -1;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
      (st.st_mode & 077) == 0) {
    n = read(fd, buf, sizeof(buf) - 1);
  }
  close(fd);
  if (n <= 0) {
    return NULL;
  }
  buf[n] = '\0';
  buf[strcspn(buf, "\r\n")] = '\0';
  return buf[0] ? str_dup(buf) : NULL;
}

bool lace_socket_owned_by_me(const char *path) {
  struct stat st;
  return path && lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) &&
         st.st_uid == geteuid();
}

bool lace_peer_is_me(int fd) {
  uid_t peer = (uid_t)-1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
  gid_t gid;
  if (getpeereid(fd, &peer, &gid) != 0) {
    return false;
  }
#elif defined(SO_PEERCRED)
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return false;
  }
  peer = cred.uid;
#else
  (void)fd;
  return true; /* No way to ask: rely on the socket's ownership check */
#endif
  return peer == geteuid();
}

/* ==========================================================================
 * Daemon Detection
 * ========================================================================== */

bool lace_daemon_is_running(const char *socket_path) {
  char *path = NULL;
  bool allocated = false;

  if (socket_path) {
    path = (char *)socket_path;
  } else {
    path = lace_get_default_socket_path();
    if (!path) {
      return false;
    }
    allocated = true;
  }

  /* Check if socket file exists */
  struct stat st;
  if (stat(path, &st) < 0 || !S_ISSOCK(st.st_mode)) {
    if (allocated) free(path);
    return false;
  }

  /* Try to connect */
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (allocated) free(path);
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (allocated) free(path);

  /* Set non-blocking for quick timeout */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (result < 0 && errno != EINPROGRESS) {
    close(fd);
    return false;
  }

  if (result < 0) {
    /* Wait for connection with short timeout */
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    result = poll(&pfd, 1, 100); /* 100ms timeout */
    if (result <= 0 || !(pfd.revents & POLLOUT)) {
      close(fd);
      return false;
    }

    /* Check connection result */
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
      close(fd);
      return false;
    }
  }

  close(fd);
  return true;
}

/* ==========================================================================
 * Socket Connection
 * ========================================================================== */

int lace_connect_unix(const char *path, int timeout_ms) {
  if (!path) {
    return -1;
  }

  /* The default path is found automatically, so make sure it is ours: the
   * socket must belong to this user, and so must the daemon behind it.
   * An explicitly configured path (e.g. a shared socket for a group) is
   * trusted as configured. */
  char *default_path = lace_get_default_socket_path();
  bool verify = default_path && strcmp(default_path, path) == 0;
  free(default_path);
  if (verify && !lace_socket_owned_by_me(path)) {
    errno = EPERM;
    return -1;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  socket_no_sigpipe(fd);

  /* Set non-blocking for timeout handling */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (result < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }

  if (result < 0) {
    /* Wait for connection */
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    int poll_timeout = timeout_ms > 0 ? timeout_ms : LACE_DEFAULT_CONNECT_TIMEOUT_MS;
    result = poll(&pfd, 1, poll_timeout);

    if (result <= 0) {
      close(fd);
      return -1;
    }

    /* Check connection result */
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
      close(fd);
      return -1;
    }
  }

  /* Restore blocking mode */
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags);
  }

  if (verify && !lace_peer_is_me(fd)) {
    close(fd);
    errno = EPERM;
    return -1;
  }

  return fd;
}

int lace_connect_tcp(const char *host, int port, int timeout_ms) {
  if (port <= 0) {
    port = LACE_DEFAULT_PORT;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  socket_no_sigpipe(fd);

  /* Set non-blocking for timeout handling */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);

  if (!host || strlen(host) == 0) {
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    /* Try hostname resolution */
    struct hostent *he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET) {
      close(fd);
      return -1;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  }

  int result = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (result < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }

  if (result < 0) {
    /* Wait for connection */
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    int poll_timeout = timeout_ms > 0 ? timeout_ms : LACE_DEFAULT_CONNECT_TIMEOUT_MS;
    result = poll(&pfd, 1, poll_timeout);

    if (result <= 0) {
      close(fd);
      return -1;
    }

    /* Check connection result */
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
      close(fd);
      return -1;
    }
  }

  /* Restore blocking mode */
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags);
  }

  return fd;
}

/* ==========================================================================
 * Daemon Discovery and Connection
 * ========================================================================== */

/*
 * Find and connect to a running daemon.
 * Searches: Unix socket, then TCP on localhost.
 */
int lace_find_daemon_socket(void) {
  /* Try Unix socket first */
  char *socket_path = lace_get_default_socket_path();
  if (socket_path) {
    int fd = lace_connect_unix(socket_path, 1000);
    free(socket_path);
    if (fd >= 0) {
      return fd;
    }
  }

  /* Try TCP on localhost */
  int fd = lace_connect_tcp("127.0.0.1", LACE_DEFAULT_PORT, 1000);
  if (fd >= 0) {
    return fd;
  }

  return -1;
}
