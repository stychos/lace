/*
 * laced - Lace Database Daemon
 * SSH tunnel management
 *
 * Creates local port forwarding via the system ssh command.
 * The tunnel is transparent to the database layer -- it connects
 * to localhost:local_port which forwards to the remote db host.
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
 * Create an SSH tunnel with local port forwarding.
 *
 * Forks an ssh process: ssh -N -L localport:dbhost:dbport [-l user] [-p port] sshhost
 * Password authentication uses the SSH_ASKPASS mechanism.
 * When ssh_user/ssh_port are not set, ssh reads from ~/.ssh/config.
 *
 * @param cs          Parsed connection string with ssh=true
 * @param local_port  Output: local port to connect through
 * @param err         Output: error message on failure (caller must free)
 * @return            Tunnel handle, or NULL on failure
 */
SshTunnel *ssh_tunnel_create(const ConnString *cs, int *local_port, char **err);

/*
 * Destroy an SSH tunnel and kill the ssh process.
 *
 * @param tunnel  Tunnel handle (NULL is safe)
 */
void ssh_tunnel_destroy(SshTunnel *tunnel);

/*
 * Check if the tunnel ssh process is still running.
 *
 * @param tunnel  Tunnel handle
 * @return        true if process is alive
 */
bool ssh_tunnel_is_alive(const SshTunnel *tunnel);

#endif /* LACED_SSH_TUNNEL_H */
