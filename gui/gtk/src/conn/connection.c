/*
 * Lace GTK frontend
 * LaceConnection: worker thread, job execution, reconnects, history
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connection.h"
#include "core/history.h"
#include <lace.h>
#include <config/saved_connection.h>
#include <gtk/gtk.h>
#include <util/connstr.h>
#include <util/mem.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

struct _LaceConnection {
  GObject parent_instance;

  lace_client_t *client;
  char *connstr;
  char *saved_id;
  char *name;
  LaceDriver driver;
  const char *driver_name;

  /* Shared with the worker (lock) */
  GMutex lock;
  int conn_id;
  uint64_t generation;
  int w_tx_state;      /* The worker's view */
  gboolean lost_notice; /* A reconnect happened; tell the user once */

  /* Main thread */
  int tx_state;
  GtkStringList *tables;
  gboolean tables_loaded;
  int pending;
  LaceJob *running;
  gboolean closing;
  gboolean closed;

  GThread *thread;
  GAsyncQueue *queue;

  LaceHistory *history;
  int history_mode;
  int history_max;
};

enum { SIG_TABLES_CHANGED, SIG_TX_CHANGED, SIG_BUSY_CHANGED, SIG_NOTICE,
       SIG_HISTORY_CHANGED, SIG_CLOSED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(LaceConnection, lace_connection, G_TYPE_OBJECT)

/* Queue sentinel: stop the worker */
static LaceJob quit_job;

/* A job carries its connection while queued and until done ran */
typedef struct {
  LaceJob *job;
  LaceConnection *conn;
} Queued;

/* ==========================================================================
 * Worker side
 * ========================================================================== */

static void job_fail(LaceJob *job, lace_client_t *client, int code) {
  job->code = code;
  free(job->error);
  job->error = str_dup(lace_client_error(client));
  if (code == LACE_ERR_QUERY_CANCELLED)
    job->cancelled = TRUE;
}

static void job_fail_msg(LaceJob *job, int code, const char *msg) {
  job->code = code;
  free(job->error);
  job->error = str_dup(msg);
}

/* Record the transaction state liblace saw after the last statement */
static void note_tx(LaceConnection *c, LaceJob *job) {
  int st = lace_last_transaction_state();
  if (st < 0)
    return;
  g_mutex_lock(&c->lock);
  if (c->w_tx_state != LACE_TX_LOST)
    c->w_tx_state = st; /* 0 none, 1 open, 2 failed */
  job->tx_state = c->w_tx_state;
  g_mutex_unlock(&c->lock);
}

static int64_t first_int(const LaceResult *r) {
  if (!r || r->num_rows == 0 || !r->rows || r->rows[0].num_cells == 0)
    return -1;
  const LaceValue *v = &r->rows[0].cells[0];
  if (v->type == LACE_TYPE_INT)
    return v->int_val;
  if (v->type == LACE_TYPE_DECIMAL || v->type == LACE_TYPE_TEXT) {
    int64_t n;
    if (v->text.data && str_to_int64(v->text.data, &n))
      return n;
  }
  if (v->type == LACE_TYPE_FLOAT)
    return (int64_t)v->float_val;
  return -1;
}

/* The one place a job's liblace calls happen. Returns the liblace code. */
static int run_once(LaceConnection *c, LaceJob *job, int conn_id) {
  lace_client_t *client = c->client;
  int rc = LACE_OK;
  switch (job->kind) {
  case LACE_JOB_EXEC:
    rc = lace_exec(client, conn_id, job->sql, &job->result);
    if (rc == LACE_OK && job->result)
      job->affected = job->result->rows_affected;
    break;
  case LACE_JOB_SCHEMA:
    rc = lace_get_schema(client, conn_id, job->table, &job->schema);
    break;
  case LACE_JOB_TABLES:
    rc = lace_list_tables(client, conn_id, &job->tables, &job->num_tables);
    break;
  case LACE_JOB_COUNT: {
    if (job->table && !job->where && job->approximate) {
      size_t n = 0;
      bool approx = true;
      rc = lace_count(client, conn_id, job->table, &n, &approx);
      job->count = (int64_t)n;
      job->count_approximate = approx;
    } else {
      char *sql = job->sql ? str_dup(job->sql)
                           : lace_sql_table_count(c->driver, job->table, job->where);
      LaceResult *r = NULL;
      rc = lace_exec(client, conn_id, sql, &r);
      free(sql);
      if (rc == LACE_OK)
        job->count = first_int(r);
      lace_result_free(r);
    }
    break;
  }
  case LACE_JOB_UPDATE:
  case LACE_JOB_DELETE: {
    char *why = NULL;
    if (!lace_row_key_verify(client, conn_id, job->table, &job->key, &job->snapshot,
                        &why)) {
      job_fail_msg(job, LACE_ERR_ROW_NOT_FOUND, why ? why : "row check failed");
      free(why);
      return job->code;
    }
    LacePkValue *pk = lace_row_key_to_pk(&job->key);
    if (job->kind == LACE_JOB_UPDATE)
      rc = lace_update(client, conn_id, job->table, pk, job->key.count,
                       job->column, &job->value);
    else
      rc = lace_delete(client, conn_id, job->table, pk, job->key.count);
    free(pk);
    if (lace_last_sql())
      job->last_sql = str_dup(lace_last_sql());
    job->affected = rc == LACE_OK ? 1 : 0;
    break;
  }
  case LACE_JOB_INSERT:
    rc = lace_insert(client, conn_id, job->table,
                     (const char **)job->columns, job->values, job->num_values);
    if (lace_last_sql())
      job->last_sql = str_dup(lace_last_sql());
    job->affected = rc == LACE_OK ? 1 : 0;
    break;
  case LACE_JOB_DISCONNECT:
    rc = lace_disconnect(client, conn_id);
    break;
  }
  if (rc != LACE_OK)
    job_fail(job, client, rc);
  return rc;
}

/* The daemon was replaced: open the database again. Returns true when the
 * job may be retried. */
static gboolean reconnect(LaceConnection *c, LaceJob *job) {
  int new_id = -1;
  int rc = lace_connect(c->client, c->connstr, NULL, &new_id);
  if (rc != LACE_OK) {
    char *msg = str_printf("The daemon was replaced and reconnecting failed: %s",
                           lace_client_error(c->client));
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST, msg);
    free(msg);
    return FALSE;
  }
  g_mutex_lock(&c->lock);
  c->conn_id = new_id;
  c->generation = lace_client_generation(c->client);
  gboolean tx_was_open = c->w_tx_state == LACE_TX_OPEN ||
                         c->w_tx_state == LACE_TX_FAILED;
  c->w_tx_state = tx_was_open ? LACE_TX_LOST : LACE_TX_NONE;
  c->lost_notice = TRUE;
  g_mutex_unlock(&c->lock);
  lace_expect_generation(c->client, c->generation);
  if (tx_was_open) {
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST,
                 "The daemon was replaced while a transaction was open. Its "
                 "work is lost: run ROLLBACK to acknowledge.");
    return FALSE;
  }
  if (!job->read_only) {
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST,
                 "The daemon was replaced; reconnected. Please retry.");
    return FALSE;
  }
  return TRUE;
}

