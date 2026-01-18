/*
 * laced - Lace Database Daemon
 * Socket utilities for Unix and TCP socket communication
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "server.h"
#include <util/mem.h>
#include <util/str.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Default port for TCP mode */
#define LACED_DEFAULT_PORT 7433

/* ==========================================================================
 * Socket Path Resolution
 * ========================================================================== */

/*
 * Get default socket path with platform-specific handling.
 * Priority:
 *   macOS sandbox: ~/Library/Caches/laced.sock
 *   Linux/other:   $XDG_RUNTIME_DIR/laced.sock or /tmp/laced-{uid}.sock
 */
char *laced_get_default_socket_path(void) {
  char path[512];

#ifdef __APPLE__
  /* Check for macOS sandbox environment */
  const char *sandbox_container = getenv("APP_SANDBOX_CONTAINER_ID");
  const char *home = getenv("HOME");

  if (sandbox_container && home) {
    /* Sandboxed app - use Caches directory in container */
    snprintf(path, sizeof(path), "%s/Library/Caches/laced.sock", home);
    return str_dup(path);
  }

  /* Non-sandboxed macOS - prefer user's Caches directory */
  if (home) {
    snprintf(path, sizeof(path), "%s/Library/Caches/laced.sock", home);
    /* Check if we can write to the directory */
    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/Library/Caches", home);
    if (access(cache_dir, W_OK) == 0) {
      return str_dup(path);
    }
  }
#endif

  /* Option 1: /run/laced.sock (if writable, typically root on Linux) */
  if (access("/run", W_OK) == 0) {
    return str_dup("/run/laced.sock");
  }

  /* Option 2: $XDG_RUNTIME_DIR/laced.sock (Linux user session) */
  const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
  if (xdg_runtime && access(xdg_runtime, W_OK) == 0) {
    snprintf(path, sizeof(path), "%s/laced.sock", xdg_runtime);
    return str_dup(path);
  }

  /* Option 3: /tmp/laced-{uid}.sock (universal fallback) */
  snprintf(path, sizeof(path), "/tmp/laced-%d.sock", (int)getuid());
  return str_dup(path);
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
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

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
 * Socket Creation
 * ========================================================================== */

int laced_create_unix_socket(const char *path) {
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
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  /* Set socket permissions to allow user access */
  chmod(path, 0600);

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
  return client;
}

void laced_client_free(LacedClient *client) {
  if (!client) {
    return;
  }

  if (client->fd >= 0) {
    close(client->fd);
  }
  free(client->partial_buf);
  free(client);
}

bool laced_client_write_line(LacedClient *client, const char *line) {
  if (!client || client->fd < 0 || !line) {
    return false;
  }

  size_t len = strlen(line);
  size_t total = len + 1; /* Include newline */

  /* Allocate buffer for line + newline */
  char *buf = safe_malloc(total);
  if (!buf) {
    return false;
  }
  memcpy(buf, line, len);
  buf[len] = '\n';

  /* Write in a loop to handle partial writes */
  size_t written = 0;
  while (written < total) {
    ssize_t n = write(client->fd, buf + written, total - written);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Socket buffer full, try again */
        usleep(1000);
        continue;
      }
      free(buf);
      return false;
    }
    written += (size_t)n;
  }

  free(buf);
  return true;
}

char *laced_client_try_read_line(LacedClient *client) {
  if (!client || client->fd < 0) {
    return NULL;
  }

  /* Read available data */
  char temp[4096];
  ssize_t n = read(client->fd, temp, sizeof(temp));

  if (n <= 0) {
    if (n == 0) {
      /* EOF - return any partial data as final line */
      if (client->partial_len > 0) {
        char *line = client->partial_buf;
        line[client->partial_len] = '\0';
        client->partial_buf = NULL;
        client->partial_len = 0;
        client->partial_cap = 0;
        return line;
      }
    }
    return NULL;
  }

  /* Ensure buffer capacity */
  size_t needed = client->partial_len + (size_t)n + 1;
  if (needed > client->partial_cap) {
    size_t new_cap = client->partial_cap ? client->partial_cap * 2 : 4096;
    while (new_cap < needed)
      new_cap *= 2;
    char *new_buf = safe_realloc(client->partial_buf, new_cap);
    if (!new_buf) {
      return NULL;
    }
    client->partial_buf = new_buf;
    client->partial_cap = new_cap;
  }

  /* Append new data */
  memcpy(client->partial_buf + client->partial_len, temp, (size_t)n);
  client->partial_len += (size_t)n;

  /* Look for newline */
  for (size_t i = 0; i < client->partial_len; i++) {
    if (client->partial_buf[i] == '\n') {
      /* Extract complete line */
      size_t line_len = i;
      char *line = safe_malloc(line_len + 1);
      if (!line) {
        return NULL;
      }
      memcpy(line, client->partial_buf, line_len);
      line[line_len] = '\0';

      /* Shift remaining data */
      size_t remaining = client->partial_len - i - 1;
      if (remaining > 0) {
        memmove(client->partial_buf, client->partial_buf + i + 1, remaining);
      }
      client->partial_len = remaining;

      return line;
    }
  }

  return NULL; /* No complete line yet */
}

/* Check if client connection is still alive */
bool laced_client_is_connected(LacedClient *client) {
  if (!client || client->fd < 0) {
    return false;
  }

  /* Try a zero-byte read to check connection status */
  char buf;
  ssize_t n = recv(client->fd, &buf, 0, MSG_PEEK | MSG_DONTWAIT);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true; /* Socket is fine, just no data */
    }
    return false; /* Connection error */
  }
  return true;
}
