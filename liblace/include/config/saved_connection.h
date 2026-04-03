/*
 * liblace - Lace Client Library
 * Saved Connection Type
 *
 * This module provides the SavedConnection type for storing database
 * connection credentials and building connection strings.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_SAVED_CONNECTION_H
#define LIBLACE_SAVED_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * Saved Connection
 * ========================================================================== */

/* Saved connection entry (serializable) */
typedef struct {
  char *id;       /* Unique UUID string */
  char *name;     /* Display name */
  char *driver;   /* sqlite, postgres, mysql, mariadb */
  char *host;     /* Host (empty for sqlite) */
  char *database; /* Database path or name */
  char *user;     /* Username (empty for sqlite) */
  char *password; /* Password (if save_password is true) */
  int port;       /* Port number (0 for default) */
  bool save_password;

  /* SSH tunnel fields */
  bool ssh;            /* true if connection uses SSH tunnel */
  char *ssh_host;      /* SSH server hostname */
  char *ssh_user;      /* SSH username (NULL = use ~/.ssh/config) */
  char *ssh_password;  /* SSH password (NULL = key auth) */
  int ssh_port;        /* SSH port (0 = default 22) */
  bool save_ssh_password;
} LaceSavedConnection;

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

/* Create a new empty saved connection with generated UUID */
LaceSavedConnection *lace_saved_conn_new(void);

/* Create a saved connection from a connection string
 * Returns NULL on error (sets *error if provided) */
LaceSavedConnection *lace_saved_conn_from_connstr(const char *connstr,
                                                  char **error);

/* Free a saved connection and all its fields */
void lace_saved_conn_free(LaceSavedConnection *conn);

/* Deep copy a saved connection */
LaceSavedConnection *lace_saved_conn_copy(const LaceSavedConnection *conn);

/* ==========================================================================
 * Connection String Operations
 * ========================================================================== */

/* Build a connection URL from saved connection
 * Format: driver://[user[:password]@][host[:port]/]database
 * Returns allocated string, caller must free */
char *lace_saved_conn_to_connstr(const LaceSavedConnection *conn);

/* ==========================================================================
 * Utilities
 * ========================================================================== */

/* Generate a new UUID string (v4)
 * Returns allocated string, caller must free */
char *lace_generate_uuid(void);

/* Get default port for a driver (0 = no default) */
int lace_driver_default_port(const char *driver);

/* Validate driver name
 * Returns true if valid: sqlite, postgres, postgresql, mysql, mariadb */
bool lace_driver_is_valid(const char *driver);

/* Normalize driver name (e.g., "postgresql" -> "postgres")
 * Returns static string, do not free */
const char *lace_driver_normalize(const char *driver);

#endif /* LIBLACE_SAVED_CONNECTION_H */
