/*
 * Lace GTK frontend
 * LaceConnection: one open database connection and its worker thread
 *
 * The daemon runs one statement at a time per database connection, so every
 * liblace call for a connection goes through its own worker thread, in
 * order, as a LaceJob. Results come back on the main loop. The object also
 * holds what the UI shows about the connection: its name, driver, tables,
 * transaction state and query history.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_CONNECTION_H
#define LACE_GTK_CONNECTION_H

#include "jobs.h"
#include <config/history.h>
#include <gio/gio.h>
#include <lace.h>

G_BEGIN_DECLS

#define LACE_TYPE_CONNECTION (lace_connection_get_type())
G_DECLARE_FINAL_TYPE(LaceConnection, lace_connection, LACE, CONNECTION, GObject)

/* Transaction state as shown in the UI */
typedef enum {
  LACE_TX_NONE = 0,
  LACE_TX_OPEN = 1,
  LACE_TX_FAILED = 2, /* PostgreSQL: in error, only ROLLBACK works */
  LACE_TX_LOST = 3,   /* The daemon was replaced while one was open */
} LaceTxState;

/* Takes the open conn_id. connstr may hold passwords (copied, wiped when
 * freed). saved_id names the saved connection (history file), may be NULL.
 * history_mode/max as in LACE_HISTORY_MODE_*. */
LaceConnection *lace_connection_new(lace_client_t *client, int conn_id,
                                    const char *connstr, const char *saved_id,
                                    const char *name, int history_mode,
                                    int history_max);

lace_client_t *lace_connection_get_client(LaceConnection *c);
int lace_connection_get_id(LaceConnection *c);
LaceDriver lace_connection_get_driver(LaceConnection *c);
const char *lace_connection_get_driver_name(LaceConnection *c); /* "sqlite" */
const char *lace_connection_get_icon_name(LaceConnection *c);
const char *lace_connection_get_name(LaceConnection *c);
void lace_connection_set_name(LaceConnection *c, const char *name);
const char *lace_connection_get_connstr(LaceConnection *c);
const char *lace_connection_get_saved_id(LaceConnection *c);
/* "sqlite: /path/db" or "postgres://user@host:5432/db via ssh user@host":
 * never a password, never the tunnel socket (allocated) */
char *lace_connection_get_display(LaceConnection *c);
/* "data.sqlite" or "db on host" (allocated) */
char *lace_connection_short_name(LaceConnection *c);
/* A password-less connection string for the session file (allocated) */
char *lace_connection_get_masked_connstr(LaceConnection *c);

/* The tables (a GtkStringList of table references); refreshed on demand */
GListModel *lace_connection_get_tables(LaceConnection *c);
void lace_connection_refresh_tables(LaceConnection *c);
gboolean lace_connection_tables_loaded(LaceConnection *c);

LaceTxState lace_connection_get_tx_state(LaceConnection *c);
gboolean lace_connection_is_busy(LaceConnection *c);
gboolean lace_connection_is_closed(LaceConnection *c);

/* Queue a job. owner (may be NULL): the done callback is skipped once it is
 * gone. The job is unreffed after done; keep a ref to use it later. */
void lace_connection_submit(LaceConnection *c, LaceJob *job, GObject *owner,
                            LaceJobDone done, gpointer user_data);
/* Same, with user_data freed by destroy once the job is over, whether or
 * not done ran (the owner may be gone by then) */
void lace_connection_submit_full(LaceConnection *c, LaceJob *job, GObject *owner,
                                 LaceJobDone done, gpointer user_data,
                                 GDestroyNotify destroy);

/* Cancel the statement running now (best effort, main thread) */
void lace_connection_cancel(LaceConnection *c);

/* Disconnect: pending jobs still run, then the database connection is
 * closed and the worker stops. Emits "closed" when done. */
void lace_connection_close(LaceConnection *c);

/* History (shared file with the TUI when the connection is a saved one and
 * the mode is persistent) */
LaceHistory *lace_connection_get_history(LaceConnection *c);
void lace_connection_record_history(LaceConnection *c, const char *sql,
                                    LaceHistoryType type);
void lace_connection_history_remove(LaceConnection *c, size_t index);
void lace_connection_history_clear(LaceConnection *c);

/* Signals:
 *   tables-changed ()        the table list was (re)loaded or failed
 *   tx-changed ()            transaction state changed
 *   busy-changed ()          jobs started or finished
 *   notice (gchar *message)  something the user should see (reconnects)
 *   history-changed ()
 *   closed ()
 */

G_END_DECLS

#endif /* LACE_GTK_CONNECTION_H */