static void run_job(LaceConnection *c, LaceJob *job) {
  gint64 t0 = g_get_monotonic_time();
  g_mutex_lock(&c->lock);
  int conn_id = c->conn_id;
  uint64_t gen = c->generation;
  int tx = c->w_tx_state;
  g_mutex_unlock(&c->lock);
  lace_expect_generation(c->client, gen);

  /* A lost transaction: only ROLLBACK (acknowledged, not sent) gets through */
  if (tx == LACE_TX_LOST && job->kind != LACE_JOB_DISCONNECT) {
    if (job->kind == LACE_JOB_EXEC && lace_sql_is_rollback(job->sql)) {
      g_mutex_lock(&c->lock);
      c->w_tx_state = LACE_TX_NONE;
      g_mutex_unlock(&c->lock);
      job->tx_state = LACE_TX_NONE;
      job->result = safe_calloc(1, sizeof(LaceResult));
      job->result->rows_affected = 0;
      job->affected = 0;
      goto done;
    }
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST,
                 "The transaction was lost when the daemon was replaced. Run "
                 "ROLLBACK to acknowledge.");
    job->tx_state = LACE_TX_LOST;
    goto done;
  }

  /* Reads inside a live PostgreSQL transaction get a savepoint, so a
   * failed or cancelled page does not abort the user's transaction */
  gboolean sp = job->savepoint && c->driver == LACE_DRIVER_POSTGRES &&
                tx == LACE_TX_OPEN;
  if (sp && lace_exec(c->client, conn_id, "SAVEPOINT lace_read", NULL) != LACE_OK)
    sp = FALSE;

  int rc = run_once(c, job, conn_id);
  if (rc == LACE_ERR_CONNECTION_LOST &&
      lace_client_generation(c->client) != gen) {
    sp = FALSE; /* The old daemon's savepoint is gone with it */
    if (reconnect(c, job)) {
      free(job->error);
      job->error = NULL;
      job->code = LACE_OK;
      g_mutex_lock(&c->lock);
      conn_id = c->conn_id;
      g_mutex_unlock(&c->lock);
      rc = run_once(c, job, conn_id);
    }
  }
  if (rc != LACE_OK && job->kind != LACE_JOB_DISCONNECT)
    note_tx(c, job);
  if (sp) {
    int src = lace_exec(c->client, conn_id,
                        rc == LACE_OK ? "RELEASE SAVEPOINT lace_read"
                                      : "ROLLBACK TO SAVEPOINT lace_read",
                        NULL);
    if (src == LACE_OK)
      note_tx(c, job); /* The failed read marked it failed; live again */
  } else if (rc == LACE_OK && job->kind != LACE_JOB_DISCONNECT) {
    note_tx(c, job);
  }

