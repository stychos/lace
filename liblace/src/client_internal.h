/*
 * liblace - Lace Client Library
 * Private client structure shared by client.c and rpc.c.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_CLIENT_INTERNAL_H
#define LIBLACE_CLIENT_INTERNAL_H

#include "../include/lace.h"
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>

/* Maximum tracked connections */
#define LACE_CLIENT_MAX_CONNECTIONS 64

/* Connection tracking entry */
typedef struct {
  int conn_id;
  LaceDriver driver;
} LaceConnEntry;

/* Client structure */
struct lace_client {
  pid_t daemon_pid;       /* Daemon process ID (0 if connected to socket) */
  FILE *to_daemon;        /* Write side of the daemon socket */
  FILE *from_daemon;      /* Read side of the daemon socket */
  int socket_fd;          /* Socket fd (-1 if not connected) */
  LaceConnMode conn_mode; /* Connection mode */
  int timeout_ms;         /* Request timeout */
  int64_t next_id;        /* Next request ID */
  bool connected;         /* Whether the daemon link is usable */

  /* Serializes RPC calls: one request/response pair in flight at a time.
   * The TUI calls into liblace from worker threads and the main thread. */
  pthread_mutex_t lock;

  /* Bumped whenever the daemon was respawned: every conn_id from before is
   * gone and callers must reconnect their databases. */
  uint64_t generation;

  /* Identity of the daemon behind the socket ("version" -> "instance").
   * Compared after every reconnect: a different daemon means a fresh,
   * unrelated connection pool, so the generation must move on. */
  char daemon_instance[64];

  /* Unix socket this client talks to (reconnects and cancel side-channels
   * must use the same daemon, not whatever the default path resolves to) */
  char *socket_path;

  /* Connection tracking (driver type per conn_id, for SQL dialect) */
  LaceConnEntry connections[LACE_CLIENT_MAX_CONNECTIONS];
  size_t num_connections;
};

/* Initialize the fields every constructor must set (lock, ids, fds). */
void lace_client_init_common(lace_client_t *client);

/* Attach FILE streams to a connected socket fd. Takes ownership of fd.
 * Returns false (fd closed) on failure. */
bool lace_client_attach_socket(lace_client_t *client, int fd);

/* Close the socket streams without touching the daemon process. */
void lace_client_close_streams(lace_client_t *client);

/* Re-establish the daemon link after it dropped: reconnect to the client's
 * socket, or spawn a fresh daemon there if none is listening (default path
 * only). Returns 0 on failure, 1 when reconnected to a listening daemon
 * (possibly a different one: verify its instance), 2 when it spawned one.
 * Caller holds client->lock. */
int lace_client_reattach(lace_client_t *client);

/* True if the calling thread declared (lace_expect_generation) a different
 * generation for this client than the current one. */
bool lace_thread_generation_mismatch(const lace_client_t *client);

/* Remember the Unix socket path this client is attached to. */
void lace_client_set_socket_path(lace_client_t *client, const char *path);

/* Record an error for the calling thread (see lace_client_error). */
void lace_set_thread_error(const char *msg);

#endif /* LIBLACE_CLIENT_INTERNAL_H */
