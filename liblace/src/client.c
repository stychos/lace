/*
 * liblace - Lace Client Library
 * Client implementation - daemon spawning and IPC
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../include/lace.h"
#include "rpc.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Default timeout in milliseconds */
#define DEFAULT_TIMEOUT_MS 30000

/* Client structure */
struct lace_client {
  pid_t daemon_pid;       /* Daemon process ID */
  FILE *to_daemon;        /* Write to daemon stdin */
  FILE *from_daemon;      /* Read from daemon stdout */
  int timeout_ms;         /* Request timeout */
  char *last_error;       /* Last error message */
  int64_t next_id;        /* Next request ID */
  bool connected;         /* Whether daemon is running */
};

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Set error message */
static void set_error(lace_client_t *client, const char *msg) {
  free(client->last_error);
  client->last_error = msg ? strdup(msg) : NULL;
}

/* Find daemon executable */
static char *find_daemon(const char *daemon_path) {
  if (daemon_path) {
    /* Check if the provided path exists and is executable */
    if (access(daemon_path, X_OK) == 0) {
      return strdup(daemon_path);
    }
    return NULL;
  }

  /* Search in common locations */
  const char *search_paths[] = {
      "./laced/build/laced",      /* Development build */
      "./build/laced",            /* Local build */
      "../laced/build/laced",     /* Sibling directory */
      "../../laced/build/laced",  /* Frontend in frontends/ subdir */
      "/usr/local/bin/laced",     /* Standard install */
      "/usr/bin/laced",           /* System install */
      NULL
  };

  for (int i = 0; search_paths[i]; i++) {
    if (access(search_paths[i], X_OK) == 0) {
      return strdup(search_paths[i]);
    }
  }

  /* Try PATH */
  const char *path_env = getenv("PATH");
  if (path_env) {
    char *path_copy = strdup(path_env);
    if (path_copy) {
      char *saveptr;
      char *dir = strtok_r(path_copy, ":", &saveptr);
      while (dir) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/laced", dir);
        if (access(full_path, X_OK) == 0) {
          free(path_copy);
          return strdup(full_path);
        }
        dir = strtok_r(NULL, ":", &saveptr);
      }
      free(path_copy);
    }
  }

  return NULL;
}

