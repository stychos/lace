#include "conn/connection.h"
#include "conn/jobs_internal.h"
#include "config/recent.h"
#include "core/connstr_display.h"
#include "core/history.h"
#include <config/history.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <util/connstr.h>
#include <util/mem.h>
#include <util/str.h>

/* A job carries its connection while queued and until done ran */
typedef struct Queued {
  NcJob *job;
  NcConnection *conn;
  struct Queued *next;
} Queued;

struct NcConnection {
  lace_client_t *client;
  MainQueue *mainq;
  char *connstr;
  char *saved_id;
  char *name;
  LaceDriver driver;
  const char *driver_name;

  /* Shared with the worker (lock) */
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int conn_id;
  uint64_t generation;
  int w_tx_state;       /* the worker's view */
  bool lost_notice;     /* a reconnect happened; tell the user once */
  Queued *queue_head, *queue_tail;
  bool stop;

  /* Main thread */
  int refs; /* the owner's plus one per Queued item not yet completed */
  int tx_state;
  char **tables;
  size_t num_tables;
  bool tables_loaded;
  int pending;
  bool closing, closed;
  pthread_t thread;
  bool started;
  NcConnListener listener;
  void *listener_data;

  LaceHistory *history;
  int history_mode, history_max;
};

static void emit(NcConnection *c, NcConnEvent ev, const char *msg) {
  if (c->listener) c->listener(c, ev, msg, c->listener_data);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ==========================================================================
 * Worker side
 * ========================================================================== */

static void job_fail(NcJob *job, lace_client_t *client, int code) {
  job->code = code;
  free(job->error);
  job->error = str_dup(lace_client_error(client));
  if (code == LACE_ERR_QUERY_CANCELLED) job->cancelled = true;
}

static void job_fail_msg(NcJob *job, int code, const char *msg) {
  job->code = code;
  free(job->error);
  job->error = str_dup(msg);
}

/* Record the transaction state liblace saw after the last statement */
static void note_tx(NcConnection *c, NcJob *job) {
  int st = lace_last_transaction_state();
  if (st < 0) return;
  pthread_mutex_lock(&c->lock);
  if (c->w_tx_state != NC_TX_LOST) c->w_tx_state = st; /* 0 none, 1 open, 2 failed */
  job->tx_state = c->w_tx_state;
  pthread_mutex_unlock(&c->lock);
}

static int64_t first_int(const LaceResult *r) {
  if (!r || r->num_rows == 0 || !r->rows || r->rows[0].num_cells == 0) return -1;
  const LaceValue *v = &r->rows[0].cells[0];
  if (v->type == LACE_TYPE_INT) return v->int_val;
  if (v->type == LACE_TYPE_DECIMAL || v->type == LACE_TYPE_TEXT) {
    int64_t n;
    if (v->text.data && str_to_int64(v->text.data, &n)) return n;
  }
  if (v->type == LACE_TYPE_FLOAT) return (int64_t)v->float_val;
  return -1;
}

/* The one place a job's liblace calls happen. Returns the liblace code. */
static int run_once(NcConnection *c, NcJob *job, int conn_id) {
  lace_client_t *client = c->client;
  int rc = LACE_OK;
  switch (job->kind) {
  case NC_JOB_EXEC:
    rc = lace_exec(client, conn_id, job->sql, &job->result);
    if (rc == LACE_OK && job->result) job->affected = job->result->rows_affected;
    break;
  case NC_JOB_SCHEMA:
    rc = lace_get_schema(client, conn_id, job->table, &job->schema);
    break;
  case NC_JOB_TABLES:
    rc = lace_list_tables(client, conn_id, &job->tables, &job->num_tables);
    break;
  case NC_JOB_COUNT:
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
      if (rc == LACE_OK) job->count = first_int(r);
      lace_result_free(r);
    }
    break;
  case NC_JOB_UPDATE:
  case NC_JOB_DELETE: {
    char *why = NULL;
    if (!lace_row_key_verify(client, conn_id, job->table, &job->key, &job->snapshot, &why)) {
      job_fail_msg(job, LACE_ERR_ROW_NOT_FOUND, why ? why : "row check failed");
      free(why);
      return job->code;
    }
    LacePkValue *pk = lace_row_key_to_pk(&job->key);
    if (job->kind == NC_JOB_UPDATE)
      rc = lace_update(client, conn_id, job->table, pk, job->key.count, job->column, &job->value);
    else
      rc = lace_delete(client, conn_id, job->table, pk, job->key.count);
    free(pk);
    if (lace_last_sql()) job->last_sql = str_dup(lace_last_sql());
    job->affected = rc == LACE_OK ? 1 : 0;
    break;
  }
  case NC_JOB_INSERT:
    rc = lace_insert(client, conn_id, job->table, (const char **)job->columns, job->values,
                     job->num_values);
    if (lace_last_sql()) job->last_sql = str_dup(lace_last_sql());
    job->affected = rc == LACE_OK ? 1 : 0;
    break;
  case NC_JOB_DISCONNECT:
    rc = lace_disconnect(client, conn_id);
    break;
  }
  if (rc != LACE_OK) job_fail(job, client, rc);
  return rc;
}

