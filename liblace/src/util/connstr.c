/*
 * Lace
 * Connection string parser implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../../include/util/connstr.h"
#include "../../include/util/mem.h"
#include "../../include/util/str.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Fallback for systems without PATH_MAX */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char *decode_component(const char *s, size_t len) {
  char *temp = str_ndup(s, len);
  if (!temp)
    return NULL;
  char *decoded = str_url_decode(temp);
  free(temp);
  return decoded;
}

/* Maximum connection string length (4KB is more than sufficient) */
#define MAX_CONNSTR_LEN 4096

/*
 * Post-process a parsed connection string that contains +ssh in the driver.
 *
 * After standard parsing of "postgres+ssh://sshuser:sshpass@sshhost:22/dbuser:dbpass@dbhost:5432/mydb":
 *   driver="postgres+ssh", user="sshuser", password="sshpass", host="sshhost",
 *   port=22, database="dbuser:dbpass@dbhost:5432/mydb"
 *
 * This function:
 * 1. Strips "+ssh" from driver
 * 2. Moves user/pass/host/port to ssh_* fields
 * 3. Re-parses the "database" field as [user[:pass]@]host[:port]/dbname
 */
static bool connstr_postprocess_ssh(ConnString *cs, char **err) {
  if (!cs->driver)
    return true;

  char *plus = strstr(cs->driver, "+ssh");
  if (!plus)
    return true; /* Not SSH */

  /* Ensure +ssh is at the end of the driver name */
  if (plus[4] != '\0') {
    err_setf(err, "Invalid driver scheme: %s", cs->driver);
    return false;
  }

  /* Extract real driver name */
  char *real_driver = str_ndup(cs->driver, (size_t)(plus - cs->driver));
  if (!real_driver) {
    err_setf(err, "Out of memory");
    return false;
  }

  /* Reject sqlite+ssh */
  if (str_eq(real_driver, "sqlite")) {
    free(real_driver);
    err_setf(err, "SSH tunneling is not supported for SQLite");
    return false;
  }

  /* Move parsed authority fields to SSH fields */
  cs->ssh = true;
  cs->ssh_user = cs->user;
  cs->user = NULL;
  cs->ssh_password = cs->password;
  cs->password = NULL;
  cs->ssh_host = cs->host;
  cs->host = NULL;
  cs->ssh_port = cs->port;
  cs->port = 0;

  free(cs->driver);
  cs->driver = real_driver;

  /* Now re-parse cs->database which contains "dbuser:dbpass@dbhost:5432/mydb"
   * or just "dbhost:5432/mydb" or "dbhost/mydb" */
  if (!cs->database || *cs->database == '\0') {
    err_setf(err, "SSH connection string missing database path");
    return false;
  }

  char *db_part = cs->database;
  cs->database = NULL;

  const char *p = db_part;
  const char *at = strchr(p, '@');
  const char *slash = strchr(p, '/');

  /* Parse [user[:pass]@] - only if @ comes before / */
  if (at && (!slash || at < slash)) {
    const char *colon = strchr(p, ':');
    if (colon && colon < at) {
      cs->user = decode_component(p, (size_t)(colon - p));
      cs->password = decode_component(colon + 1, (size_t)(at - colon - 1));
      if (!cs->user || !cs->password) {
        free(db_part);
        err_setf(err, "Out of memory");
        return false;
      }
    } else {
      cs->user = decode_component(p, (size_t)(at - p));
      if (!cs->user) {
        free(db_part);
        err_setf(err, "Out of memory");
        return false;
      }
    }
    p = at + 1;
  }

  /* Parse host[:port] before the next / */
  slash = strchr(p, '/');
  const char *host_end = slash ? slash : (p + strlen(p));

  if (host_end > p) {
    /* Find last colon for port */
    const char *colon = NULL;
    for (const char *c = host_end - 1; c >= p; c--) {
      if (*c == ':') {
        colon = c;
        break;
      }
    }
    if (colon) {
      cs->host = str_ndup(p, (size_t)(colon - p));
      char *port_str = str_ndup(colon + 1, (size_t)(host_end - colon - 1));
      if (port_str) {
        char *endptr;
        errno = 0;
        long port_val = strtol(port_str, &endptr, 10);
        if (errno == 0 && *endptr == '\0' && port_val > 0 && port_val <= 65535) {
          cs->port = (int)port_val;
        }
        free(port_str);
      }
    } else {
      cs->host = str_ndup(p, (size_t)(host_end - p));
    }
    if (!cs->host) {
      free(db_part);
      err_setf(err, "Out of memory");
      return false;
    }
  }

  /* Parse /database */
  if (slash && *(slash + 1)) {
    cs->database = str_url_decode(slash + 1);
    if (!cs->database) {
      free(db_part);
      err_setf(err, "Out of memory");
      return false;
    }
  }

  free(db_part);
  return true;
}

