/*
 * Lace
 * Connection string display (no ncurses)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connstr_display.h"
#include "../../../liblace/include/util/connstr.h"
#include "../../../liblace/include/util/str.h"
#include <stdlib.h>
#include <string.h>

char *connstr_display(const char *connstr) {
  ConnString *cs = connstr ? connstr_parse(connstr, NULL) : NULL;
  if (!cs)
    return NULL;
  char *where;
  if (cs->driver && str_eq(cs->driver, "sqlite")) {
    where = str_printf("sqlite: %s", cs->database ? cs->database : "");
  } else {
    bool v6 = cs->host && strchr(cs->host, ':');
    char *port = cs->port > 0 ? str_printf(":%d", cs->port) : str_dup("");
    where = str_printf("%s://%s%s%s%s%s%s/%s", cs->driver ? cs->driver : "",
                       cs->user && cs->user[0] ? cs->user : "",
                       cs->user && cs->user[0] ? "@" : "", v6 ? "[" : "",
                       cs->host && cs->host[0] ? cs->host : "localhost",
                       v6 ? "]" : "", port, cs->database ? cs->database : "");
    free(port);
    if (cs->ssh) {
      char *via = str_printf("%s  via ssh %s%s%s", where,
                             cs->ssh_user && cs->ssh_user[0] ? cs->ssh_user : "",
                             cs->ssh_user && cs->ssh_user[0] ? "@" : "",
                             cs->ssh_host ? cs->ssh_host : "");
      free(where);
      where = via;
    }
  }
  connstr_free(cs);
  return where;
}