/* Spawn daemon process */
static bool spawn_daemon(lace_client_t *client, const char *daemon_path) {
  char *daemon_exe = find_daemon(daemon_path);
  if (!daemon_exe) {
    set_error(client, "Daemon executable not found");
    return false;
  }

  /* Create pipes for communication */
  int to_daemon_pipe[2];   /* Client writes, daemon reads */
  int from_daemon_pipe[2]; /* Daemon writes, client reads */

  if (pipe(to_daemon_pipe) < 0) {
    free(daemon_exe);
    set_error(client, "Failed to create pipe");
    return false;
  }

  if (pipe(from_daemon_pipe) < 0) {
    close(to_daemon_pipe[0]);
    close(to_daemon_pipe[1]);
    free(daemon_exe);
    set_error(client, "Failed to create pipe");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(to_daemon_pipe[0]);
    close(to_daemon_pipe[1]);
    close(from_daemon_pipe[0]);
    close(from_daemon_pipe[1]);
    free(daemon_exe);
    set_error(client, "Fork failed");
    return false;
  }

  if (pid == 0) {
    /* Child process - become the daemon */

    /* Set up stdin from pipe */
    close(to_daemon_pipe[1]); /* Close write end */
    dup2(to_daemon_pipe[0], STDIN_FILENO);
    close(to_daemon_pipe[0]);

    /* Set up stdout to pipe */
    close(from_daemon_pipe[0]); /* Close read end */
    dup2(from_daemon_pipe[1], STDOUT_FILENO);
    close(from_daemon_pipe[1]);

    /* Redirect stderr to /dev/null or keep for debugging */
    /* int devnull = open("/dev/null", O_WRONLY);
       if (devnull >= 0) {
         dup2(devnull, STDERR_FILENO);
         close(devnull);
       } */

    /* Execute daemon */
    execl(daemon_exe, daemon_exe, "--stdio", NULL);

    /* If exec fails, exit */
    _exit(127);
  }

  /* Parent process */
  free(daemon_exe);

  /* Close unused pipe ends */
  close(to_daemon_pipe[0]);   /* Close read end */
  close(from_daemon_pipe[1]); /* Close write end */

  /* Create FILE streams */
  client->to_daemon = fdopen(to_daemon_pipe[1], "w");
  client->from_daemon = fdopen(from_daemon_pipe[0], "r");

  if (!client->to_daemon || !client->from_daemon) {
    if (client->to_daemon) fclose(client->to_daemon);
    if (client->from_daemon) fclose(client->from_daemon);
    close(to_daemon_pipe[1]);
    close(from_daemon_pipe[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    set_error(client, "Failed to create file streams");
    return false;
  }

  /* Disable buffering for immediate communication */
  setvbuf(client->to_daemon, NULL, _IONBF, 0);
  setvbuf(client->from_daemon, NULL, _IONBF, 0);

  client->daemon_pid = pid;
  client->connected = true;

  return true;
}

/* ==========================================================================
 * Client Lifecycle
 * ========================================================================== */

lace_client_t *lace_client_create(const char *daemon_path) {
  lace_client_t *client = calloc(1, sizeof(lace_client_t));
  if (!client) {
    return NULL;
  }

  client->timeout_ms = DEFAULT_TIMEOUT_MS;
  client->next_id = 1;

  if (!spawn_daemon(client, daemon_path)) {
    /* Error already set in spawn_daemon */
    /* Don't free client - caller needs to check error */
    return client;
  }

  return client;
}

void lace_client_destroy(lace_client_t *client) {
  if (!client) {
    return;
  }

  if (client->connected && client->daemon_pid > 0) {
    /* Try graceful shutdown first */
    if (client->to_daemon) {
      lace_shutdown(client);
    }

    /* Close streams */
    if (client->to_daemon) {
      fclose(client->to_daemon);
    }
    if (client->from_daemon) {
      fclose(client->from_daemon);
    }

    /* Wait for daemon to exit, with timeout */
    int status;
    int wait_result = waitpid(client->daemon_pid, &status, WNOHANG);
    if (wait_result == 0) {
      /* Daemon still running, send SIGTERM */
      kill(client->daemon_pid, SIGTERM);
      usleep(100000); /* 100ms */
      wait_result = waitpid(client->daemon_pid, &status, WNOHANG);
      if (wait_result == 0) {
        /* Still running, force kill */
        kill(client->daemon_pid, SIGKILL);
        waitpid(client->daemon_pid, &status, 0);
      }
    }
  }

  free(client->last_error);
  free(client);
}

bool lace_client_connected(const lace_client_t *client) {
  return client && client->connected;
}

const char *lace_client_error(const lace_client_t *client) {
  return client ? client->last_error : "Invalid client";
}

/* ==========================================================================
 * Configuration
 * ========================================================================== */

void lace_set_timeout(lace_client_t *client, int timeout_ms) {
  if (client) {
    client->timeout_ms = timeout_ms;
  }
}

int lace_get_timeout(const lace_client_t *client) {
  return client ? client->timeout_ms : 0;
}

/* ==========================================================================
 * Database Connection
 * ========================================================================== */

int lace_connect(lace_client_t *client, const char *connstr,
                 const char *password, int *conn_id) {
  if (!client || !client->connected || !connstr || !conn_id) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddStringToObject(params, "connstr", connstr);
  if (password) {
    cJSON_AddStringToObject(params, "password", password);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connect", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  cJSON *id_json = cJSON_GetObjectItem(result, "conn_id");
  if (!id_json || !cJSON_IsNumber(id_json)) {
    cJSON_Delete(result);
    return LACE_ERR_INTERNAL_ERROR;
  }

  *conn_id = id_json->valueint;
  cJSON_Delete(result);
  return LACE_OK;
}

int lace_disconnect(lace_client_t *client, int conn_id) {
  if (!client || !client->connected) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "disconnect", params, &result);
  cJSON_Delete(params);
  cJSON_Delete(result);

  return err;
}

int lace_reconnect(lace_client_t *client, int conn_id, const char *password) {
  /* Reconnect is currently implemented as disconnect + connect
   * This would need the original connection string stored */
  (void)client;
  (void)conn_id;
  (void)password;
  return LACE_ERR_INTERNAL_ERROR; /* TODO: Implement */
}

int lace_list_connections(lace_client_t *client, LaceConnInfo **info,
                          size_t *count) {
  if (!client || !client->connected || !info || !count) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "connections", NULL, &result);
  if (err != LACE_OK) {
    return err;
  }

  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return LACE_ERR_INTERNAL_ERROR;
  }

  int num = cJSON_GetArraySize(result);
  if (num == 0) {
    *info = NULL;
    *count = 0;
    cJSON_Delete(result);
    return LACE_OK;
  }

  LaceConnInfo *arr = calloc((size_t)num, sizeof(LaceConnInfo));
  if (!arr) {
    cJSON_Delete(result);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  for (int i = 0; i < num; i++) {
    cJSON *item = cJSON_GetArrayItem(result, i);
    if (!item) continue;

    cJSON *id = cJSON_GetObjectItem(item, "id");
    cJSON *driver = cJSON_GetObjectItem(item, "driver");
    cJSON *database = cJSON_GetObjectItem(item, "database");
    cJSON *host = cJSON_GetObjectItem(item, "host");
    cJSON *port = cJSON_GetObjectItem(item, "port");
    cJSON *user = cJSON_GetObjectItem(item, "user");

    if (id && cJSON_IsNumber(id)) arr[i].id = id->valueint;
    if (driver && cJSON_IsString(driver)) {
      /* Map driver string to enum */
      if (strcmp(driver->valuestring, "sqlite") == 0) {
        arr[i].driver = LACE_DRIVER_SQLITE;
      } else if (strcmp(driver->valuestring, "postgres") == 0) {
        arr[i].driver = LACE_DRIVER_POSTGRES;
      } else if (strcmp(driver->valuestring, "mysql") == 0) {
        arr[i].driver = LACE_DRIVER_MYSQL;
      } else if (strcmp(driver->valuestring, "mariadb") == 0) {
        arr[i].driver = LACE_DRIVER_MARIADB;
      }
    }
    if (database && cJSON_IsString(database)) arr[i].database = strdup(database->valuestring);
    if (host && cJSON_IsString(host)) arr[i].host = strdup(host->valuestring);
    if (port && cJSON_IsNumber(port)) arr[i].port = port->valueint;
    if (user && cJSON_IsString(user)) arr[i].user = strdup(user->valuestring);
  }

  *info = arr;
  *count = (size_t)num;
  cJSON_Delete(result);
  return LACE_OK;
}

void lace_conn_info_array_free(LaceConnInfo *info, size_t count) {
  if (!info) return;

  for (size_t i = 0; i < count; i++) {
    free(info[i].database);
    free(info[i].host);
    free(info[i].user);
  }
  free(info);
}

/* ==========================================================================
 * Schema Discovery
 * ========================================================================== */

int lace_list_tables(lace_client_t *client, int conn_id, char ***tables,
                     size_t *count) {
  if (!client || !client->connected || !tables || !count) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "tables", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return LACE_ERR_INTERNAL_ERROR;
  }

  int num = cJSON_GetArraySize(result);
  if (num == 0) {
    *tables = NULL;
    *count = 0;
    cJSON_Delete(result);
    return LACE_OK;
  }

  char **arr = calloc((size_t)num, sizeof(char *));
  if (!arr) {
    cJSON_Delete(result);
    return LACE_ERR_OUT_OF_MEMORY;
  }

  for (int i = 0; i < num; i++) {
    cJSON *item = cJSON_GetArrayItem(result, i);
    if (item && cJSON_IsString(item)) {
      arr[i] = strdup(item->valuestring);
    }
  }

  *tables = arr;
  *count = (size_t)num;
  cJSON_Delete(result);
  return LACE_OK;
}