ConnString *connstr_parse(const char *str, char **err) {
  if (!str || *str == '\0') {
    err_setf(err, "Connection string is empty");
    return NULL;
  }

  /* Prevent processing extremely long connection strings */
  size_t len = strlen(str);
  if (len > MAX_CONNSTR_LEN) {
    err_setf(err, "Connection string too long (max %d characters)",
              MAX_CONNSTR_LEN);
    return NULL;
  }

  ConnString *cs = safe_calloc(1, sizeof(ConnString));

  cs->raw = str_dup(str);

  const char *p = str;

  /* Parse driver (scheme) */
  const char *scheme_end = strstr(p, "://");
  if (!scheme_end) {
    err_setf(err, "Missing '://' in connection string");
    connstr_free(cs);
    return NULL;
  }

  cs->driver = str_ndup(p, scheme_end - p);
  if (!cs->driver) {
    err_setf(err, "Out of memory");
    connstr_free(cs);
    return NULL;
  }
  str_lower(cs->driver);

  p = scheme_end + 3;

  /* Special handling for sqlite - path can start immediately */
  if (str_eq(cs->driver, "sqlite")) {
    /* For sqlite, everything after :// is the path */
    /* Handle sqlite:///absolute/path and sqlite://./relative/path */
    if (*p == '\0') {
      err_setf(err, "SQLite connection string missing database path");
      connstr_free(cs);
      return NULL;
    }

    /* Find query string if any */
    const char *query = strchr(p, '?');
    if (query) {
      cs->database = str_ndup(p, query - p);
      p = query + 1;
    } else {
      cs->database = str_dup(p);
      p = p + strlen(p);
    }

    if (!cs->database) {
      err_setf(err, "Out of memory");
      connstr_free(cs);
      return NULL;
    }
  } else {
    /* Parse [user[:password]@] */
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');

    /* Only consider @ if it comes before / */
    if (at && (!slash || at < slash)) {
      /* Has user info */
      const char *colon = strchr(p, ':');
      if (colon && colon < at) {
        /* Has password */
        cs->user = decode_component(p, colon - p);
        cs->password = decode_component(colon + 1, at - colon - 1);
        if (!cs->user || !cs->password) {
          err_setf(err, "Out of memory");
          connstr_free(cs);
          return NULL;
        }
      } else {
        cs->user = decode_component(p, at - p);
        if (!cs->user) {
          err_setf(err, "Out of memory");
          connstr_free(cs);
          return NULL;
        }
      }
      p = at + 1;
    }

    /* Parse host[:port] */
    slash = strchr(p, '/');
    const char *host_end = slash ? slash : (p + strlen(p));
    const char *query_in_host = strchr(p, '?');
    if (query_in_host && query_in_host < host_end) {
      host_end = query_in_host;
    }

    if (host_end > p) {
      /* Look for port - find last colon (to handle IPv6) */
      const char *port_start = NULL;
      const char *bracket = strchr(p, '[');

      if (bracket && bracket < host_end) {
        /* IPv6 address in brackets */
        const char *bracket_end = strchr(bracket, ']');
        if (bracket_end && bracket_end < host_end) {
          cs->host = str_ndup(bracket + 1, bracket_end - bracket - 1);
          if (!cs->host) {
            err_setf(err, "Out of memory");
            connstr_free(cs);
            return NULL;
          }
          if (bracket_end + 1 < host_end && bracket_end[1] == ':') {
            port_start = bracket_end + 2;
          }
        }
      } else {
        /* Regular host:port */
        const char *colon = NULL;
        for (const char *c = host_end - 1; c >= p; c--) {
          if (*c == ':') {
            colon = c;
            break;
          }
        }
        if (colon) {
          cs->host = str_ndup(p, colon - p);
          port_start = colon + 1;
        } else {
          cs->host = str_ndup(p, host_end - p);
        }
        if (!cs->host) {
          err_setf(err, "Out of memory");
          connstr_free(cs);
          return NULL;
        }
      }

      if (port_start && port_start < host_end) {
        char *port_str = str_ndup(port_start, host_end - port_start);
        if (port_str) {
          char *endptr;
          errno = 0; /* Reset errno before strtol */
          long port_val = strtol(port_str, &endptr, 10);
          /* Validate: no overflow, all chars consumed, in valid range */
          if (errno == 0 && *endptr == '\0' && port_val > 0 &&
              port_val <= 65535) {
            cs->port = (int)port_val;
          } else {
            cs->port = 0; /* Invalid port, use default */
          }
          free(port_str);
        }
      }
    }

    p = host_end;

    /* Parse /database */
    if (*p == '/') {
      p++;
      const char *db_end = strchr(p, '?');
      if (db_end) {
        cs->database = decode_component(p, db_end - p);
        p = db_end;
      } else {
        cs->database = str_url_decode(p);
        p = p + strlen(p);
      }
      if (!cs->database) {
        err_setf(err, "Out of memory");
        connstr_free(cs);
        return NULL;
      }
    }
  }

  /* Parse ?options */
  if (*p == '?') {
    p++;

    /* Count options */
    size_t count = 1;
    for (const char *c = p; *c; c++) {
      if (*c == '&')
        count++;
    }

    cs->option_keys = safe_calloc(count, sizeof(char *));
    cs->option_values = safe_calloc(count, sizeof(char *));

    size_t i = 0;
    while (*p && i < count) {
      const char *amp = strchr(p, '&');
      const char *end = amp ? amp : (p + strlen(p));

      const char *eq = strchr(p, '=');
      if (eq && eq < end) {
        cs->option_keys[i] = decode_component(p, eq - p);
        cs->option_values[i] = decode_component(eq + 1, end - eq - 1);
      } else {
        cs->option_keys[i] = decode_component(p, end - p);
        cs->option_values[i] = str_dup("");
      }

      /* Check for allocation failure */
      if (!cs->option_keys[i] || !cs->option_values[i]) {
        cs->num_options = i + 1; /* Include partial for cleanup */
        err_setf(err, "Out of memory");
        connstr_free(cs);
        return NULL;
      }

      i++;
      p = amp ? (amp + 1) : end;
    }
    cs->num_options = i;
  }

  /* Post-process SSH connection strings (+ssh in driver) */
  if (!connstr_postprocess_ssh(cs, err)) {
    connstr_free(cs);
    return NULL;
  }

  return cs;
}

