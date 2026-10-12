/*
 * Lace
 * Query tab: running the editor's text (Ctrl+R / Ctrl+A / Ctrl+T)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_internal.h"
#include "../../liblace/include/util/mem.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================================================================
 * Running the editor's text (Ctrl+R / Ctrl+A / Ctrl+T)
 * ============================================================================
 */

/* Run each statement of text in turn (split like query_find_at_cursor, so
 * quotes, comments and $tag$ bodies never split one). Fragments holding only
 * comments are skipped (MySQL rejects them as "Query was empty"). Stops at a
 * cancel, and at the first error when stop_on_error. Returns the number run;
 * *errors and *cancelled say how it went. */
static int run_statements(TuiState *state, Tab *tab, const char *text,
                          bool stop_on_error, int *errors, bool *cancelled) {
  const char *driver = db_conn_driver_name(TUI_CONN(state));
  int count = 0;
  *errors = 0;
  *cancelled = false;
  const char *p = text;
  while (*p) {
    size_t len = query_statement_length(p, driver);
    char *stmt = str_ndup(p, len);
    QuerySqlShape shape;
    query_sql_shape(stmt, driver, &shape);
    if (stmt && shape.end > 0) {
      stmt[shape.end] = '\0'; /* Without trailing space and comments */
      const char *q = stmt;
      while (isspace((unsigned char)*q))
        q++;
      query_execute(state, q);
      if (query_last_execute_cancelled()) {
        *cancelled = true; /* Never run (or commit) past a cancel */
        free(stmt);
        break;
      }
      count++;
      if (tab->query_error) {
        (*errors)++;
        if (stop_on_error) {
          free(stmt);
          break;
        }
      }
    }
    free(stmt);
    p += len;
    if (*p == ';')
      p++;
  }
  return count;
}

void query_run_script(TuiState *state, QueryRunMode mode) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_QUERY)
    return;

  if (mode == QUERY_RUN_AT_CURSOR) {
    char *query = query_find_at_cursor(tab->query_text, tab->query_cursor,
                                       db_conn_driver_name(TUI_CONN(state)));
    if (query && *query) {
      query_execute(state, query);
    } else {
      tui_set_error(state, "No query at cursor");
    }
    free(query);
    return;
  }

  if (!tab->query_text || !*tab->query_text) {
    tui_set_error(state, "No queries to execute");
    return;
  }
  /* query_execute replaces tab state: run from a copy of the text */
  char *text = str_dup(tab->query_text);
  if (!text)
    return;
  int errors = 0;
  bool cancelled = false;

  if (mode == QUERY_RUN_ALL) {
    int count = run_statements(state, tab, text, false, &errors, &cancelled);
    if (cancelled) {
      tui_set_status(state, "Cancelled after %d of the queries", count);
    } else if (errors > 0) {
      tui_set_error(state, "Executed %d queries, %d errors", count, errors);
    } else {
      tui_set_status(state, "Executed %d queries", count);
    }
    free(text);
    return;
  }

  /* QUERY_RUN_TRANSACTION */
  DbConnection *conn = TUI_CONN(state);
  if (!conn) {
    tui_set_error(state, "Not connected to database");
    free(text);
    return;
  }
  /* Inside the user's own transaction, BEGIN would commit it (MySQL) or be
   * ignored (PostgreSQL), and the closing COMMIT or ROLLBACK would end the
   * user's work along with the script's */
  if (conn->in_transaction || conn->transaction_lost) {
    tui_set_error(state, "A transaction is already open: COMMIT or ROLLBACK "
                         "it first, or run the script with Ctrl+A");
    free(text);
    return;
  }
  char *err = NULL;
  db_exec(conn, "BEGIN", &err);
  if (err) {
    tui_set_error(state, "Failed to start transaction: %s", err);
    free(err);
    free(text);
    return;
  }
  int count = run_statements(state, tab, text, true, &errors, &cancelled);
  free(text);
  conn = TUI_CONN(state);
  if (cancelled || errors > 0) {
    db_exec(conn, "ROLLBACK", &err);
    free(err);
    if (cancelled) {
      tui_set_status(state, "Transaction cancelled and rolled back");
    } else {
      tui_set_error(state, "Transaction rolled back after error in query %d",
                    count);
    }
    return;
  }
  db_exec(conn, "COMMIT", &err);
  if (err) {
    tui_set_error(state, "Commit failed: %s", err);
    free(err);
  } else {
    tui_set_status(state, "Transaction committed (%d queries)", count);
  }
}