void lace_tables_free(char **tables, size_t count) {
  if (!tables) return;

  for (size_t i = 0; i < count; i++) {
    free(tables[i]);
  }
  free(tables);
}

int lace_get_schema(lace_client_t *client, int conn_id, const char *table,
                    LaceSchema **schema) {
  if (!client || !client->connected || !table || !schema) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "table", table);

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "schema", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  /* Parse schema from JSON */
  *schema = lace_rpc_parse_schema(result);
  cJSON_Delete(result);

  if (!*schema) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  return LACE_OK;
}

/* ==========================================================================
 * Data Queries
 * ========================================================================== */

int lace_query(lace_client_t *client, int conn_id, const char *table,
               const LaceFilter *filters, size_t num_filters,
               const LaceSort *sorts, size_t num_sorts,
               size_t offset, size_t limit, LaceResult **result) {
  if (!client || !client->connected || !table || !result) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "table", table);
  cJSON_AddNumberToObject(params, "offset", (double)offset);
  cJSON_AddNumberToObject(params, "limit", (double)(limit > 0 ? limit : 500));

  /* TODO: Add filters and sorts to params */
  (void)filters;
  (void)num_filters;
  (void)sorts;
  (void)num_sorts;

  cJSON *resp = NULL;
  int err = lace_rpc_call(client, "query", params, &resp);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  /* Parse result from JSON */
  *result = lace_rpc_parse_result(resp);
  cJSON_Delete(resp);

  if (!*result) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  return LACE_OK;
}

