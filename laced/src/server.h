/*
 * laced - Lace Database Daemon
 * Server interface
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_SERVER_H
#define LACED_SERVER_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

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
  int fd;                     /* Client socket file descriptor */
  char *partial_buf;          /* Partial line buffer */
  size_t partial_len;         /* Current length in buffer */
  size_t partial_cap;         /* Buffer capacity */
  struct LacedClient *next;   /* Next client in linked list */
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
 * @param shutdown_flag     Pointer to flag that signals shutdown
 * @return                  0 on success, non-zero on error
 */
int laced_server_run_unix(LacedServer *server, const char *socket_path,
                          size_t max_clients,
                          volatile sig_atomic_t *shutdown_flag);

/*
 * Run server listening on a TCP socket.
 * Supports multiple concurrent clients.
 *
 * @param server            Server handle
 * @param bind_addr         Address to bind (NULL for localhost)
 * @param port              Port number (0 for default 7433)
 * @param max_clients       Maximum concurrent clients (0 for default 64)
 * @param shutdown_flag     Pointer to flag that signals shutdown
 * @return                  0 on success, non-zero on error
 */
int laced_server_run_tcp(LacedServer *server, const char *bind_addr, int port,
                         size_t max_clients,
                         volatile sig_atomic_t *shutdown_flag);

/* ==========================================================================
 * Socket Utilities
 * ========================================================================== */

/*
 * Get the default Unix socket path.
 * Priority: /run/laced.sock (if writable) > $XDG_RUNTIME_DIR/laced.sock > /tmp/laced-{uid}.sock
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

#endif /* LACED_SERVER_H */
