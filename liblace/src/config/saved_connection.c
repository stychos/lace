/*
 * liblace - Lace Client Library
 * Saved Connection Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../include/config/saved_connection.h"
#include "../../include/util/connstr.h"
#include "../../include/util/mem.h"
#include "../../include/util/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==========================================================================
 * UUID Generation
 * ========================================================================== */

char *lace_generate_uuid(void) {
  static bool seeded = false;
  if (!seeded) {
    srand((unsigned int)time(NULL) ^ (unsigned int)((size_t)&seeded));
    seeded = true;
  }

  /* Generate UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
   * where y is one of: 8, 9, a, b */
  char uuid[37];
  snprintf(uuid, sizeof(uuid),
           "%08x-%04x-4%03x-%x%03x-%012llx",
           (unsigned int)(rand() & 0xFFFFFFFF),
           (unsigned int)(rand() & 0xFFFF),
           (unsigned int)(rand() & 0x0FFF),
           (unsigned int)(8 + (rand() & 0x3)),
           (unsigned int)(rand() & 0x0FFF),
           (unsigned long long)(((unsigned long long)rand() << 32) | rand()) & 0xFFFFFFFFFFFFULL);

  return str_dup(uuid);
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

LaceSavedConnection *lace_saved_conn_new(void) {
  LaceSavedConnection *conn = safe_calloc(1, sizeof(LaceSavedConnection));
  if (!conn)
    return NULL;

  conn->id = lace_generate_uuid();
  return conn;
}

LaceSavedConnection *lace_saved_conn_from_connstr(const char *connstr_str,
                                                  char **error) {
  if (!connstr_str || !connstr_str[0]) {
    if (error)
      *error = str_dup("Empty connection string");
    return NULL;
  }

  /* Parse the connection string */
  char *parse_err = NULL;
  ConnString *cs = connstr_parse(connstr_str, &parse_err);
  if (!cs) {
    if (error)
      *error = parse_err ? parse_err : str_dup("Invalid connection string format");
    else
      free(parse_err);
    return NULL;
  }

  /* Validate driver */
  if (!lace_driver_is_valid(cs->driver)) {
    if (error)
      *error = str_printf("Unknown driver: %s", cs->driver);
    connstr_free(cs);
    return NULL;
  }

  /* Create saved connection */
  LaceSavedConnection *conn = lace_saved_conn_new();
  if (!conn) {
    if (error)
      *error = str_dup("Out of memory");
    connstr_free(cs);
    return NULL;
  }

  conn->driver = str_dup(lace_driver_normalize(cs->driver));
  conn->host = cs->host ? str_dup(cs->host) : NULL;
  conn->database = cs->database ? str_dup(cs->database) : NULL;
  conn->user = cs->user ? str_dup(cs->user) : NULL;
  conn->password = cs->password ? str_dup(cs->password) : NULL;
  conn->port = cs->port;
  conn->save_password = (cs->password != NULL);

  /* Generate default name from database path/name */
  if (conn->database) {
    /* For file paths, use just the filename */
    const char *name = conn->database;
    const char *slash = strrchr(conn->database, '/');
    if (slash)
      name = slash + 1;
#ifdef _WIN32
    const char *backslash = strrchr(name, '\\');
    if (backslash)
      name = backslash + 1;
#endif
    conn->name = str_dup(name);
  } else {
    conn->name = str_dup("New Connection");
  }

  connstr_free(cs);
  return conn;
}

void lace_saved_conn_free(LaceSavedConnection *conn) {
  if (!conn)
    return;

  free(conn->id);
  free(conn->name);
  free(conn->driver);
  free(conn->host);
  free(conn->database);
  free(conn->user);
  str_secure_free(conn->password); /* Securely wipe password from memory */
  free(conn);
}

LaceSavedConnection *lace_saved_conn_copy(const LaceSavedConnection *conn) {
  if (!conn)
    return NULL;

  LaceSavedConnection *copy = safe_calloc(1, sizeof(LaceSavedConnection));
  if (!copy)
    return NULL;

  copy->id = conn->id ? str_dup(conn->id) : lace_generate_uuid();
  copy->name = conn->name ? str_dup(conn->name) : NULL;
  copy->driver = conn->driver ? str_dup(conn->driver) : NULL;
  copy->host = conn->host ? str_dup(conn->host) : NULL;
  copy->database = conn->database ? str_dup(conn->database) : NULL;
  copy->user = conn->user ? str_dup(conn->user) : NULL;
  copy->password = conn->password ? str_dup(conn->password) : NULL;
  copy->port = conn->port;
  copy->save_password = conn->save_password;

  return copy;
}

/* ==========================================================================
 * Connection String Operations
 * ========================================================================== */

char *lace_saved_conn_to_connstr(const LaceSavedConnection *conn) {
  if (!conn || !conn->driver)
    return NULL;

  StringBuilder *sb = sb_new(256);
  if (!sb)
    return NULL;

  /* Start with driver:// */
  sb_printf(sb, "%s://", conn->driver);

  /* Add user[:password]@ if present */
  if (conn->user && conn->user[0]) {
    sb_append(sb, conn->user);
    if (conn->password && conn->password[0] && conn->save_password) {
      sb_append_char(sb, ':');
      sb_append(sb, conn->password);
    }
    sb_append_char(sb, '@');
  }

  /* Add host[:port]/ if present (not for sqlite) */
  bool is_sqlite = str_eq(conn->driver, "sqlite");
  if (!is_sqlite && conn->host && conn->host[0]) {
    sb_append(sb, conn->host);
    if (conn->port > 0) {
      sb_printf(sb, ":%d", conn->port);
    }
    sb_append_char(sb, '/');
  }

  /* Add database */
  if (conn->database && conn->database[0]) {
    sb_append(sb, conn->database);
  }

  return sb_to_string(sb);
}

/* ==========================================================================
 * Driver Utilities
 * ========================================================================== */

int lace_driver_default_port(const char *driver) {
  if (!driver)
    return 0;

  if (str_eq(driver, "postgres") || str_eq(driver, "postgresql") ||
      str_eq(driver, "pg"))
    return 5432;

  if (str_eq(driver, "mysql") || str_eq(driver, "mariadb"))
    return 3306;

  return 0; /* sqlite has no port */
}

bool lace_driver_is_valid(const char *driver) {
  if (!driver)
    return false;

  return str_eq(driver, "sqlite") ||
         str_eq(driver, "postgres") ||
         str_eq(driver, "postgresql") ||
         str_eq(driver, "pg") ||
         str_eq(driver, "mysql") ||
         str_eq(driver, "mariadb");
}

const char *lace_driver_normalize(const char *driver) {
  if (!driver)
    return "sqlite";

  if (str_eq(driver, "postgresql") || str_eq(driver, "pg"))
    return "postgres";

  if (str_eq(driver, "sqlite") ||
      str_eq(driver, "postgres") ||
      str_eq(driver, "mysql") ||
      str_eq(driver, "mariadb"))
    return driver;

  return "sqlite";
}