/* The daemon was replaced: open the database again. Returns true when the
 * job may be retried. */
static bool reconnect(NcConnection *c, NcJob *job) {
  int new_id = -1;
  int rc = lace_connect(c->client, c->connstr, NULL, &new_id);
  if (rc != LACE_OK) {
    char *msg = str_printf("The daemon was replaced and reconnecting failed: %s",
                           lace_client_error(c->client));
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST, msg);
    free(msg);
    return false;
  }
  pthread_mutex_lock(&c->lock);
  c->conn_id = new_id;
  c->generation = lace_client_generation(c->client);
  bool tx_was_open = c->w_tx_state == NC_TX_OPEN || c->w_tx_state == NC_TX_FAILED;
  c->w_tx_state = tx_was_open ? NC_TX_LOST : NC_TX_NONE;
  c->lost_notice = true;
  uint64_t gen = c->generation;
  pthread_mutex_unlock(&c->lock);
  lace_expect_generation(c->client, gen);
  if (tx_was_open) {
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST,
                 "The daemon was replaced while a transaction was open. Its "
                 "work is lost: run ROLLBACK to acknowledge.");
    return false;
  }
  if (!job->read_only) {
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST,
                 "The daemon was replaced; reconnected. Please retry.");
    return false;
  }
  return true;
}

static void run_job(NcConnection *c, NcJob *job) {
  double t0 = now_ms();
  pthread_mutex_lock(&c->lock);
  int conn_id = c->conn_id;
  uint64_t gen = c->generation;
  int tx = c->w_tx_state;
  pthread_mutex_unlock(&c->lock);
  lace_expect_generation(c->client, gen);

  /* A lost transaction: only ROLLBACK (acknowledged, not sent) gets through */
  if (tx == NC_TX_LOST && job->kind != NC_JOB_DISCONNECT) {
    if (job->kind == NC_JOB_EXEC && lace_sql_is_rollback(job->sql)) {
      pthread_mutex_lock(&c->lock);
      c->w_tx_state = NC_TX_NONE;
      pthread_mutex_unlock(&c->lock);
      job->tx_state = NC_TX_NONE;
      job->result = safe_calloc(1, sizeof(LaceResult));
      job->result->rows_affected = 0;
      job->affected = 0;
      goto done;
    }
    job_fail_msg(job, LACE_ERR_CONNECTION_LOST,
                 "The transaction was lost when the daemon was replaced. Run "
                 "ROLLBACK to acknowledge.");
    job->tx_state = NC_TX_LOST;
    goto done;
  }

  /* Reads inside a live PostgreSQL transaction get a savepoint, so a failed
   * or cancelled page does not abort the user's transaction */
  bool sp = job->savepoint && c->driver == LACE_DRIVER_POSTGRES && tx == NC_TX_OPEN;
  if (sp && lace_exec(c->client, conn_id, "SAVEPOINT lace_read", NULL) != LACE_OK) sp = false;

  int rc = run_once(c, job, conn_id);
  if (rc == LACE_ERR_CONNECTION_LOST && lace_client_generation(c->client) != gen) {
    sp = false; /* the old daemon's savepoint is gone with it */
    if (reconnect(c, job)) {
      free(job->error);
      job->error = NULL;
      job->code = LACE_OK;
      pthread_mutex_lock(&c->lock);
      conn_id = c->conn_id;
      pthread_mutex_unlock(&c->lock);
      rc = run_once(c, job, conn_id);
    }
  }
  if (rc != LACE_OK && job->kind != NC_JOB_DISCONNECT) note_tx(c, job);
  if (sp) {
    int src = lace_exec(c->client, conn_id,
                        rc == LACE_OK ? "RELEASE SAVEPOINT lace_read"
                                      : "ROLLBACK TO SAVEPOINT lace_read",
                        NULL);
    if (src == LACE_OK) note_tx(c, job); /* the failed read marked it failed; live again */
  } else if (rc == LACE_OK && job->kind != NC_JOB_DISCONNECT) {
    note_tx(c, job);
  }

done:
  job->elapsed_ms = now_ms() - t0;
}

