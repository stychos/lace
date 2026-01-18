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
  struct stat st;

#ifdef __APPLE__
  /* Check for macOS sandbox environment */
  const char *sandbox_container = getenv("APP_SANDBOX_CONTAINER_ID");
  const char *home = getenv("HOME");

  if (sandbox_container && home) {
    /* Sandboxed app - use Caches directory in container */
    snprintf(path, sizeof(path), "%s/Library/Caches/laced.sock", home);
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
      return str_dup(path);
    }
    /* Return this path even if socket doesn't exist (for spawn) */
    return str_dup(path);
  }

  /* Non-sandboxed macOS - check Caches directory first */
  if (home) {
    snprintf(path, sizeof(path), "%s/Library/Caches/laced.sock", home);
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
      return str_dup(path);
    }
  }
#endif

  /* Option 1: /run/laced.sock (system-wide daemon, typically root) */
  if (stat("/run/laced.sock", &st) == 0 && S_ISSOCK(st.st_mode)) {
    return str_dup("/run/laced.sock");
  }

  /* Option 2: $XDG_RUNTIME_DIR/laced.sock (Linux user session) */
  const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
  if (xdg_runtime) {
    snprintf(path, sizeof(path), "%s/laced.sock", xdg_runtime);
    if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
      return str_dup(path);
    }
  }

  /* Option 3: /tmp/laced-{uid}.sock (universal fallback) */
  snprintf(path, sizeof(path), "/tmp/laced-%d.sock", (int)getuid());
  if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
    return str_dup(path);
  }

  /* No existing socket found - return default path for spawn mode */
#ifdef __APPLE__
  const char *home_env = getenv("HOME");
  if (home_env) {
    snprintf(path, sizeof(path), "%s/Library/Caches/laced.sock", home_env);
    return str_dup(path);
  }
#endif

  if (xdg_runtime) {
    snprintf(path, sizeof(path), "%s/laced.sock", xdg_runtime);
  } else {
    snprintf(path, sizeof(path), "/tmp/laced-%d.sock", (int)getuid());
  }
  return str_dup(path);
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

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

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
