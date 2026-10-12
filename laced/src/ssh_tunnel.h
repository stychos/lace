/*
 * laced - Lace Database Daemon
 * SSH tunnel management
 *
 * Forwards a Unix socket in a private directory to the database through
 * the system ssh command. The driver connects to that socket (connection
 * string option lace_socket, set by laced_session_connect).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_SSH_TUNNEL_H
#define LACED_SSH_TUNNEL_H

#include <stdbool.h>
#include <util/connstr.h>

/* Opaque SSH tunnel handle */
typedef struct SshTunnel SshTunnel;

/*
 * Create an SSH tunnel forwarding a Unix socket in a private directory (see
 * ssh_tunnel_socket_path) to the database.
 *
 * Forks an ssh process: ssh -N -L socket:dbhost:dbport [-l user] [-p port] sshhost
 * Password authentication uses the SSH_ASKPASS mechanism.
 * When ssh_user/ssh_port are not set, ssh reads from ~/.ssh/config.
 *
 * @param cs          Parsed connection string with ssh=true
 * @param err         Output: error message on failure (caller must free)
 * @return            Tunnel handle, or NULL on failure
 */
SshTunnel *ssh_tunnel_create(const ConnString *cs, char **err);

/*
 * Destroy an SSH tunnel and kill the ssh process.
 *
 * @param tunnel  Tunnel handle (NULL is safe)
 */
void ssh_tunnel_destroy(SshTunnel *tunnel);

/*
 * Check if the tunnel ssh process is still running (reaps it if not).
 *
 * @param tunnel  Tunnel handle
 * @return        true if process is alive
 */
bool ssh_tunnel_is_alive(SshTunnel *tunnel);

/*
 * Start ssh again if it exited (server gone, keepalive timeout), on the
 * same socket path so the connection string through it stays valid.
 * The database connection through the old tunnel is gone either way.
 */
bool ssh_tunnel_restart(SshTunnel *tunnel, char **err);

/* Ask ssh to exit (SIGTERM) without waiting or freeing anything */
void ssh_tunnel_signal_exit(SshTunnel *tunnel);

/* Daemon shutdown: tunnel setups in progress stop waiting and fail. */
void ssh_tunnel_abort_pending(void);

/* Where the tunnel goes: the SSH server, and the Unix socket the database
 * connection uses (for PostgreSQL <dir>/.s.PGSQL.<port>, so libpq finds it
 * with host=<dir>) */
const char *ssh_tunnel_host(const SshTunnel *tunnel);
const char *ssh_tunnel_socket_path(const SshTunnel *tunnel);

/* Remove what crashed daemons left in the socket directory: lace-tun-*
 * directories whose owning daemon is gone, and askpass files of older
 * daemons. Called once at startup. */
void ssh_tunnel_sweep_stale(void);

#endif /* LACED_SSH_TUNNEL_H */