done:
  job->elapsed_ms = (double)(g_get_monotonic_time() - t0) / 1000.0;
}

static gboolean job_done_idle(gpointer data);

static gpointer worker_main(gpointer data) {
  LaceConnection *c = data;
  for (;;) {
    Queued *q = g_async_queue_pop(c->queue);
    if (q->job == &quit_job) {
      g_free(q);
      break;
    }
    q->job->was_running = TRUE;
    run_job(c, q->job);
    gboolean stop = q->job->kind == LACE_JOB_DISCONNECT;
    g_main_context_invoke(NULL, job_done_idle, q);
    if (stop)
      break;
  }
  return NULL;
}

/* ==========================================================================
 * Main thread side
 * ========================================================================== */

static void history_persist(LaceConnection *c) {
  if (c->history_mode != LACE_HISTORY_MODE_PERSISTENT || !c->saved_id)
    return;
  char *err = NULL;
  if (!history_save(c->history, &err))
    g_warning("history: %s", err ? err : "save failed");
  free(err);
}

static gboolean job_done_idle(gpointer data) {
  Queued *q = data;
  LaceJob *job = q->job;
  LaceConnection *c = q->conn;

  c->pending--;
  if (c->running == job)
    c->running = NULL;

  if (job->tx_state >= 0 && job->tx_state != c->tx_state) {
    c->tx_state = job->tx_state;
    g_signal_emit(c, signals[SIG_TX_CHANGED], 0);
  }
  g_mutex_lock(&c->lock);
  gboolean notice = c->lost_notice;
  c->lost_notice = FALSE;
  g_mutex_unlock(&c->lock);
  if (notice) {
    g_signal_emit(c, signals[SIG_NOTICE], 0,
                  c->tx_state == LACE_TX_LOST
                      ? "The daemon was replaced. The open transaction is lost."
                      : "The daemon was replaced; the connection was reopened.");
  }

  if (job->kind == LACE_JOB_TABLES) {
    if (!job->error) {
      /* liblace's array is sized to its count; GTK wants a NULL terminator */
      const char **names = g_new0(const char *, job->num_tables + 1);
      for (size_t i = 0; i < job->num_tables; i++)
        names[i] = job->tables[i];
      gtk_string_list_splice(c->tables, 0, g_list_model_get_n_items(G_LIST_MODEL(c->tables)),
                             names);
      g_free(names);
      c->tables_loaded = TRUE;
    }
    g_signal_emit(c, signals[SIG_TABLES_CHANGED], 0);
  }
  if (job->record && !job->error && !job->cancelled) {
    const char *sql = job->last_sql ? job->last_sql : job->sql;
    if (sql && *sql)
      lace_connection_record_history(c, sql, lace_history_detect_type(sql));
  }

  if (job->done) {
    gboolean deliver = TRUE;
    GObject *owner = NULL;
    if (job->has_owner) {
      owner = g_weak_ref_get(&job->owner);
      deliver = owner != NULL;
    }
    if (deliver)
      job->done(job, job->user_data);
    g_clear_object(&owner);
  }
  if (job->destroy_data)
    job->destroy_data(job->user_data);
  g_signal_emit(c, signals[SIG_BUSY_CHANGED], 0);

  if (job->kind == LACE_JOB_DISCONNECT) {
    c->closed = TRUE;
    g_signal_emit(c, signals[SIG_CLOSED], 0);
  }
  lace_job_unref(job);
  g_object_unref(c);
  g_free(q);
  return G_SOURCE_REMOVE;
}

