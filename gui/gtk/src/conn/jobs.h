/*
 * Lace GTK frontend
 * Jobs: units of liblace work run on a connection's worker thread
 *
 * A job owns its inputs and outputs. It is built on the main thread, run on
 * the connection's worker, and its done callback runs back on the main loop
 * with the results filled in. Read results only in the done callback.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_JOBS_H
#define LACE_GTK_JOBS_H

#include <lace.h>
#include <glib-object.h>
#include <lace.h>

G_BEGIN_DECLS

typedef struct _LaceConnection LaceConnection;

typedef enum {
  LACE_JOB_EXEC,       /* sql -> result */
  LACE_JOB_SCHEMA,     /* table -> schema */
  LACE_JOB_TABLES,     /* -> tables (string array) */
  LACE_JOB_COUNT,      /* sql (or table + where) -> count */
  LACE_JOB_UPDATE,     /* table, key, snapshot, column, value */
  LACE_JOB_DELETE,     /* table, key, snapshot */
  LACE_JOB_INSERT,     /* table, columns, values */
  LACE_JOB_DISCONNECT, /* close the database connection */
  LACE_JOB_TABLE_INFO, /* table -> info, autoinc (lace_table_info, lace_autoincrement_get) */
  LACE_JOB_DEPENDENTS, /* table -> deps (lace_table_dependents) */
  LACE_JOB_DEFINITION, /* an object (object_kind, sql = name, table, column = detail) -> definition */
  LACE_JOB_NAMES,      /* a picker's names (column = "types"/"collations"/"engines", table = a charset) -> tables */
} LaceJobKind;

typedef struct _LaceJob LaceJob;
typedef void (*LaceJobDone)(LaceJob *job, gpointer user_data);

struct _LaceJob {
  /* Private to the connection */
  gatomicrefcount ref;
  LaceJobKind kind;
  LaceJobDone done;
  gpointer user_data;
  GDestroyNotify destroy_data; /* Frees user_data after done, delivered or not */
  GWeakRef owner;
  gboolean has_owner;
  gpointer tag; /* Caller's marker, e.g. a page number */
  gboolean read_only;   /* May be retried after a reconnect */
  gboolean savepoint;   /* Wrap in SAVEPOINT inside a PostgreSQL transaction */
  gboolean record;      /* Record the statement in history on success */
  gboolean user_statement; /* A statement the user asked for (a query view's, an edit, a structure
                            * change, a count asked for): the lock banner shows while it runs */
  gboolean was_running; /* Set when the worker picked it up */

  /* Inputs */
  char *sql;
  char *table;
  char *where;
  gboolean approximate; /* COUNT: a catalog estimate is acceptable */
  LaceRowKey key;
  LaceRowSnapshot snapshot;
  char *column;
  LaceValue value;
  char **columns;
  LaceValue *values;
  size_t num_values;

  /* Outputs */
  LaceResult *result;
  LaceSchema *schema;
  char **tables;
  size_t num_tables;
  LaceObject *objects; /* TABLES: the views, routines and triggers with the tables */
  size_t num_objects;
  LaceTableInfo *info; /* TABLE_INFO */
  LaceAutoInc *autoinc;
  LaceDependent *deps; /* DEPENDENTS */
  size_t num_deps;
  char *definition;    /* DEFINITION */
  int object_kind;
  int64_t count;
  gboolean count_approximate;
  int64_t affected;
  char *error; /* NULL on success */
  int code;    /* LACE_OK or the error code */
  gboolean cancelled;
  double elapsed_ms;
  int tx_state; /* Transaction state after the job (LACE_TX_*) */
  char *last_sql; /* The SQL liblace sent (for history), if any */
};

LaceJob *lace_job_exec(const char *sql);
/* A read the daemon runs for us (page, count, schema): retried after a
 * reconnect and protected by a savepoint inside a PostgreSQL transaction */
LaceJob *lace_job_read(const char *sql);
LaceJob *lace_job_schema(const char *table);
LaceJob *lace_job_tables(void);
/* COUNT(*) of a table with an optional WHERE; with approximate and no
 * WHERE, large server tables may get a catalog estimate */
LaceJob *lace_job_table_count(const char *table, const char *where,
                              gboolean approximate);
LaceJob *lace_job_count_sql(const char *sql); /* SELECT COUNT(*) ... */
LaceJob *lace_job_update(const char *table, const LaceRowKey *key,
                         const LaceRowSnapshot *snapshot, const char *column,
                         const LaceValue *value);
LaceJob *lace_job_delete(const char *table, const LaceRowKey *key,
                         const LaceRowSnapshot *snapshot);
LaceJob *lace_job_insert(const char *table, const char *const *columns,
                         const LaceValue *values, size_t n);
LaceJob *lace_job_disconnect(void);
/* The sub-views' reads: a table's information and counter, the tables
 * referencing one, the CREATE text of a view, routine or trigger */
LaceJob *lace_job_table_info(const char *table);
LaceJob *lace_job_dependents(const char *table);
LaceJob *lace_job_definition(LaceObjectKind kind, const char *name, const char *table, const char *detail);
/* The names a picker lists (a read): what is "types", "collations" (arg: a
 * charset, or NULL) or "engines"; they land in tables/num_tables */
LaceJob *lace_job_names(const char *what, const char *arg);

LaceJob *lace_job_ref(LaceJob *job);
void lace_job_unref(LaceJob *job);

/* Take the result out of a finished job (the job no longer frees it) */
LaceResult *lace_job_steal_result(LaceJob *job);
LaceSchema *lace_job_steal_schema(LaceJob *job);

/* Error text for display ("Query failed: ...") */
const char *lace_job_error(const LaceJob *job);

G_END_DECLS

#endif /* LACE_GTK_JOBS_H */