void connstr_free(ConnString *cs) {
  if (!cs)
    return;

  free(cs->driver);
  free(cs->user);
  str_secure_free(cs->password); /* Securely clear password from memory */
  free(cs->host);
  free(cs->database);
  free(cs->schema);

  /* SSH tunnel fields */
  free(cs->ssh_user);
  str_secure_free(cs->ssh_password);
  free(cs->ssh_host);

  str_secure_free(cs->raw); /* Raw string may contain password */

  FREE_STRING_ARRAY(cs->option_keys, cs->num_options);
  FREE_STRING_ARRAY(cs->option_values, cs->num_options);

  free(cs);
}

const char *connstr_get_option(const ConnString *cs, const char *key) {
  if (!cs || !key)
    return NULL;

  for (size_t i = 0; i < cs->num_options; i++) {
    if (str_eq_nocase(cs->option_keys[i], key)) {
      return cs->option_values[i];
    }
  }
  return NULL;
}

const char *connstr_get_option_default(const ConnString *cs, const char *key,
                                       const char *default_val) {
  const char *val = connstr_get_option(cs, key);
  return val ? val : default_val;
}

bool connstr_has_option(const ConnString *cs, const char *key) {
  return connstr_get_option(cs, key) != NULL;
}

int connstr_get_port(const ConnString *cs) {
  if (!cs)
    return 0;

  if (cs->port > 0)
    return cs->port;

  /* Return default port for driver */
  if (str_eq(cs->driver, "postgres") || str_eq(cs->driver, "postgresql") ||
      str_eq(cs->driver, "pg")) {
    return CONNSTR_PORT_POSTGRES;
  }
  if (str_eq(cs->driver, "mysql") || str_eq(cs->driver, "mariadb")) {
    return CONNSTR_PORT_MYSQL;
  }

  return 0;
}