void lace_connection_submit(LaceConnection *c, LaceJob *job, GObject *owner,
                            LaceJobDone done, gpointer user_data) {
  lace_connection_submit_full(c, job, owner, done, user_data, NULL);
}

void lace_connection_submit_full(LaceConnection *c, LaceJob *job, GObject *owner,
                                 LaceJobDone done, gpointer user_data,
                                 GDestroyNotify destroy) {
  g_return_if_fail(LACE_IS_CONNECTION(c));
  g_return_if_fail(job != NULL);
  job->done = done;
  job->user_data = user_data;
  job->destroy_data = destroy;
  if (owner) {
    g_weak_ref_init(&job->owner, owner);
    job->has_owner = TRUE;
  }
  if (c->closed || (c->closing && job->kind != LACE_JOB_DISCONNECT)) {
    job_fail_msg(job, LACE_ERR_CONNECTION_CLOSED, "The connection is closed");
    Queued *q = g_new0(Queued, 1);
    q->job = job;
    q->conn = g_object_ref(c);
    c->pending++;
    g_idle_add(job_done_idle, q);
    return;
  }
  Queued *q = g_new0(Queued, 1);
  q->job = job;
  q->conn = g_object_ref(c);
  c->pending++;
  if (!c->running)
    c->running = job;
  g_async_queue_push(c->queue, q);
  g_signal_emit(c, signals[SIG_BUSY_CHANGED], 0);
}

void lace_connection_cancel(LaceConnection *c) {
  g_return_if_fail(LACE_IS_CONNECTION(c));
  if (c->pending == 0 || c->closed)
    return;
  g_mutex_lock(&c->lock);
  int id = c->conn_id;
  g_mutex_unlock(&c->lock);
  lace_cancel_query(c->client, id);
}

void lace_connection_close(LaceConnection *c) {
  g_return_if_fail(LACE_IS_CONNECTION(c));
  if (c->closing || c->closed)
    return;
  lace_connection_submit(c, lace_job_disconnect(), NULL, NULL, NULL);
  c->closing = TRUE;
}

/* ==========================================================================
 * Object
 * ========================================================================== */

static const char *driver_name_of(LaceDriver d) {
  switch (d) {
  case LACE_DRIVER_SQLITE: return "sqlite";
  case LACE_DRIVER_POSTGRES: return "postgres";
  case LACE_DRIVER_MYSQL: return "mysql";
  case LACE_DRIVER_MARIADB: return "mariadb";
  }
  return "sqlite";
}

static LaceDriver driver_of_connstr(const char *connstr) {
  const char *n = lace_driver_normalize(connstr);
  (void)n;
  if (g_str_has_prefix(connstr, "postgres"))
    return LACE_DRIVER_POSTGRES;
  if (g_str_has_prefix(connstr, "mariadb"))
    return LACE_DRIVER_MARIADB;
  if (g_str_has_prefix(connstr, "mysql"))
    return LACE_DRIVER_MYSQL;
  return LACE_DRIVER_SQLITE;
}