static void job_done_main(void *data);

static void *worker_main(void *data) {
  NcConnection *c = data;
  for (;;) {
    pthread_mutex_lock(&c->lock);
    while (!c->queue_head && !c->stop) pthread_cond_wait(&c->cond, &c->lock);
    if (c->stop) {
      /* Stopped without a DISCONNECT job (quitting): disconnect here, so the
       * daemon does not keep the connection, and an open transaction's
       * locks, until its orphan reaper. The worker thread declared the
       * connection's generation, so a replaced daemon's reused id is never
       * reached. The result is ignored; liblace's call timeout bounds it. */
      int id = c->conn_id;
      uint64_t gen = c->generation;
      pthread_mutex_unlock(&c->lock);
      lace_expect_generation(c->client, gen);
      if (id >= 0) (void)lace_disconnect(c->client, id);
      break;
    }
    Queued *q = c->queue_head;
    c->queue_head = q->next;
    if (!c->queue_head) c->queue_tail = NULL;
    q->next = NULL;
    pthread_mutex_unlock(&c->lock);
    q->job->was_running = true;
    run_job(c, q->job);
    bool last = q->job->kind == NC_JOB_DISCONNECT;
    mainq_post(c->mainq, job_done_main, q);
    if (last) break;
  }
  return NULL;
}

/* ==========================================================================
 * Main thread side
 * ========================================================================== */

static void history_persist(NcConnection *c) {
  if (c->history_mode != LACE_HISTORY_MODE_PERSISTENT || !c->saved_id) return;
  char *err = NULL;
  if (!history_save(c->history, &err)) fprintf(stderr, "history: %s\n", err ? err : "save failed");
  free(err);
}

static void job_done_main(void *data) {
  Queued *q = data;
  NcJob *job = q->job;
  NcConnection *c = q->conn;

  c->pending--;
  if (job->tx_state >= 0 && job->tx_state != c->tx_state) {
    c->tx_state = job->tx_state;
    emit(c, NC_CONN_EV_TX, NULL);
  }
  pthread_mutex_lock(&c->lock);
  bool notice = c->lost_notice;
  c->lost_notice = false;
  pthread_mutex_unlock(&c->lock);
  if (notice)
    emit(c, NC_CONN_EV_NOTICE,
         c->tx_state == NC_TX_LOST ? "The daemon was replaced. The open transaction is lost."
                                   : "The daemon was replaced; the connection was reopened.");

  if (job->kind == NC_JOB_TABLES) {
    if (!job->error) {
      lace_tables_free(c->tables, c->num_tables);
      c->tables = job->tables; /* the connection takes the array over */
      c->num_tables = job->num_tables;
      job->tables = NULL;
      job->num_tables = 0;
      c->tables_loaded = true;
    }
    emit(c, NC_CONN_EV_TABLES, NULL);
  }
  if (job->record && !job->error && !job->cancelled) {
    const char *sql = job->last_sql ? job->last_sql : job->sql;
    if (sql && *sql) nc_connection_record_history(c, sql, lace_history_detect_type(sql));
  }

  if (job->done && nc_job_owner_alive(job)) job->done(job, job->data);
  if (job->destroy_data) job->destroy_data(job->data);
  emit(c, NC_CONN_EV_BUSY, NULL);

  if (job->kind == NC_JOB_DISCONNECT) {
    c->closed = true;
    emit(c, NC_CONN_EV_CLOSED, NULL);
  }
  nc_job_unref(job);
  free(q);
  nc_connection_unref(c); /* last: may free the connection */
}

