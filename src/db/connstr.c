/*
 * lace - Database Viewer and Manager
 * Connection string parser implementation
 */

#include "connstr.h"
#include "../util/str.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Fallback for systems without PATH_MAX */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void set_error(char **err, const char *fmt, ...) {
  if (!err)
    return;

  va_list args;
  va_start(args, fmt);
  *err = str_vprintf(fmt, args);
  va_end(args);
}

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

ConnString *connstr_parse(const char *str, char **err) {
  if (!str || *str == '\0') {
    set_error(err, "Connection string is empty");
    return NULL;
  }

  /* Prevent processing extremely long connection strings */
  size_t len = strlen(str);
  if (len > MAX_CONNSTR_LEN) {
    set_error(err, "Connection string too long (max %d characters)",
              MAX_CONNSTR_LEN);
    return NULL;
  }

  ConnString *cs = calloc(1, sizeof(ConnString));
  if (!cs) {
    set_error(err, "Out of memory");
    return NULL;
  }

  cs->raw = str_dup(str);
  if (!cs->raw) {
    free(cs);
    set_error(err, "Out of memory");
    return NULL;
  }

  const char *p = str;

  /* Parse driver (scheme) */
  const char *scheme_end = strstr(p, "://");
  if (!scheme_end) {
    set_error(err, "Missing '://' in connection string");
    connstr_free(cs);
    return NULL;
  }

  cs->driver = str_ndup(p, scheme_end - p);
  if (!cs->driver) {
    set_error(err, "Out of memory");
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
      set_error(err, "SQLite connection string missing database path");
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
      set_error(err, "Out of memory");
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
          set_error(err, "Out of memory");
          connstr_free(cs);
          return NULL;
        }
      } else {
        cs->user = decode_component(p, at - p);
        if (!cs->user) {
          set_error(err, "Out of memory");
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
      }

      if (port_start && port_start < host_end) {
        char *port_str = str_ndup(port_start, host_end - port_start);
        if (port_str) {
          char *endptr;
          long port_val = strtol(port_str, &endptr, 10);
          /* Validate: all chars consumed, in valid range */
          if (*endptr == '\0' && port_val > 0 && port_val <= 65535) {
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

    cs->option_keys = calloc(count, sizeof(char *));
    cs->option_values = calloc(count, sizeof(char *));
    if (!cs->option_keys || !cs->option_values) {
      set_error(err, "Out of memory");
      connstr_free(cs);
      return NULL;
    }

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
        set_error(err, "Out of memory");
        connstr_free(cs);
        return NULL;
      }

      i++;
      p = amp ? (amp + 1) : end;
    }
    cs->num_options = i;
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
  str_secure_free(cs->raw); /* Raw string may contain password */

  for (size_t i = 0; i < cs->num_options; i++) {
    free(cs->option_keys[i]);
    free(cs->option_values[i]);
  }
  free(cs->option_keys);
  free(cs->option_values);

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
      free(encoded_pass);
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
    set_error(err, "Connection string is NULL");
    return false;
  }

  if (!cs->driver || *cs->driver == '\0') {
    set_error(err, "Driver not specified");
    return false;
  }

  /* Check known drivers */
  bool known_driver =
      str_eq(cs->driver, "sqlite") || str_eq(cs->driver, "postgres") ||
      str_eq(cs->driver, "postgresql") || str_eq(cs->driver, "pg") ||
      str_eq(cs->driver, "mysql") || str_eq(cs->driver, "mariadb");

  if (!known_driver) {
    set_error(err, "Unknown driver: %s", cs->driver);
    return false;
  }

  /* SQLite validation */
  if (str_eq(cs->driver, "sqlite")) {
    if (!cs->database || *cs->database == '\0') {
      set_error(err, "SQLite requires a database path");
      return false;
    }
    return true;
  }

  /* Network database validation */
  if (!cs->host || *cs->host == '\0') {
    set_error(err, "Host is required for %s", cs->driver);
    return false;
  }

  if (!cs->database || *cs->database == '\0') {
    set_error(err, "Database name is required for %s", cs->driver);
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
    set_error(err, "Empty file path");
    return NULL;
  }

  /* Check if file exists */
  struct stat st;
  if (stat(path, &st) != 0) {
    set_error(err, "File not found: %s", path);
    return NULL;
  }

  /* Check it's a regular file */
  if (!S_ISREG(st.st_mode)) {
    set_error(err, "Not a file: %s", path);
    return NULL;
  }

  /* Validate it's a SQLite database */
  if (!connstr_is_sqlite_file(path)) {
    set_error(err, "Not a SQLite database: %s", path);
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
