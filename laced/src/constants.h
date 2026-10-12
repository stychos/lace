/*
 * laced daemon
 * Tuning constants: the daemon's limits and timeouts, in one place. Also
 * re-exports liblace/include/constants.h (result limits shared with the
 * client).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_CONSTANTS_H
#define LACED_CONSTANTS_H

#include "../../liblace/include/constants.h"

#define LACED_VERSION "0.1.0"
#define LACED_PROTOCOL_VERSION "1.0"

/* ==========================================================================
 * Clients
 * ========================================================================== */

/* Default for --max-clients */
#define LACED_DEFAULT_MAX_CLIENTS 64

/* Default TCP port (the same as liblace's LACE_DEFAULT_PORT) */
#define LACED_DEFAULT_PORT 7433

/* Size of a client's owner key ("u:<hello id>" or "s:<serial>") */
#define LACED_OWNER_MAX 64

/* Requests served from one client before the others get their turn */
#define LACED_REQUESTS_PER_TURN 16

/* A socket that never sent a request is dropped after this many seconds */
#define LACED_CLIENT_IDLE_TIMEOUT_S 300

/* A TCP client must authenticate (hello with the token) this soon after
 * connecting, and may send requests no longer than this until it has */
#define LACED_AUTH_TIMEOUT_S 10
#define LACED_MAX_UNAUTHED_LINE (64u * 1024u)
/* TCP: unauthenticated clients at once; a new one evicts the oldest, so
 * idle connections cannot hold every client slot */
#define LACED_MAX_UNAUTHED_CLIENTS 8
/* A statement waits up to this long for a cancel of the one before it to
 * finish (so the cancel cannot reach the new statement) */
#define LACED_CANCEL_SETTLE_S 5

/* Requests longer than this are refused: the client is dropped instead of
 * growing the daemon's buffer without bound */
#define LACED_MAX_REQUEST_LINE (64u * 1024u * 1024u)

/* Replies queued for one client at most: a client that reads nothing while
 * results pile up is dropped instead of holding the daemon's memory */
#define LACED_MAX_PENDING_OUTPUT ((size_t)LACE_MAX_RESULT_BYTES * 4)
/* A client's output buffer larger than this is freed once drained */
#define LACED_OUT_BUF_KEEP ((size_t)1 << 20)

/* A client whose queued replies make no progress for this long is dropped
 * (a stall, not a whole transfer: a slow reader that keeps draining stays) */
#define LACED_WRITE_TIMEOUT_MS 5000

/* ==========================================================================
 * Workers and connections
 * ========================================================================== */

/* Worker threads at most, in all and for one client; a request beyond
 * either fails with LACE_ERR_DAEMON_BUSY instead of starting a thread */
#define LACED_MAX_WORKERS 256
#define LACED_MAX_JOBS_PER_CLIENT 32

/* Database connections one daemon holds at most */
#define LACED_MAX_CONNECTIONS 64

/* How long a client's DB connections outlive its last socket (enough for
 * liblace to reattach after a transient drop or daemon hiccup) */
#define LACED_ORPHAN_GRACE_S 60

/* How long shutdown waits for running queries to stop after cancelling them */
#define LACED_SHUTDOWN_WAIT_MS 5000

/* ==========================================================================
 * Drivers and tunnels
 * ========================================================================== */

/* Registered database drivers at most */
#define LACED_MAX_DRIVERS 16

/* Timeout for opening a PostgreSQL connection, also on a reconnect (PQreset) */
#define LACED_PG_CONNECT_TIMEOUT_S 10

/* Timeout for opening a MySQL/MariaDB connection, also on a reconnect */
#define LACED_MYSQL_CONNECT_TIMEOUT_S 10

/* A MySQL connection idle longer than this is pinged before a statement
 * (wait_timeout, server restart) */
#define LACED_MYSQL_IDLE_PING_S 30

/* Timeout of the side connection that sends KILL QUERY */
#define LACED_MYSQL_CANCEL_TIMEOUT_S 3

/* How long SSH tunnel setup waits for the local port to accept */
#define LACED_TUNNEL_READY_TIMEOUT_MS 30000

#endif /* LACED_CONSTANTS_H */
