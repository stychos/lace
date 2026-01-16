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

/* Forward declarations */
typedef struct LacedServer LacedServer;
typedef struct AsyncQueue AsyncQueue;

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

#endif /* LACED_SERVER_H */