int lace_count(lace_client_t *client, int conn_id, const char *table,
               const LaceFilter *filters, size_t num_filters,
               size_t *count, bool *approximate) {
  if (!client || !client->connected || !table || !count) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "table", table);

  /* TODO: Add filters to params */
  (void)filters;
  (void)num_filters;

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "count", params, &result);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  cJSON *cnt = cJSON_GetObjectItem(result, "count");
  cJSON *approx = cJSON_GetObjectItem(result, "approximate");

  if (!cnt || !cJSON_IsNumber(cnt)) {
    cJSON_Delete(result);
    return LACE_ERR_INTERNAL_ERROR;
  }

  *count = (size_t)cnt->valuedouble;
  if (approximate) {
    *approximate = approx && cJSON_IsTrue(approx);
  }

  cJSON_Delete(result);
  return LACE_OK;
}

int lace_exec(lace_client_t *client, int conn_id, const char *sql,
              LaceResult **result) {
  if (!client || !client->connected || !sql) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "sql", sql);

  cJSON *resp = NULL;
  int err = lace_rpc_call(client, "exec", params, &resp);
  cJSON_Delete(params);

  if (err != LACE_OK) {
    return err;
  }

  if (result) {
    /* Check if it's a select result */
    cJSON *type = cJSON_GetObjectItem(resp, "type");
    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "select") == 0) {
      cJSON *data = cJSON_GetObjectItem(resp, "data");
      *result = lace_rpc_parse_result(data);
    } else {
      /* Non-select: create minimal result with affected count */
      *result = calloc(1, sizeof(LaceResult));
      if (*result) {
        cJSON *affected = cJSON_GetObjectItem(resp, "affected");
        if (affected && cJSON_IsNumber(affected)) {
          (*result)->total_rows = (size_t)affected->valuedouble;
        }
      }
    }
  }

  cJSON_Delete(resp);
  return LACE_OK;
}

int lace_cancel_query(lace_client_t *client, int conn_id) {
  if (!client || !client->connected) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);

  cJSON *resp = NULL;
  int err = lace_rpc_call(client, "cancel", params, &resp);
  cJSON_Delete(params);
  cJSON_Delete(resp);

  return err;
}

/* ==========================================================================
 * Data Mutations
 * ========================================================================== */