char *connstr_build(const char *driver, const char *user, const char *password,
                    const char *host, int port, const char *database,
                    const char **option_keys, const char **option_values,
                    size_t num_options) {
  StringBuilder *sb = sb_new(256);
  if (!sb)
    return NULL;

  sb_append(sb, driver);
  sb_append(sb, "://");

  if (user) {
    char *encoded_user = str_url_encode(user);
    if (!encoded_user) {
      sb_free(sb);
      return NULL;
    }
    sb_append(sb, encoded_user);
    free(encoded_user);

    if (password) {
      sb_append_char(sb, ':');
      char *encoded_pass = str_url_encode(password);
      if (!encoded_pass) {
        sb_free(sb);
        return NULL;
      }
      sb_append(sb, encoded_pass);
      str_secure_free(encoded_pass); /* Encoded password is sensitive */
    }
    sb_append_char(sb, '@');
  }

  if (host) {
    /* Handle IPv6 */
    if (strchr(host, ':')) {
      sb_append_char(sb, '[');
      sb_append(sb, host);
      sb_append_char(sb, ']');
    } else {
      sb_append(sb, host);
    }

    if (port > 0) {
      sb_printf(sb, ":%d", port);
    }
  }

  if (database) {
    sb_append_char(sb, '/');
    if (str_eq(driver, "sqlite")) {
      sb_append(sb, database);
    } else {
      char *encoded_db = str_url_encode(database);
      if (!encoded_db) {
        sb_free(sb);
        return NULL;
      }
      sb_append(sb, encoded_db);
      free(encoded_db);
    }
  }

  if (num_options > 0 && option_keys && option_values) {
    sb_append_char(sb, '?');
    for (size_t i = 0; i < num_options; i++) {
      if (i > 0)
        sb_append_char(sb, '&');
      char *encoded_key = str_url_encode(option_keys[i]);
      char *encoded_val = str_url_encode(option_values[i]);
      if (!encoded_key || !encoded_val) {
        free(encoded_key);
        free(encoded_val);
        sb_free(sb);
        return NULL;
      }
      sb_printf(sb, "%s=%s", encoded_key, encoded_val);
      free(encoded_key);
      free(encoded_val);
    }
  }

  return sb_to_string(sb);
}

bool connstr_validate(const ConnString *cs, char **err) {
  if (!cs) {
    err_setf(err, "Connection string is NULL");
    return false;
  }

  if (!cs->driver || *cs->driver == '\0') {
    err_setf(err, "Driver not specified");
    return false;
  }

  /* Check known drivers */
  bool known_driver =
      str_eq(cs->driver, "sqlite") || str_eq(cs->driver, "postgres") ||
      str_eq(cs->driver, "postgresql") || str_eq(cs->driver, "pg") ||
      str_eq(cs->driver, "mysql") || str_eq(cs->driver, "mariadb");

  if (!known_driver) {
    err_setf(err, "Unknown driver: %s", cs->driver);
    return false;
  }

  /* SQLite validation */
  if (str_eq(cs->driver, "sqlite")) {
    if (!cs->database || *cs->database == '\0') {
      err_setf(err, "SQLite requires a database path");
      return false;
    }
    return true;
  }

  /* SSH tunnel validation */
  if (cs->ssh) {
    if (!cs->ssh_host || *cs->ssh_host == '\0') {
      err_setf(err, "SSH tunnel requires a host");
      return false;
    }
    /* ssh_user is optional -- ssh reads from ~/.ssh/config */
  }

  /* Network database validation */
  if (!cs->host || *cs->host == '\0') {
    err_setf(err, "Host is required for %s", cs->driver);
    return false;
  }

  if (!cs->database || *cs->database == '\0') {
    err_setf(err, "Database name is required for %s", cs->driver);
    return false;
  }

  return true;
}

