/*
 * laced - Lace Database Daemon
 * Server interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_SERVER_H
#define LACED_SERVER_H

#include <sys/types.h>
#include "constants.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Forward declarations */
typedef struct LacedServer LacedServer;
typedef struct AsyncQueue AsyncQueue;

/* ==========================================================================
 * Transport Types
 * ========================================================================== */

/* Transport mode for daemon communication */
typedef enum {
  LACED_TRANSPORT_STDIO, /* stdin/stdout (default, single client) */
  LACED_TRANSPORT_UNIX,  /* Unix domain socket (multi-client) */
  LACED_TRANSPORT_TCP    /* TCP socket (multi-client) */
} LacedTransport;

/* Client connection (for socket modes) */
typedef struct LacedClient {
  int fd;                     /* Socket, or stdin for the stdio client */
  int out_fd;                 /* Where replies go: -1 for fd, stdout (stdio) */
  int poll_in, poll_out;      /* Its entries in the event loop's pollfd array
                                 (-1: none this pass) */
  bool closed;                /* Peer hung up or socket errored; remove on next pass */
  bool readable;              /* Set by the event loop's poll() each pass */
  bool authed;                /* May call methods (TCP: after the handshake) */
  int auth_failures;          /* Challenges and bad proofs; dropped after 3 */
  char auth_nc[65];           /* TCP: pending challenge, client nonce ("" = none) */
  char auth_nd[33];           /* TCP: pending challenge, daemon nonce */
  char owner[LACED_OWNER_MAX];             /* Owner key for its DB connections: "u:<id>"
                                 from a liblace "hello", else "s:<serial>" */
  uint64_t serial;            /* Unique per accepted client; pointers get reused */
  char *partial_buf;          /* Partial line buffer */
  size_t partial_len;         /* Current length in buffer */
  size_t partial_cap;         /* Buffer capacity */
  size_t partial_scanned;     /* Bytes already searched for '\n' */
  /* Replies not yet accepted by the socket: the event loop never blocks
   * on a slow reader, it flushes when poll() reports the socket writable */
  char *out_buf;
  size_t out_off;             /* Bytes of out_buf already written */
  size_t out_len;
  size_t out_cap;
  uint64_t out_progress_ms;   /* Last time a write made progress */
  bool has_buffered;          /* Requests left buffered after its turn */
  struct LacedClient *next;   /* Next client in linked list */
  /* Activity tracking */
  time_t connected_at;        /* Connection timestamp */
  time_t last_activity;       /* Last request timestamp */
  uint64_t requests_count;    /* Total requests handled */
} LacedClient;

/* ==========================================================================
 * Server Lifecycle
 * ========================================================================== */

/*
 * Create a new daemon server instance.
 * Initializes database drivers and connection pool.
 *
 * @return Server handle, or NULL on failure
 */
LacedServer *laced_server_create(void);

/* How long a client's database connections outlive its last socket before
 * they are closed (default LACED_ORPHAN_GRACE_S) */
void laced_server_set_orphan_grace(LacedServer *server, int seconds);

/*
 * Destroy server and cleanup all resources.
 *
 * @param server  Server handle (NULL is safe)
 */
void laced_server_destroy(LacedServer *server);

/*
 * Get the async query queue from a server.
 *
 * @param server  Server handle
 * @return        Async queue, or NULL if invalid
 */
AsyncQueue *laced_server_get_async_queue(LacedServer *server);

/*
 * Unix socket mode: also let members of this group connect (the socket is
 * created 0660 and given to the group; the default is owner-only 0600).
 * Peers of other users are refused either way. Root may always connect.
 */
void laced_server_set_socket_group(LacedServer *server, gid_t group);

/*
 * TCP mode: where to write the authentication token (default: next to the
 * default socket, laced-<port>.token). Clients must send it in "hello".
 */
void laced_server_set_token_file(LacedServer *server, const char *path);

/* ==========================================================================
 * Server Execution
 * ========================================================================== */

/*
 * Run server using stdin/stdout for communication.
 * Blocks until shutdown is requested or connection closes.
 *
 * @param server            Server handle
 * @param shutdown_flag     Pointer to flag that signals shutdown
 * @return                  0 on success, non-zero on error
 */
int laced_server_run_stdio(LacedServer *server,
                           volatile sig_atomic_t *shutdown_flag);

/*
 * Run server listening on a Unix domain socket.
 * Supports multiple concurrent clients.
 *
 * @param server            Server handle
 * @param socket_path       Path to Unix socket (NULL for default)
 * @param max_clients       Maximum concurrent clients (0 for default 64)
 * @param idle_timeout      Shutdown after N seconds with no clients (0=disabled)
 * @param shutdown_flag     Pointer to flag that signals shutdown
 * @return                  0 on success, non-zero on error
 */
int laced_server_run_unix(LacedServer *server, const char *socket_path,
                          size_t max_clients, int idle_timeout,
                          volatile sig_atomic_t *shutdown_flag);

/*
 * Run server listening on a TCP socket.
 * Supports multiple concurrent clients.
 *
 * @param server            Server handle
 * @param bind_addr         Address to bind (NULL for localhost)
 * @param port              Port number (0 for default 7433)
 * @param max_clients       Maximum concurrent clients (0 for default 64)
 * @param idle_timeout      Shutdown after N seconds with no clients (0=disabled)
 * @param shutdown_flag     Pointer to flag that signals shutdown
 * @return                  0 on success, non-zero on error
 */
int laced_server_run_tcp(LacedServer *server, const char *bind_addr, int port,
                         size_t max_clients, int idle_timeout,
                         volatile sig_atomic_t *shutdown_flag);

/* ==========================================================================
 * Socket Utilities
 * ========================================================================== */

/*
 * Get the default Unix socket path (lace_get_default_socket_path, the same
 * per-user path clients use) and create its private directory.
 *
 * @return  Allocated string with socket path (caller must free), or NULL on error
 */
char *laced_get_default_socket_path(void);

/*
 * Remove a stale socket file if no daemon is listening.
 *
 * @param path  Path to socket file
 * @return      true if socket was stale and removed (or didn't exist), false if daemon is running
 */
bool laced_remove_stale_socket(const char *path);

/* Listening sockets (socket.c). -1 on failure. A Unix socket is created
 * 0600, or 0660 owned by group when group is not (gid_t)-1. */
int laced_create_unix_socket(const char *path, gid_t group);
int laced_create_tcp_socket(const char *bind_addr, int port);

/* May this peer use the daemon: its own user, root, or a member of group */
bool laced_peer_allowed(int fd, gid_t group);

/* Clients (socket.c) */
LacedClient *laced_accept_client(int listen_fd);
void laced_client_free(LacedClient *client);
bool laced_client_write_line(LacedClient *client, const char *line);
bool laced_client_flush(LacedClient *client);

/* Tell the parent of "laced -d" that the daemon is listening (main.c;
 * no-op without -d) */
void laced_signal_ready(void);

/* Monotonic milliseconds (stall and timeout clocks) */
uint64_t laced_mono_ms(void);
char *laced_client_try_read_line(LacedClient *client);
bool laced_client_is_connected(LacedClient *client);

#endif /* LACED_SERVER_H */