int lace_update(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk,
                const char *column, const LaceValue *value) {
  if (!client || !client->connected || !table || !pk || !column || !value) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "table", table);
  cJSON_AddStringToObject(params, "column", column);

  /* Add value */
  cJSON *val_json = lace_rpc_value_to_json(value);
  if (val_json) {
    cJSON_AddItemToObject(params, "value", val_json);
  }

  /* Add primary key array */
  cJSON *pk_array = cJSON_CreateArray();
  if (pk_array) {
    for (size_t i = 0; i < num_pk; i++) {
      cJSON *pk_item = cJSON_CreateObject();
      if (pk_item) {
        cJSON_AddStringToObject(pk_item, "column", pk[i].column);
        cJSON *pk_val = lace_rpc_value_to_json(&pk[i].value);
        if (pk_val) {
          cJSON_AddItemToObject(pk_item, "value", pk_val);
        }
        cJSON_AddItemToArray(pk_array, pk_item);
      }
    }
    cJSON_AddItemToObject(params, "pk", pk_array);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "update", params, &result);
  cJSON_Delete(params);
  cJSON_Delete(result);

  return err;
}

int lace_delete(lace_client_t *client, int conn_id, const char *table,
                const LacePkValue *pk, size_t num_pk) {
  if (!client || !client->connected || !table || !pk) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *params = cJSON_CreateObject();
  if (!params) {
    return LACE_ERR_OUT_OF_MEMORY;
  }

  cJSON_AddNumberToObject(params, "conn_id", conn_id);
  cJSON_AddStringToObject(params, "table", table);

  /* Add primary key array */
  cJSON *pk_array = cJSON_CreateArray();
  if (pk_array) {
    for (size_t i = 0; i < num_pk; i++) {
      cJSON *pk_item = cJSON_CreateObject();
      if (pk_item) {
        cJSON_AddStringToObject(pk_item, "column", pk[i].column);
        cJSON *pk_val = lace_rpc_value_to_json(&pk[i].value);
        if (pk_val) {
          cJSON_AddItemToObject(pk_item, "value", pk_val);
        }
        cJSON_AddItemToArray(pk_array, pk_item);
      }
    }
    cJSON_AddItemToObject(params, "pk", pk_array);
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "delete", params, &result);
  cJSON_Delete(params);
  cJSON_Delete(result);

  return err;
}

int lace_insert(lace_client_t *client, int conn_id, const char *table,
                const char **columns, const LaceValue *values, size_t num_columns,
                LacePkValue **out_pk, size_t *out_num_pk) {
  /* TODO: Implement insert */
  (void)client;
  (void)conn_id;
  (void)table;
  (void)columns;
  (void)values;
  (void)num_columns;
  (void)out_pk;
  (void)out_num_pk;
  return LACE_ERR_INTERNAL_ERROR;
}

void lace_pk_free(LacePkValue *pk, size_t num_pk) {
  if (!pk) return;

  for (size_t i = 0; i < num_pk; i++) {
    free(pk[i].column);
    lace_value_free(&pk[i].value);
  }
  free(pk);
}

/* ==========================================================================
 * Utility Functions
 * ========================================================================== */

int lace_ping(lace_client_t *client) {
  if (!client || !client->connected) {
    return LACE_ERR_CONNECTION_CLOSED;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "ping", NULL, &result);
  cJSON_Delete(result);
  return err;
}

int lace_version(lace_client_t *client, char **version) {
  if (!client || !client->connected || !version) {
    return LACE_ERR_INVALID_PARAMS;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "version", NULL, &result);
  if (err != LACE_OK) {
    return err;
  }

  cJSON *ver = cJSON_GetObjectItem(result, "daemon_version");
  if (ver && cJSON_IsString(ver)) {
    *version = strdup(ver->valuestring);
  } else {
    *version = strdup("unknown");
  }

  cJSON_Delete(result);
  return LACE_OK;
}

int lace_shutdown(lace_client_t *client) {
  if (!client || !client->connected) {
    return LACE_ERR_CONNECTION_CLOSED;
  }

  cJSON *result = NULL;
  int err = lace_rpc_call(client, "shutdown", NULL, &result);
  cJSON_Delete(result);
  return err;
}