LaceConnection *lace_connection_new(lace_client_t *client, int conn_id,
                                    const char *connstr, const char *saved_id,
                                    const char *name, int history_mode,
                                    int history_max) {
  LaceConnection *c = g_object_new(LACE_TYPE_CONNECTION, NULL);
  c->client = client;
  c->conn_id = conn_id;
  c->generation = lace_client_generation(client);
  c->connstr = str_dup(connstr ? connstr : "");
  c->saved_id = saved_id && *saved_id ? str_dup(saved_id) : NULL;
  c->driver = driver_of_connstr(c->connstr);
  c->driver_name = driver_name_of(c->driver);
  c->name = name && *name ? str_dup(name) : lace_connection_short_name(c);
  c->history_mode = history_mode;
  c->history_max = history_max;
  c->history = lace_history_create(c->saved_id);
  if (history_mode == LACE_HISTORY_MODE_PERSISTENT && c->saved_id) {
    char *err = NULL;
    if (!history_load(c->history, &err))
      g_warning("history: %s", err ? err : "load failed");
    free(err);
  }
  c->thread = g_thread_new("lace-conn", worker_main, c);
  return c;
}

static void lace_connection_dispose(GObject *obj) {
  LaceConnection *c = LACE_CONNECTION(obj);
  if (c->thread) {
    Queued *q = g_new0(Queued, 1);
    q->job = &quit_job;
    g_async_queue_push(c->queue, q);
    g_thread_join(c->thread);
    c->thread = NULL;
  }
  g_clear_object(&c->tables);
  G_OBJECT_CLASS(lace_connection_parent_class)->dispose(obj);
}

static void lace_connection_finalize(GObject *obj) {
  LaceConnection *c = LACE_CONNECTION(obj);
  g_async_queue_unref(c->queue);
  g_mutex_clear(&c->lock);
  str_secure_free(c->connstr);
  free(c->saved_id);
  free(c->name);
  lace_history_free(c->history);
  G_OBJECT_CLASS(lace_connection_parent_class)->finalize(obj);
}