void nc_connection_submit(NcConnection *c, NcJob *job, NcOwner *owner, NcJobDone done, void *data) {
  nc_connection_submit_full(c, job, owner, done, data, NULL);
}

void nc_connection_submit_full(NcConnection *c, NcJob *job, NcOwner *owner, NcJobDone done,
                               void *data, void (*destroy)(void *)) {
  job->done = done;
  job->data = data;
  job->destroy_data = destroy;
  nc_job_set_owner(job, owner);
  Queued *q = safe_calloc(1, sizeof *q);
  q->job = job;
  q->conn = nc_connection_ref(c);
  c->pending++;
  if (c->closed || (c->closing && job->kind != NC_JOB_DISCONNECT)) {
    job_fail_msg(job, LACE_ERR_CONNECTION_CLOSED, "The connection is closed");
    mainq_post(c->mainq, job_done_main, q);
    return;
  }
  pthread_mutex_lock(&c->lock);
  if (c->queue_tail) c->queue_tail->next = q;
  else c->queue_head = q;
  c->queue_tail = q;
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->lock);
  emit(c, NC_CONN_EV_BUSY, NULL);
}

void nc_connection_cancel(NcConnection *c) {
  if (c->pending == 0 || c->closed) return;
  pthread_mutex_lock(&c->lock);
  int id = c->conn_id;
  pthread_mutex_unlock(&c->lock);
  lace_cancel_query(c->client, id);
}

void nc_connection_close(NcConnection *c) {
  if (c->closing || c->closed) return;
  nc_connection_submit(c, nc_job_disconnect(), NULL, NULL, NULL);
  c->closing = true;
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
  if (!strncmp(connstr, "postgres", 8)) return LACE_DRIVER_POSTGRES;
  if (!strncmp(connstr, "mariadb", 7)) return LACE_DRIVER_MARIADB;
  if (!strncmp(connstr, "mysql", 5)) return LACE_DRIVER_MYSQL;
  return LACE_DRIVER_SQLITE;
}

NcConnection *nc_connection_new(lace_client_t *client, MainQueue *q, int conn_id,
                                const char *connstr, const char *saved_id, const char *name,
                                int history_mode, int history_max) {
  NcConnection *c = safe_calloc(1, sizeof *c);
  c->client = client;
  c->mainq = q;
  c->conn_id = conn_id;
  c->generation = lace_client_generation(client);
  c->connstr = str_dup(connstr ? connstr : "");
  c->saved_id = saved_id && *saved_id ? str_dup(saved_id) : NULL;
  c->driver = driver_of_connstr(c->connstr);
  c->driver_name = driver_name_of(c->driver);
  c->name = name && *name ? str_dup(name) : nc_connection_short_name(c);
  c->history_mode = history_mode;
  c->history_max = history_max;
  c->refs = 1;
  c->tx_state = c->w_tx_state = NC_TX_NONE;
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->cond, NULL);
  c->history = lace_history_create(c->saved_id);
  if (history_mode == LACE_HISTORY_MODE_PERSISTENT && c->saved_id) {
    char *err = NULL;
    if (!history_load(c->history, &err)) fprintf(stderr, "history: %s\n", err ? err : "load failed");
    free(err);
  }
  c->started = pthread_create(&c->thread, NULL, worker_main, c) == 0;
  return c;
}

void nc_connection_set_listener(NcConnection *c, NcConnListener fn, void *data) {
  c->listener = fn;
  c->listener_data = data;
}

NcConnection *nc_connection_ref(NcConnection *c) {
  c->refs++;
  return c;
}

static void connection_destroy(NcConnection *c) {
  lace_tables_free(c->tables, c->num_tables);
  pthread_mutex_destroy(&c->lock);
  pthread_cond_destroy(&c->cond);
  str_secure_free(c->connstr);
  free(c->saved_id);
  free(c->name);
  lace_history_free(c->history);
  free(c);
}

