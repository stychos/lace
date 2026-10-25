/*
 * Lace GTK frontend
 * Job constructors and lifecycle (running them is connection.c)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "jobs.h"
#include <util/mem.h>
#include <util/str.h>
#include <stdlib.h>
#include <string.h>

static LaceJob *job_new(LaceJobKind kind) {
  LaceJob *job = safe_calloc(1, sizeof(*job));
  g_atomic_ref_count_init(&job->ref);
  job->kind = kind;
  job->code = LACE_OK;
  job->affected = -1;
  job->count = -1;
  job->tx_state = -1;
  job->value = lace_value_null();
  return job;
}

LaceJob *lace_job_exec(const char *sql) {
  LaceJob *job = job_new(LACE_JOB_EXEC);
  job->sql = str_dup(sql ? sql : "");
  job->record = TRUE;
  return job;
}

LaceJob *lace_job_read(const char *sql) {
  LaceJob *job = job_new(LACE_JOB_EXEC);
  job->sql = str_dup(sql ? sql : "");
  job->read_only = TRUE;
  job->savepoint = TRUE;
  return job;
}

LaceJob *lace_job_schema(const char *table) {
  LaceJob *job = job_new(LACE_JOB_SCHEMA);
  job->table = str_dup(table);
  job->read_only = TRUE;
  job->savepoint = TRUE;
  return job;
}

LaceJob *lace_job_tables(void) {
  LaceJob *job = job_new(LACE_JOB_TABLES);
  job->read_only = TRUE;
  job->savepoint = TRUE;
  return job;
}

LaceJob *lace_job_table_count(const char *table, const char *where,
                              gboolean approximate) {
  LaceJob *job = job_new(LACE_JOB_COUNT);
  job->table = str_dup(table);
  job->where = where ? str_dup(where) : NULL;
  job->approximate = approximate;
  job->read_only = TRUE;
  job->savepoint = TRUE;
  return job;
}

LaceJob *lace_job_count_sql(const char *sql) {
  LaceJob *job = job_new(LACE_JOB_COUNT);
  job->sql = str_dup(sql);
  job->read_only = TRUE;
  job->savepoint = TRUE;
  return job;
}

LaceJob *lace_job_update(const char *table, const LaceRowKey *key,
                         const LaceRowSnapshot *snapshot, const char *column,
                         const LaceValue *value) {
  LaceJob *job = job_new(LACE_JOB_UPDATE);
  job->table = str_dup(table);
  lace_row_key_copy(&job->key, key);
  if (snapshot) {
    job->snapshot.count = snapshot->count;
    job->snapshot.names = safe_calloc(snapshot->count + 1, sizeof(char *));
    job->snapshot.values = safe_calloc(snapshot->count + 1, sizeof(LaceValue));
    for (size_t i = 0; i < snapshot->count; i++) {
      job->snapshot.names[i] = str_dup(snapshot->names[i]);
      job->snapshot.values[i] = lace_value_copy(&snapshot->values[i]);
    }
  }
  job->column = str_dup(column);
  job->value = lace_value_copy(value);
  job->record = TRUE;
  return job;
}

LaceJob *lace_job_delete(const char *table, const LaceRowKey *key,
                         const LaceRowSnapshot *snapshot) {
  LaceJob *job = lace_job_update(table, key, snapshot, "", NULL);
  job->kind = LACE_JOB_DELETE;
  free(job->column);
  job->column = NULL;
  return job;
}

LaceJob *lace_job_insert(const char *table, const char *const *columns,
                         const LaceValue *values, size_t n) {
  LaceJob *job = job_new(LACE_JOB_INSERT);
  job->table = str_dup(table);
  job->num_values = n;
  job->columns = safe_calloc(n + 1, sizeof(char *));
  job->values = safe_calloc(n + 1, sizeof(LaceValue));
  for (size_t i = 0; i < n; i++) {
    job->columns[i] = str_dup(columns[i]);
    job->values[i] = lace_value_copy(&values[i]);
  }
  job->record = TRUE;
  return job;
}

LaceJob *lace_job_disconnect(void) { return job_new(LACE_JOB_DISCONNECT); }

LaceJob *lace_job_table_info(const char *table) {
  LaceJob *j = lace_job_schema(table);
  j->kind = LACE_JOB_TABLE_INFO;
  return j;
}

LaceJob *lace_job_dependents(const char *table) {
  LaceJob *j = lace_job_schema(table);
  j->kind = LACE_JOB_DEPENDENTS;
  return j;
}

LaceJob *lace_job_definition(LaceObjectKind kind, const char *name, const char *table, const char *detail) {
  LaceJob *j = lace_job_schema(name);
  j->kind = LACE_JOB_DEFINITION;
  j->object_kind = (int)kind;
  j->sql = g_strdup(name ? name : "");
  free(j->table);
  j->table = table ? g_strdup(table) : NULL;
  j->column = detail ? g_strdup(detail) : NULL;
  return j;
}

LaceJob *lace_job_ref(LaceJob *job) {
  g_atomic_ref_count_inc(&job->ref);
  return job;
}

void lace_job_unref(LaceJob *job) {
  if (!job || !g_atomic_ref_count_dec(&job->ref))
    return;
  if (job->has_owner)
    g_weak_ref_clear(&job->owner);
  str_secure_free(job->sql);
  free(job->table);
  free(job->where);
  lace_row_key_free(&job->key);
  lace_row_snapshot_free(&job->snapshot);
  free(job->column);
  lace_value_free(&job->value);
  for (size_t i = 0; i < job->num_values; i++) {
    free(job->columns[i]);
    lace_value_free(&job->values[i]);
  }
  free(job->columns);
  free(job->values);
  lace_result_free(job->result);
  lace_schema_free(job->schema);
  lace_tables_free(job->tables, job->num_tables);
  lace_objects_free(job->objects, job->num_objects);
  lace_table_info_free(job->info);
  lace_autoinc_free(job->autoinc);
  lace_dependents_free(job->deps, job->num_deps);
  free(job->definition);
  free(job->error);
  str_secure_free(job->last_sql);
  free(job);
}

LaceResult *lace_job_steal_result(LaceJob *job) {
  LaceResult *r = job->result;
  job->result = NULL;
  return r;
}

LaceSchema *lace_job_steal_schema(LaceJob *job) {
  LaceSchema *s = job->schema;
  job->schema = NULL;
  return s;
}

const char *lace_job_error(const LaceJob *job) {
  if (!job)
    return "No job";
  if (job->cancelled)
    return "Cancelled";
  return job->error ? job->error : "Unknown error";
}