/* SQLite file header magic string (first 16 bytes) */
static const char SQLITE_MAGIC[] = "SQLite format 3";

bool connstr_is_sqlite_file(const char *path) {
  if (!path)
    return false;

  FILE *f = fopen(path, "rb");
  if (!f)
    return false;

  char header[16];
  size_t read = fread(header, 1, sizeof(header), f);
  fclose(f);

  if (read < sizeof(header))
    return false;

  /* Compare with SQLite magic (first 15 chars + null terminator) */
  return memcmp(header, SQLITE_MAGIC, 16) == 0;
}

char *connstr_from_path(const char *path, char **err) {
  if (!path || *path == '\0') {
    err_setf(err, "Empty file path");
    return NULL;
  }

  /* Check if file exists */
  struct stat st;
  if (stat(path, &st) != 0) {
    err_setf(err, "File not found: %s", path);
    return NULL;
  }

  /* Check it's a regular file */
  if (!S_ISREG(st.st_mode)) {
    err_setf(err, "Not a file: %s", path);
    return NULL;
  }

  /* Validate it's a SQLite database */
  if (!connstr_is_sqlite_file(path)) {
    err_setf(err, "Not a SQLite database: %s", path);
    return NULL;
  }

  /* Resolve to absolute path */
  char abs_path[PATH_MAX];
  if (!realpath(path, abs_path)) {
    /* realpath failed, use original path */
    strncpy(abs_path, path, PATH_MAX - 1);
    abs_path[PATH_MAX - 1] = '\0';
  }

  /* Build sqlite:// connection string */
  return str_printf("sqlite://%s", abs_path);
}

char *connstr_mask_password(const char *connstr) {
  if (!connstr)
    return NULL;

  /* Parse to get components */
  ConnString *cs = connstr_parse(connstr, NULL);
  if (!cs) {
    /* Can't parse - return copy of original */
    return str_dup(connstr);
  }

  if (cs->ssh) {
    /* Build: driver+ssh://[ssh_user@]ssh_host[:ssh_port]/[db_user@]db_host[:db_port]/db_name */
    StringBuilder *sb = sb_new(256);
    if (!sb) {
      connstr_free(cs);
      return NULL;
    }
    sb_append(sb, cs->driver);
    sb_append(sb, "+ssh://");
    if (cs->ssh_user) {
      sb_append(sb, cs->ssh_user);
      sb_append_char(sb, '@');
    }
    if (cs->ssh_host)
      sb_append(sb, cs->ssh_host);
    if (cs->ssh_port > 0)
      sb_printf(sb, ":%d", cs->ssh_port);
    sb_append_char(sb, '/');
    if (cs->user) {
      sb_append(sb, cs->user);
      sb_append_char(sb, '@');
    }
    if (cs->host)
      sb_append(sb, cs->host);
    if (cs->port > 0)
      sb_printf(sb, ":%d", cs->port);
    sb_append_char(sb, '/');
    if (cs->database)
      sb_append(sb, cs->database);

    connstr_free(cs);
    return sb_to_string(sb);
  }

  /* Rebuild without password */
  char *result = connstr_build(
      cs->driver, cs->user, NULL, /* Remove password from display */
      cs->host, cs->port, cs->database, (const char **)cs->option_keys,
      (const char **)cs->option_values, cs->num_options);

  connstr_free(cs);
  return result;
}