static void lace_connection_class_init(LaceConnectionClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = lace_connection_dispose;
  oc->finalize = lace_connection_finalize;
  signals[SIG_TABLES_CHANGED] = g_signal_new("tables-changed", LACE_TYPE_CONNECTION,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_TX_CHANGED] = g_signal_new("tx-changed", LACE_TYPE_CONNECTION,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_BUSY_CHANGED] = g_signal_new("busy-changed", LACE_TYPE_CONNECTION,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_NOTICE] = g_signal_new("notice", LACE_TYPE_CONNECTION,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_HISTORY_CHANGED] = g_signal_new("history-changed", LACE_TYPE_CONNECTION,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_CLOSED] = g_signal_new("closed", LACE_TYPE_CONNECTION,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_connection_init(LaceConnection *c) {
  g_mutex_init(&c->lock);
  c->queue = g_async_queue_new();
  c->tables = gtk_string_list_new(NULL);
  c->tx_state = LACE_TX_NONE;
  c->w_tx_state = LACE_TX_NONE;
}

/* ==========================================================================
 * Accessors
 * ========================================================================== */

lace_client_t *lace_connection_get_client(LaceConnection *c) { return c->client; }

int lace_connection_get_id(LaceConnection *c) {
  g_mutex_lock(&c->lock);
  int id = c->conn_id;
  g_mutex_unlock(&c->lock);
  return id;
}

LaceDriver lace_connection_get_driver(LaceConnection *c) { return c->driver; }
const char *lace_connection_get_driver_name(LaceConnection *c) { return c->driver_name; }

const char *lace_connection_get_icon_name(LaceConnection *c) {
  switch (c->driver) {
  case LACE_DRIVER_POSTGRES: return "lace-postgres-symbolic";
  case LACE_DRIVER_MYSQL:
  case LACE_DRIVER_MARIADB: return "lace-mysql-symbolic";
  default: return "lace-sqlite-symbolic";
  }
}

const char *lace_connection_get_name(LaceConnection *c) { return c->name; }

void lace_connection_set_name(LaceConnection *c, const char *name) {
  free(c->name);
  c->name = str_dup(name && *name ? name : "Connection");
}

const char *lace_connection_get_connstr(LaceConnection *c) { return c->connstr; }
const char *lace_connection_get_saved_id(LaceConnection *c) { return c->saved_id; }

char *lace_connection_short_name(LaceConnection *c) {
  ConnString *cs = connstr_parse(c->connstr, NULL);
  if (!cs)
    return str_dup("database");
  char *out;
  if (str_eq(cs->driver, "sqlite")) {
    const char *db = cs->database ? cs->database : "";
    const char *slash = strrchr(db, '/');
    out = str_dup(slash && slash[1] ? slash + 1 : db);
  } else {
    out = str_printf("%s on %s", cs->database ? cs->database : "db",
                     cs->host && *cs->host ? cs->host : "localhost");
  }
  connstr_free(cs);
  return out;
}

char *lace_connection_get_display(LaceConnection *c) {
  ConnString *cs = connstr_parse(c->connstr, NULL);
  if (!cs)
    return str_dup("database");
  char *out;
  if (str_eq(cs->driver, "sqlite")) {
    out = str_printf("sqlite: %s", cs->database ? cs->database : "");
  } else {
    StringBuilder *sb = sb_new(128);
    sb_printf(sb, "%s://", cs->driver ? cs->driver : "db");
    if (cs->user && *cs->user)
      sb_printf(sb, "%s@", cs->user);
    sb_append(sb, cs->host && *cs->host ? cs->host : "localhost");
    if (cs->port > 0)
      sb_printf(sb, ":%d", cs->port);
    sb_printf(sb, "/%s", cs->database ? cs->database : "");
    if (cs->ssh && cs->ssh_host) {
      sb_append(sb, " via ssh ");
      if (cs->ssh_user && *cs->ssh_user)
        sb_printf(sb, "%s@", cs->ssh_user);
      sb_append(sb, cs->ssh_host);
    }
    out = sb_finish(sb);
  }
  connstr_free(cs);
  return out;
}

char *lace_connection_get_masked_connstr(LaceConnection *c) {
  LaceSavedConnection *sc = lace_saved_conn_from_connstr(c->connstr, NULL);
  if (!sc)
    return connstr_mask_password(c->connstr);
  sc->save_password = false;
  sc->save_ssh_password = false;
  char *out = lace_saved_conn_build_connstr(sc, NULL, NULL);
  lace_saved_conn_free(sc);
  return out;
}

GListModel *lace_connection_get_tables(LaceConnection *c) {
  return G_LIST_MODEL(c->tables);
}

gboolean lace_connection_tables_loaded(LaceConnection *c) { return c->tables_loaded; }

void lace_connection_refresh_tables(LaceConnection *c) {
  lace_connection_submit(c, lace_job_tables(), NULL, NULL, NULL);
}

LaceTxState lace_connection_get_tx_state(LaceConnection *c) { return c->tx_state; }
gboolean lace_connection_is_busy(LaceConnection *c) { return c->pending > 0; }
gboolean lace_connection_is_closed(LaceConnection *c) { return c->closed || c->closing; }

LaceHistory *lace_connection_get_history(LaceConnection *c) { return c->history; }

void lace_connection_record_history(LaceConnection *c, const char *sql,
                                    LaceHistoryType type) {
  if (c->history_mode == LACE_HISTORY_MODE_OFF || !sql || !*sql)
    return;
  lace_history_add(c->history, sql, type, c->history_max);
  history_persist(c);
  g_signal_emit(c, signals[SIG_HISTORY_CHANGED], 0);
}

void lace_connection_history_remove(LaceConnection *c, size_t index) {
  lace_history_remove(c->history, index);
  history_persist(c);
  g_signal_emit(c, signals[SIG_HISTORY_CHANGED], 0);
}

void lace_connection_history_clear(LaceConnection *c) {
  lace_history_clear(c->history);
  history_persist(c);
  g_signal_emit(c, signals[SIG_HISTORY_CHANGED], 0);
}
