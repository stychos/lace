/*
 * Lace
 * Common database driver utilities - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "db_common.h"
#include "../../../liblace/include/lace.h"
#include "util/mem.h"
#include "db.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void db_common_free_connection(DbConnection *conn) {
  if (!conn)
    return;
  str_secure_free(conn->connstr);
  free(conn->database);
  free(conn->host);
  free(conn->user);
  free(conn);
}

bool db_common_sql_starts_with_word(const char *sql, const char *word) {
  const char *p = sql;
  for (;;) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (p[0] == '-' && p[1] == '-') {
      while (*p && *p != '\n')
        p++;
    } else if (p[0] == '/' && p[1] == '*') {
      const char *e = strstr(p + 2, "*/");
      p = e ? e + 2 : p + strlen(p);
    } else {
      break;
    }
  }
  size_t n = strlen(word);
  return strncasecmp(p, word, n) == 0 && !isalnum((unsigned char)p[n]) &&
         p[n] != '_';
}

void db_rows_init(DbRowCollector *rc, ResultSet *rs) {
  rc->rs = rs;
  rc->bytes = 0;
  rc->cap = 0;
  free(rs->rows);
  rs->rows = NULL;
  rs->num_rows = 0;
}

DbValue *db_rows_add(DbRowCollector *rc, size_t num_cells) {
  ResultSet *rs = rc->rs;
  if (rs->num_rows >= (size_t)LACE_MAX_RESULT_ROWS || rc->bytes >= LACE_MAX_RESULT_BYTES) {
    rs->truncated = true;
    return NULL;
  }
  if (rs->num_rows == rc->cap) {
    size_t cap = rc->cap ? rc->cap * 2 : 64;
    rs->rows = safe_reallocarray(rs->rows, cap, sizeof(Row));
    memset(rs->rows + rc->cap, 0, (cap - rc->cap) * sizeof(Row));
    rc->cap = cap;
  }
  Row *row = &rs->rows[rs->num_rows++];
  row->num_cells = num_cells;
  row->cells = safe_calloc(num_cells ? num_cells : 1, sizeof(DbValue));
  return row->cells;
}

void db_link_lost(DbLinkState *link, bool tx_open) {
  link->broken = true;
  if (tx_open)
    link->tx_lost = true;
}

DbLinkAction db_link_gate(DbLinkState *link, const char *sql,
                          const char *lost_msg, char **err) {
  if (link->tx_lost) {
    /* Exactly a ROLLBACK: not ROLLBACK TO SAVEPOINT (the TUI's own, after
     * a failed read), nor "ROLLBACK; more" (the rest would be dropped) */
    if (!lace_sql_is_rollback(sql)) {
      err_set(err, lost_msg);
      return DB_LINK_REFUSE;
    }
    return DB_LINK_ACK;
  }
  return link->broken ? DB_LINK_RECONNECT : DB_LINK_READY;
}
