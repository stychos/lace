#include "conn/jobs.h"
#include "conn/jobs_internal.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

struct NcOwner {
  int refs;
  bool alive;
};

NcOwner *nc_owner_new(void) {
  NcOwner *o = safe_calloc(1, sizeof *o);
  o->refs = 1;
  o->alive = true;
  return o;
}

static void owner_unref(NcOwner *o) {
  if (o && --o->refs == 0) free(o);
}

void nc_owner_release(NcOwner *o) {
  if (!o) return;
  o->alive = false;
  owner_unref(o);
}

void nc_job_set_owner(NcJob *j, NcOwner *o) {
  if (o) o->refs++;
  owner_unref(j->owner);
  j->owner = o;
}

bool nc_job_owner_alive(const NcJob *j) { return !j->owner || j->owner->alive; }

static NcJob *job_new(NcJobKind kind) {
  NcJob *j = safe_calloc(1, sizeof *j);
  j->refs = 1;
  j->kind = kind;
  j->code = LACE_OK;
  j->affected = -1;
  j->count = -1;
  j->tx_state = -1;
  j->value = lace_value_null();
  return j;
}

NcJob *nc_job_exec(const char *sql) {
  NcJob *j = job_new(NC_JOB_EXEC);
  j->sql = str_dup(sql ? sql : "");
  j->record = true;
  return j;
}

NcJob *nc_job_read(const char *sql) {
  NcJob *j = job_new(NC_JOB_EXEC);
  j->sql = str_dup(sql ? sql : "");
  j->read_only = j->savepoint = true;
  return j;
}

NcJob *nc_job_schema(const char *table) {
  NcJob *j = job_new(NC_JOB_SCHEMA);
  j->table = str_dup(table);
  j->read_only = j->savepoint = true;
  return j;
}

NcJob *nc_job_tables(void) {
  NcJob *j = job_new(NC_JOB_TABLES);
  j->read_only = j->savepoint = true;
  return j;
}

NcJob *nc_job_table_count(const char *table, const char *where, bool approximate) {
  NcJob *j = job_new(NC_JOB_COUNT);
  j->table = str_dup(table);
  j->where = where ? str_dup(where) : NULL;
  j->approximate = approximate;
  j->read_only = j->savepoint = true;
  return j;
}

NcJob *nc_job_dependents(const char *table) {
  NcJob *j = job_new(NC_JOB_DEPENDENTS);
  j->table = str_dup(table);
  j->read_only = j->savepoint = true;
  return j;
}

NcJob *nc_job_count_sql(const char *sql) {
  NcJob *j = job_new(NC_JOB_COUNT);
  j->sql = str_dup(sql);
  j->read_only = j->savepoint = true;
  return j;
}

NcJob *nc_job_update(const char *table, const LaceRowKey *key, const LaceRowSnapshot *snap,
                     const char *column, const LaceValue *value) {
  NcJob *j = job_new(NC_JOB_UPDATE);
  j->table = str_dup(table);
  lace_row_key_copy(&j->key, key);
  if (snap) {
    j->snapshot.count = snap->count;
    j->snapshot.names = safe_calloc(snap->count + 1, sizeof(char *));
    j->snapshot.values = safe_calloc(snap->count + 1, sizeof(LaceValue));
    for (size_t i = 0; i < snap->count; i++) {
      j->snapshot.names[i] = str_dup(snap->names[i]);
      j->snapshot.values[i] = lace_value_copy(&snap->values[i]);
    }
  }
  j->column = str_dup(column);
  j->value = value ? lace_value_copy(value) : lace_value_null();
  j->record = true;
  return j;
}

NcJob *nc_job_delete(const char *table, const LaceRowKey *key, const LaceRowSnapshot *snap) {
  NcJob *j = nc_job_update(table, key, snap, "", NULL);
  j->kind = NC_JOB_DELETE;
  free(j->column);
  j->column = NULL;
  return j;
}

NcJob *nc_job_insert(const char *table, const char *const *columns, const LaceValue *values,
                     size_t n) {
  NcJob *j = job_new(NC_JOB_INSERT);
  j->table = str_dup(table);
  j->num_values = n;
  j->columns = safe_calloc(n + 1, sizeof(char *));
  j->values = safe_calloc(n + 1, sizeof(LaceValue));
  for (size_t i = 0; i < n; i++) {
    j->columns[i] = str_dup(columns[i]);
    j->values[i] = lace_value_copy(&values[i]);
  }
  j->record = true;
  return j;
}

NcJob *nc_job_disconnect(void) { return job_new(NC_JOB_DISCONNECT); }

NcJob *nc_job_ref(NcJob *j) {
  j->refs++;
  return j;
}

void nc_job_unref(NcJob *j) {
  if (!j || --j->refs > 0) return;
  owner_unref(j->owner);
  str_secure_free(j->sql);
  free(j->table);
  free(j->where);
  lace_row_key_free(&j->key);
  lace_row_snapshot_free(&j->snapshot);
  free(j->column);
  lace_value_free(&j->value);
  for (size_t i = 0; i < j->num_values; i++) {
    free(j->columns[i]);
    lace_value_free(&j->values[i]);
  }
  free(j->columns);
  free(j->values);
  lace_result_free(j->result);
  lace_schema_free(j->schema);
  lace_tables_free(j->tables, j->num_tables);
  lace_dependents_free(j->deps, j->num_deps);
  free(j->error);
  str_secure_free(j->last_sql);
  free(j);
}

LaceResult *nc_job_steal_result(NcJob *j) {
  LaceResult *r = j->result;
  j->result = NULL;
  return r;
}

LaceSchema *nc_job_steal_schema(NcJob *j) {
  LaceSchema *s = j->schema;
  j->schema = NULL;
  return s;
}

const char *nc_job_error(const NcJob *j) {
  if (!j) return "No job";
  if (j->cancelled) return "Cancelled";
  return j->error ? j->error : "Unknown error";
}
