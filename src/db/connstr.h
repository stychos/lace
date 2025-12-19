/*
 * lace - Database Viewer and Manager
 * Connection string parser
 */

#ifndef LACE_CONNSTR_H
#define LACE_CONNSTR_H

#include <stdbool.h>
#include <stddef.h>

/* Parsed connection string */
typedef struct {
  char *driver;   /* Driver name: sqlite, postgres, mysql */
  char *user;     /* Username (optional) */
  char *password; /* Password (optional) */
  char *host;     /* Hostname or IP (optional for sqlite) */
  int port;       /* Port number (0 for default) */
  char *database; /* Database name or file path */
  char *schema;   /* Schema name (optional) */

  /* Additional options as key-value pairs */
  char **option_keys;
  char **option_values;
  size_t num_options;

  /* Raw connection string */
  char *raw;
} ConnString;

/*
 * Parse a connection string
 *
 * Format: driver://[user[:password]@]host[:port]/database[?options]
 *
 * Examples:
 *   sqlite:///path/to/database.db
 *   sqlite://./relative/path.db
 *   postgres://user:pass@localhost:5432/mydb
 *   postgres://user@localhost/mydb?sslmode=require
 *   mysql://root@127.0.0.1:3306/test
 *   mysql://user:pass@host/db?charset=utf8mb4
 *
 * Returns NULL on parse error, with error message in *err if provided.
 */
ConnString *connstr_parse(const char *str, char **err);

/* Free a parsed connection string */
void connstr_free(ConnString *cs);

/* Get an option value by key, returns NULL if not found */
const char *connstr_get_option(const ConnString *cs, const char *key);

/* Get an option value with default */
const char *connstr_get_option_default(const ConnString *cs, const char *key,
                                       const char *default_val);

/* Check if an option exists */
bool connstr_has_option(const ConnString *cs, const char *key);

/* Get port with default for driver */
int connstr_get_port(const ConnString *cs);

/* Build a connection string from components */
char *connstr_build(const char *driver, const char *user, const char *password,
                    const char *host, int port, const char *database,
                    const char **option_keys, const char **option_values,
                    size_t num_options);

/* Validate a connection string (check driver, required fields) */
bool connstr_validate(const ConnString *cs, char **err);

/* Default ports for known drivers */
#define CONNSTR_PORT_POSTGRES 5432
#define CONNSTR_PORT_MYSQL 3306

#endif /* LACE_CONNSTR_H */
