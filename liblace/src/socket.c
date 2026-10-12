/*
 * liblace - Lace Client Library
 * Reaching the daemon: default socket path, token files, connecting (the
 * system calls are in os/)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/lace.h"
#include "../include/util/str.h"
#include "client_internal.h"
#include "os/os.h"
#include <stdlib.h>
#include <string.h>

char *lace_get_default_socket_path(void) {
  return lace_os_default_socket_path();
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
  /* Only a token file that is ours and private counts */
  char *text = lace_os_read_private_file(path, 128);
  if (!text) {
    return NULL;
  }
  text[strcspn(text, "\r\n")] = '\0';
  if (!text[0]) {
    free(text);
    return NULL;
  }
  return text;
}

bool lace_socket_owned_by_me(const char *path) {
  return lace_os_socket_owned_by_me(path);
}

bool lace_peer_is_me(int fd) { return lace_os_peer_is_me((LaceOsHandle)fd); }

bool lace_daemon_is_running(const char *socket_path) {
  char *path = socket_path ? NULL : lace_get_default_socket_path();
  bool up = lace_os_unix_listening(socket_path ? socket_path : path);
  free(path);
  return up;
}

LaceOsHandle lace_connect_unix(const char *path, int timeout_ms) {
  if (!path) {
    return LACE_OS_INVALID;
  }
  /* The default path is found automatically, so make sure it is ours: the
   * socket must belong to this user, and so must the daemon behind it.
   * An explicitly configured path (e.g. a shared socket for a group) is
   * trusted as configured. */
  char *default_path = lace_get_default_socket_path();
  bool verify = default_path && strcmp(default_path, path) == 0;
  free(default_path);
  return lace_os_connect_unix(
      path, timeout_ms > 0 ? timeout_ms : LACE_DEFAULT_CONNECT_TIMEOUT_MS, verify);
}

LaceOsHandle lace_connect_tcp(const char *host, int port, int timeout_ms) {
  return lace_os_connect_tcp(
      host, port > 0 ? port : LACE_DEFAULT_PORT,
      timeout_ms > 0 ? timeout_ms : LACE_DEFAULT_CONNECT_TIMEOUT_MS);
}
