#ifndef LACE_NC_CONNECTION_H
#define LACE_NC_CONNECTION_H
#include "conn/jobs.h"
#include "conn/mainq.h"
#include <config/history.h>
#include <lace.h>

/* One open database connection and its worker thread. The daemon runs one
 * statement at a time per database connection, so every liblace call for a
 * connection goes through its own worker, in order, as an NcJob. Completions
 * run on the main thread through the MainQueue. Everything except the
 * lock-guarded fields is main-thread only. */
typedef struct NcConnection NcConnection;

typedef enum { NC_TX_NONE = 0, NC_TX_OPEN = 1, NC_TX_FAILED = 2, NC_TX_LOST = 3 } NcTxState;
typedef enum {
  NC_CONN_EV_TX, NC_CONN_EV_TABLES, NC_CONN_EV_NOTICE, NC_CONN_EV_BUSY, NC_CONN_EV_CLOSED
} NcConnEvent;
typedef void (*NcConnListener)(NcConnection *c, NcConnEvent ev, const char *message, void *data);

NcConnection *nc_connection_new(lace_client_t *client, MainQueue *q, int conn_id,
                                const char *connstr, const char *saved_id, const char *name,
                                int history_mode, int history_max);
void nc_connection_set_listener(NcConnection *c, NcConnListener fn, void *data);
/* Queue a job (it is unreffed after done). The done callback is skipped once
 * owner (may be NULL) is released. */
void nc_connection_submit(NcConnection *c, NcJob *job, NcOwner *owner, NcJobDone done, void *data);
/* Same; destroy frees data once the job is over, whether or not done ran */
void nc_connection_submit_full(NcConnection *c, NcJob *job, NcOwner *owner, NcJobDone done,
                               void *data, void (*destroy)(void *));
void nc_connection_cancel(NcConnection *c);
/* Queues DISCONNECT behind the pending jobs; the worker exits after it and
 * NC_CONN_EV_CLOSED follows */
void nc_connection_close(NcConnection *c);
/* Stops the worker (cancelling a running statement) and joins it, fails jobs
 * that never ran (destroy_data runs, done does not), disconnects the database
 * connection unless a DISCONNECT job already did, and drops the owner's
 * reference. Callable from the main thread at any time; completions already
 * posted still run, and the last of them frees the memory. Drain the main
 * queue after it (and before freeing the queue). */
void nc_connection_free(NcConnection *c);
NcConnection *nc_connection_ref(NcConnection *c); /* main thread only */
void nc_connection_unref(NcConnection *c);

int nc_connection_id(const NcConnection *c);
LaceDriver nc_connection_driver(const NcConnection *c);
const char *nc_connection_driver_name(const NcConnection *c);
const char *nc_connection_name(const NcConnection *c);
void nc_connection_set_name(NcConnection *c, const char *name);
const char *nc_connection_connstr(const NcConnection *c);
const char *nc_connection_saved_id(const NcConnection *c);
char *nc_connection_display(const NcConnection *c);        /* never a password (allocated) */
char *nc_connection_short_name(const NcConnection *c);     /* "data.sqlite" / "db on host" */
char *nc_connection_masked_connstr(const NcConnection *c); /* password-less (allocated) */
char **nc_connection_tables(const NcConnection *c, size_t *n); /* owned by c */
bool nc_connection_tables_loaded(const NcConnection *c);
/* The database the connection is in (known with the table list; NULL: none,
 * as SQLite, or not known yet). Owned by c. */
const char *nc_connection_database(const NcConnection *c);
/* The views, routines and triggers (lace_list_objects), known with the table
 * list; owned by c */
LaceObject *nc_connection_objects(const NcConnection *c, size_t *n);
const LaceObject *nc_connection_find_object(const NcConnection *c, LaceObjectKind kind, const char *name);
void nc_connection_refresh_tables(NcConnection *c);
NcTxState nc_connection_tx_state(const NcConnection *c);
bool nc_connection_busy(const NcConnection *c);
bool nc_connection_closed(const NcConnection *c);

LaceHistory *nc_connection_history(NcConnection *c);
void nc_connection_record_history(NcConnection *c, const char *sql, LaceHistoryType t);
void nc_connection_history_remove(NcConnection *c, size_t i);
void nc_connection_history_clear(NcConnection *c);
#endif