void nc_connection_unref(NcConnection *c) {
  if (c && --c->refs == 0) connection_destroy(c);
}

void nc_connection_free(NcConnection *c) {
  if (!c) return;
  c->listener = NULL; /* completions still queued must not call into the UI */
  pthread_mutex_lock(&c->lock);
  c->stop = true;
  pthread_cond_signal(&c->cond);
  int id = c->conn_id;
  pthread_mutex_unlock(&c->lock);
  /* Do not make quitting wait for a running statement */
  if (c->pending > 0 && c->started && !c->closed) (void)lace_cancel_query(c->client, id);
  if (c->started) pthread_join(c->thread, NULL);
  /* No worker ever ran (it disconnects when stopped): disconnect here, only
   * while the daemon that opened the id is still the one we talk to */
  else if (!c->closed && id >= 0 && lace_client_generation(c->client) == c->generation)
    (void)lace_disconnect(c->client, id);
  /* Jobs that never ran: failed, never delivered */
  for (Queued *q = c->queue_head; q;) {
    Queued *next = q->next;
    if (q->job->destroy_data) q->job->destroy_data(q->job->data);
    nc_job_unref(q->job);
    free(q);
    c->refs--; /* the owner's reference keeps the count above zero */
    q = next;
  }
  c->queue_head = c->queue_tail = NULL;
  history_persist(c);
  nc_connection_unref(c);
}

/* ==========================================================================
 * Accessors
 * ========================================================================== */

int nc_connection_id(const NcConnection *cc) {
  NcConnection *c = (NcConnection *)cc;
  pthread_mutex_lock(&c->lock);
  int id = c->conn_id;
  pthread_mutex_unlock(&c->lock);
  return id;
}

LaceDriver nc_connection_driver(const NcConnection *c) { return c->driver; }
const char *nc_connection_driver_name(const NcConnection *c) { return c->driver_name; }
const char *nc_connection_name(const NcConnection *c) { return c->name; }

void nc_connection_set_name(NcConnection *c, const char *name) {
  free(c->name);
  c->name = str_dup(name && *name ? name : "Connection");
}

const char *nc_connection_connstr(const NcConnection *c) { return c->connstr; }
const char *nc_connection_saved_id(const NcConnection *c) { return c->saved_id; }

char *nc_connection_short_name(const NcConnection *c) {
  ConnString *cs = connstr_parse(c->connstr, NULL);
  if (!cs) return str_dup("database");
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

char *nc_connection_display(const NcConnection *c) {
  char *out = connstr_display(c->connstr);
  return out ? out : str_dup("database");
}

char *nc_connection_masked_connstr(const NcConnection *c) {
  char *out = recent_strip_passwords(c->connstr);
  return out ? out : connstr_mask_password(c->connstr);
}

char **nc_connection_tables(const NcConnection *c, size_t *n) {
  if (n) *n = c->num_tables;
  return c->tables;
}

bool nc_connection_tables_loaded(const NcConnection *c) { return c->tables_loaded; }

void nc_connection_refresh_tables(NcConnection *c) {
  nc_connection_submit(c, nc_job_tables(), NULL, NULL, NULL);
}

NcTxState nc_connection_tx_state(const NcConnection *c) { return (NcTxState)c->tx_state; }
bool nc_connection_busy(const NcConnection *c) { return c->pending > 0; }
bool nc_connection_closed(const NcConnection *c) { return c->closed || c->closing; }

LaceHistory *nc_connection_history(NcConnection *c) { return c->history; }

void nc_connection_record_history(NcConnection *c, const char *sql, LaceHistoryType t) {
  if (c->history_mode == LACE_HISTORY_MODE_OFF || !sql || !*sql) return;
  lace_history_add(c->history, sql, t, c->history_max);
  history_persist(c);
}

void nc_connection_history_remove(NcConnection *c, size_t i) {
  lace_history_remove(c->history, i);
  history_persist(c);
}

void nc_connection_history_clear(NcConnection *c) {
  lace_history_clear(c->history);
  history_persist(c);
}
