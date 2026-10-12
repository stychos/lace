/*
 * Lace
 * Database manager - driver registry and connection entry points
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../constants.h"
#include "util/str.h"
#include "util/connstr.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global driver registry */
static DbDriver *g_drivers[LACED_MAX_DRIVERS];
static size_t g_num_drivers = 0;
static bool g_initialized = false;

extern DbDriver sqlite_driver;
extern DbDriver postgres_driver;
extern DbDriver mysql_driver;
extern DbDriver mariadb_driver;

void db_init(void) {
  if (g_initialized)
    return;

  /* Register built-in drivers */
  db_register_driver(&sqlite_driver);
  db_register_driver(&postgres_driver);
  db_register_driver(&mysql_driver);
  db_register_driver(&mariadb_driver);

  for (size_t i = 0; i < g_num_drivers; i++) {
    if (g_drivers[i] && g_drivers[i]->library_init) {
      g_drivers[i]->library_init();
    }
  }

  g_initialized = true;
}

void db_cleanup(void) {
  /* Call library cleanup for all drivers */
  for (size_t i = 0; i < g_num_drivers; i++) {
    if (g_drivers[i] && g_drivers[i]->library_cleanup) {
      g_drivers[i]->library_cleanup();
    }
  }

  g_num_drivers = 0;
  g_initialized = false;
}

void db_register_driver(DbDriver *driver) {
  if (!driver || g_num_drivers >= LACED_MAX_DRIVERS)
    return;

  /* Check for duplicate */
  for (size_t i = 0; i < g_num_drivers; i++) {
    if (str_eq(g_drivers[i]->name, driver->name)) {
      g_drivers[i] = driver; /* Replace */
      return;
    }
  }

  g_drivers[g_num_drivers++] = driver;
}

DbDriver *db_get_driver(const char *name) {
  if (!name)
    return NULL;

  for (size_t i = 0; i < g_num_drivers; i++) {
    if (str_eq(g_drivers[i]->name, name)) {
      return g_drivers[i];
    }
    /* Handle aliases */
    if ((str_eq(name, "postgresql") || str_eq(name, "pg")) &&
        str_eq(g_drivers[i]->name, "postgres")) {
      return g_drivers[i];
    }
    if (str_eq(name, "mariadb") && str_eq(g_drivers[i]->name, "mysql")) {
      return g_drivers[i];
    }
  }

  return NULL;
}

DbDriver **db_get_all_drivers(size_t *count) {
  if (count)
    *count = g_num_drivers;
  return g_drivers;
}

/* High-level API */

DbConnection *db_connect(const char *connstr, char **err) {
  if (!g_initialized) {
    db_init();
  }

  ConnString *cs = connstr_parse(connstr, err);
  if (!cs) {
    return NULL;
  }

  if (!connstr_validate(cs, err)) {
    connstr_free(cs);
    return NULL;
  }

  DbDriver *driver = db_get_driver(cs->driver);
  if (!driver) {
    if (err) {
      *err = str_printf("Unknown driver: %s", cs->driver);
      /* str_printf can return NULL on allocation failure - callers handle this */
    }
    connstr_free(cs);
    return NULL;
  }

  DbConnection *conn = driver->connect(connstr, err);
  connstr_free(cs);

  return conn;
}

void db_disconnect(DbConnection *conn) {
  if (!conn || !conn->driver)
    return;

  conn->driver->disconnect(conn);
}

ResultSet *db_query(DbConnection *conn, const char *sql, char **err) {
  if (!conn || !conn->driver || !conn->driver->query) {
    err_set(err, "Not supported");
    return NULL;
  }
  return conn->driver->query(conn, sql, err);
}
