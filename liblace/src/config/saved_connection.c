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
#include <stdint.h>
#include "../os/os.h"
#include <time.h>

/* ==========================================================================
 * UUID Generation
 * ========================================================================== */

char *lace_generate_uuid(void) {
  /* A version 4 UUID from the system's random source: rand() was neither
   * thread-safe nor random enough, and reseeded the program's own rand() */
  uint8_t b[16];
  if (!lace_os_random(b, sizeof(b))) {
    /* No random source: time and address still make a usable id */
    uint64_t t = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)b;
    for (size_t i = 0; i < sizeof(b); i++)
      b[i] = (uint8_t)(t >> ((i % 8) * 8)) ^ (uint8_t)(i * 37);
  }
  b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); /* Version 4 */
  b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); /* RFC 4122 variant */
  char uuid[37];
  snprintf(uuid, sizeof(uuid),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);
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

  /* SSH tunnel fields */
  conn->ssh = cs->ssh;
  conn->ssh_host = cs->ssh_host ? str_dup(cs->ssh_host) : NULL;
  conn->ssh_user = cs->ssh_user ? str_dup(cs->ssh_user) : NULL;
  conn->ssh_password = cs->ssh_password ? str_dup(cs->ssh_password) : NULL;
  conn->ssh_port = cs->ssh_port;
  conn->save_ssh_password = (cs->ssh_password != NULL);
  const char *key = connstr_get_option(cs, "ssh_key");
  conn->ssh_key = key ? str_dup(key) : NULL;

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
  str_secure_free(conn->password);
  free(conn->ssh_host);
  free(conn->ssh_user);
  str_secure_free(conn->ssh_password);
  free(conn->ssh_key);
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

  copy->ssh = conn->ssh;
  copy->ssh_host = conn->ssh_host ? str_dup(conn->ssh_host) : NULL;
  copy->ssh_user = conn->ssh_user ? str_dup(conn->ssh_user) : NULL;
  copy->ssh_password = conn->ssh_password ? str_dup(conn->ssh_password) : NULL;
  copy->ssh_port = conn->ssh_port;
  copy->save_ssh_password = conn->save_ssh_password;
  copy->ssh_key = conn->ssh_key ? str_dup(conn->ssh_key) : NULL;

  return copy;
}

/* ==========================================================================
 * Connection String Operations
 * ========================================================================== */

/* Append "user[:password]@" with both parts URL-encoded (the parser
 * decodes them, so a password containing '@', ':' or '/' must be encoded).
 * Nothing is appended without a user. Returns false on allocation failure. */
static bool append_userinfo(StringBuilder *sb, const char *user,
                            const char *password) {
  if (!user || !user[0])
    return true;
  char *enc_user = str_url_encode(user);
  if (!enc_user)
    return false;
  sb_append(sb, enc_user);
  free(enc_user);
  if (password && password[0]) {
    char *enc_pass = str_url_encode(password);
    if (!enc_pass)
      return false;
    sb_append_char(sb, ':');
    sb_append(sb, enc_pass);
    str_secure_free(enc_pass);
  }
  sb_append_char(sb, '@');
  return true;
}

/* host[:port] for a URL: an IPv6 address goes in brackets. A host holding
 * '/', '?', '#', '@' or whitespace would change what the URL means (a path,
 * options, a user): refused (false), never written. */
static bool append_host(StringBuilder *sb, const char *host, int port) {
  if (host && host[0]) {
    for (const char *c = host; *c; c++) {
      if (strchr("/?#@ \t\r\n", *c))
        return false;
    }
    bool v6 = strchr(host, ':') && host[0] != '[';
    sb_printf(sb, v6 ? "[%s]" : "%s", host);
  }
  if (port > 0)
    sb_printf(sb, ":%d", port);
  return true;
}

char *lace_saved_conn_build_connstr(const LaceSavedConnection *conn,
                                    const char *db_password,
                                    const char *ssh_password) {
  if (!conn || !conn->driver)
    return NULL;

  /* Saved passwords are used only when the user chose to save them */
  if (!db_password && conn->save_password)
    db_password = conn->password;
  if (!ssh_password && conn->save_ssh_password)
    ssh_password = conn->ssh_password;

  StringBuilder *sb = sb_new(256);
  if (!sb)
    return NULL;
  bool ok = true;

  if (conn->ssh) {
    /* SSH tunnel format: driver+ssh://[sshuser[:sshpass]@]sshhost[:sshport]/[dbuser[:dbpass]@]dbhost[:dbport]/dbname */
    sb_printf(sb, "%s+ssh://", conn->driver);
    ok = append_userinfo(sb, conn->ssh_user, ssh_password);
    ok = ok && append_host(sb, conn->ssh_host, conn->ssh_port);
    sb_append_char(sb, '/');
    ok = ok && append_userinfo(sb, conn->user, db_password);
    ok = ok && append_host(sb, conn->host, conn->port);
    sb_append_char(sb, '/');
    if (conn->database && conn->database[0]) {
      char *db = str_url_encode(conn->database);
      ok = ok && db;
      if (db)
        sb_append(sb, db);
      free(db);
    }
  } else {
    /* Standard format: driver://[user[:password]@]host[:port]/database */
    sb_printf(sb, "%s://", conn->driver);
    bool is_sqlite = str_eq(conn->driver, "sqlite");
    /* SQLite has no user or host: everything after :// is the path (a user
     * left over from another driver would become part of it) */
    if (!is_sqlite) {
      ok = append_userinfo(sb, conn->user, db_password);
      ok = ok && append_host(sb, conn->host, conn->port);
      sb_append_char(sb, '/');
    }
    if (conn->database && conn->database[0]) {
      /* A path for SQLite ('/' kept), a name elsewhere */
      char *db = is_sqlite ? connstr_encode_path(conn->database)
                           : str_url_encode(conn->database);
      ok = ok && db;
      if (db)
        sb_append(sb, db);
      free(db);
    }
  }

  /* The key file for the tunnel, as an option the daemon reads */
  if (ok && conn->ssh && conn->ssh_key && conn->ssh_key[0]) {
    char *key = str_url_encode(conn->ssh_key);
    ok = key != NULL;
    if (key)
      sb_printf(sb, "?ssh_key=%s", key);
    free(key);
  }

  if (!ok) {
    char *partial = sb_finish(sb);
    str_secure_free(partial);
    return NULL;
  }
  return sb_finish(sb);
}

char *lace_saved_conn_to_connstr(const LaceSavedConnection *conn) {
  return lace_saved_conn_build_connstr(conn, NULL, NULL);
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
