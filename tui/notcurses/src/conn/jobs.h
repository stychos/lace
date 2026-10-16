#ifndef LACE_NC_JOBS_H
#define LACE_NC_JOBS_H
#include <lace.h>
#include <stdbool.h>
#include <stdint.h>

/* A job owns its inputs and outputs. It is built on the main thread, run on
 * the connection's worker, and its done callback runs back on the main thread
 * with the results filled in. Read results only in the done callback. */
typedef enum {
  NC_JOB_EXEC, NC_JOB_SCHEMA, NC_JOB_TABLES, NC_JOB_COUNT,
  NC_JOB_UPDATE, NC_JOB_DELETE, NC_JOB_INSERT, NC_JOB_DISCONNECT,
} NcJobKind;

/* Main-thread token: the done callback is skipped once it is released */
typedef struct NcOwner NcOwner;
NcOwner *nc_owner_new(void);
void nc_owner_release(NcOwner *o); /* marks dead; freed when no job holds it */

typedef struct NcJob NcJob;
typedef void (*NcJobDone)(NcJob *job, void *data);

struct NcJob {
  int refs; /* main thread only; the worker never changes it */
  NcJobKind kind;
  NcJobDone done;
  void *data;
  void (*destroy_data)(void *); /* frees data after done, delivered or not */
  NcOwner *owner;
  void *tag;
  bool read_only;   /* may be retried after a reconnect */
  bool savepoint;   /* wrap in SAVEPOINT inside a PostgreSQL transaction */
  bool record;      /* record the statement in history on success */
  bool was_running; /* set when the worker picked it up */

  /* Inputs */
  char *sql, *table, *where;
  bool approximate;
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
  int64_t count;
  bool count_approximate;
  int64_t affected;
  char *error; /* NULL on success */
  int code;    /* LACE_OK or the error code */
  bool cancelled;
  double elapsed_ms;
  int tx_state; /* LACE_TX_* after the job, -1 unknown */
  char *last_sql;
};

NcJob *nc_job_exec(const char *sql);
/* A read: retried after a reconnect, savepoint inside a PostgreSQL transaction */
NcJob *nc_job_read(const char *sql);
NcJob *nc_job_schema(const char *table);
NcJob *nc_job_tables(void);
NcJob *nc_job_table_count(const char *table, const char *where, bool approximate);
NcJob *nc_job_count_sql(const char *sql);
NcJob *nc_job_update(const char *table, const LaceRowKey *key, const LaceRowSnapshot *snap,
                     const char *column, const LaceValue *value);
NcJob *nc_job_delete(const char *table, const LaceRowKey *key, const LaceRowSnapshot *snap);
NcJob *nc_job_insert(const char *table, const char *const *columns, const LaceValue *values,
                     size_t n);
NcJob *nc_job_disconnect(void);

NcJob *nc_job_ref(NcJob *j);
void nc_job_unref(NcJob *j);
LaceResult *nc_job_steal_result(NcJob *j);
LaceSchema *nc_job_steal_schema(NcJob *j);
const char *nc_job_error(const NcJob *j);

#endif
