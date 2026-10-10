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

/* Idle daemon links kept open for reuse; more are opened on demand */
#define LACE_CLIENT_MAX_IDLE_LINKS 4

/* Connection tracking entry */
typedef struct {
  int conn_id;
  LaceDriver driver;
} LaceConnEntry;

/*
 * One connection to the daemon: a socket, or the pipe pair of a stdio
 * daemon. A call borrows a link for its whole request/response exchange,
 * so calls on different links never wait for each other.
 */
typedef struct LaceLink {
  int rfd;              /* Read side (the socket, or the daemon's stdout) */
  int wfd;              /* Write side (the socket, or the daemon's stdin) */
  bool is_socket;
  bool initialized;     /* "hello" and identity check done */
  int64_t next_id;      /* Request ids are per link */
  char *buf;            /* Buffered reads */
  size_t len, cap, scanned;
  struct LaceLink *next;
} LaceLink;

/* Client structure */
struct lace_client {
  pid_t daemon_pid;       /* Daemon process ID (0 if connected to socket) */
  LaceConnMode conn_mode; /* Connection mode */
  int timeout_ms;         /* Reply timeout for control calls */
  int query_timeout_ms;   /* Reply timeout for "query" (0 = none) */
  bool connected;         /* A daemon was reached (links can be reopened) */
  bool owns_daemon;       /* Spawned in stdio mode: exits with this client */
  int idle_timeout;       /* --idle-timeout passed to daemons we spawn */

  /* Guards everything below: the link pool, generation and identity, the
   * socket path and the connection table. Never held during network I/O. */
  pthread_mutex_t lock;
  pthread_cond_t link_released; /* Single-link (stdio) callers wait on it */

  LaceLink *idle_links;   /* Links ready for reuse */
  size_t num_idle;
  size_t num_links;       /* Open links, idle or borrowed */
  bool single_link;       /* stdio: no second link to the daemon exists */

  /* Random id sent in "hello" on every link: the daemon keeps this
   * client's database connections while any link with this id is open */
  char client_id[40];

  /* Bumped whenever the daemon was respawned: every conn_id from before is
   * gone and callers must reconnect their databases. */
  uint64_t generation;

  /* Identity of the daemon behind the socket ("version" -> "instance").
   * Checked on every new link: a different daemon means a fresh,
   * unrelated connection pool, so the generation must move on. */
  char daemon_instance[64];

  /* Unix socket this client talks to (new links and respawns use it) */
  char *socket_path;
  /* TCP endpoint this client talks to (new links use it) */
  char *tcp_host;
  int tcp_port;
  char *token;            /* TCP authentication token (see lace.h) */

  /* Connection tracking (driver type per conn_id, for SQL dialect) */
  LaceConnEntry connections[LACE_CLIENT_MAX_CONNECTIONS];
  size_t num_connections;
};

/* Initialize the fields every constructor must set (lock, ids, timeouts). */
void lace_client_init_common(lace_client_t *client);

/* Wrap file descriptors in a link (takes ownership of both). */
LaceLink *lace_link_new(int rfd, int wfd, bool is_socket);

/* Close a link's descriptors and free it. */
void lace_link_free(LaceLink *link);

/* Add a connected socket as an idle link. Takes ownership of fd.
 * Returns false (fd closed) on failure. */
bool lace_client_attach_socket(lace_client_t *client, int fd);

/* Close every idle link without touching the daemon process. */
void lace_client_close_streams(lace_client_t *client);

/* Open a new link to the client's daemon: connect to its Unix socket or TCP
 * endpoint, or spawn a fresh daemon on the default socket path if none is
 * listening there. Returns 0 on failure, 1 when connected to a listening
 * daemon (possibly a different one: verify its instance), 2 when it spawned
 * one. Must be called without client->lock held. */
int lace_client_open_link(lace_client_t *client, LaceLink **out);

/* True if the calling thread declared (lace_expect_generation) a different
 * generation for this client than the current one. */
bool lace_thread_generation_mismatch(const lace_client_t *client);

/* Remember the Unix socket path this client is attached to. */
void lace_client_set_socket_path(lace_client_t *client, const char *path);

/* Record the transaction state a reply reported, for the calling thread
 * (see lace_last_transaction_state). */
void lace_set_thread_tx_state(int state);

/* SQL literal for val in driver's dialect, exactly as lace_update and
 * friends write it (caller frees). For SQL liblace builds elsewhere. */
char *lace_value_to_sql_literal(const LaceValue *val, LaceDriver driver);

/* Record an error for the calling thread (see lace_client_error). */
void lace_set_thread_error(const char *msg);

#endif /* LIBLACE_CLIENT_INTERNAL_H */
