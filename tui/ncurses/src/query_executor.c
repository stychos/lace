/*
 * Lace
 * Query tab execution and pagination functions
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

/* Extract table name from a simple SELECT query (e.g., SELECT ... FROM table)
 * Returns allocated string or NULL if not a simple single-table query */
/* True if `sql` is a plain single-table SELECT whose rows map 1:1 onto
 * rows of that table, i.e. safe to edit through primary keys. Rejects
 * JOIN, set operations, subqueries, grouping, DISTINCT, aggregates and
 * multi-table FROM lists. Strings, quoted identifiers and comments are
 * skipped, so keywords inside them do not count. On success *from_out points
 * just past the top-level FROM keyword (NULL if there is none). */
static bool query_is_simple_select(const char *sql, SqlDialect dialect,
                                   const char **from_out) {
  static const char *const banned_words[] = {
      "JOIN", "UNION", "INTERSECT", "EXCEPT", "MINUS", "GROUP", "HAVING",
      "DISTINCT", "WINDOW", "OVER", "LATERAL", "WITH", NULL};
  static const char *const aggregates[] = {
      "COUNT", "SUM", "AVG", "MIN", "MAX", "GROUP_CONCAT", "STRING_AGG",
      "ARRAY_AGG", "JSON_AGG", "JSONB_AGG", "JSON_GROUP_ARRAY", "TOTAL", NULL};
  const char *p = sql;
  bool first_word = true;
  bool in_from = false; /* between FROM and WHERE/ORDER/LIMIT/... */
  int depth = 0;
  *from_out = NULL;
  while (*p) {
    char c = *p;
    bool comment;
    size_t skip = query_sql_skip(p, dialect, &comment);
    if (skip) { /* string, quoted identifier or comment */
      p += skip;
      continue;
    }
    if (c == '(') {
      depth++;
      /* "( SELECT" anywhere is a subquery */
      const char *n = p + 1;
      while (*n && isspace((unsigned char)*n)) n++;
      if (strncasecmp(n, "SELECT", 6) == 0 && !isalnum((unsigned char)n[6]))
        return false;
      p++;
      continue;
    }
    if (c == ')') {
      depth--;
      p++;
      continue;
    }
    if (c == ',' && in_from && depth == 0) {
      return false; /* FROM a, b */
    }
    if (c == ';') {
      break;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      const char *w = p;
      while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
      size_t len = (size_t)(p - w);
      if (first_word) {
        if (!(len == 6 && strncasecmp(w, "SELECT", 6) == 0))
          return false;
        first_word = false;
        continue;
      }
      for (int i = 0; banned_words[i]; i++) {
        if (strlen(banned_words[i]) == len && strncasecmp(w, banned_words[i], len) == 0)
          return false;
      }
      /* aggregate call: NAME followed by '(' */
      const char *n = p;
      while (*n && isspace((unsigned char)*n)) n++;
      if (*n == '(') {
        for (int i = 0; aggregates[i]; i++) {
          if (strlen(aggregates[i]) == len && strncasecmp(w, aggregates[i], len) == 0)
            return false;
        }
      }
      if (depth == 0) {
        if (len == 4 && strncasecmp(w, "FROM", 4) == 0) {
          in_from = true;
          if (!*from_out)
            *from_out = p;
        } else if ((len == 5 && (strncasecmp(w, "WHERE", 5) == 0 ||
                                 strncasecmp(w, "ORDER", 5) == 0 ||
                                 strncasecmp(w, "LIMIT", 5) == 0)) ||
                   (len == 6 && strncasecmp(w, "OFFSET", 6) == 0)) {
          in_from = false;
        }
      }
      continue;
    }
    p++;
  }
  return !first_word;
}

/* Parse one SQL identifier at *p (quoted with "", `` or [], or bare) and
 * advance past it. Bare PostgreSQL identifiers fold to lower case like the
 * server does. Returns NULL if there is no identifier. */
static char *query_parse_identifier(const char **p, LaceDriver driver) {
  const char *s = *p;
  StringBuilder *sb = sb_new(32);
  if (!sb)
    return NULL;
  if (*s == '"' || *s == '`' || *s == '[') {
    char close = *s == '[' ? ']' : *s;
    s++;
    for (;;) {
      if (!*s) {
        sb_free(sb);
        return NULL;
      }
      if (*s == close) {
        if (close == ']' || s[1] != close)
          break;
        s++; /* Doubled quote */
      }
      sb_append_char(sb, *s++);
    }
    s++;
  } else {
    while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '$' ||
                  (unsigned char)*s >= 0x80)) {
      char c = *s++;
      if (driver == LACE_DRIVER_POSTGRES && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
      sb_append_char(sb, c);
    }
  }
  if (sb->len == 0) {
    sb_free(sb);
    return NULL;
  }
  *p = s;
  return sb_finish(sb);
}

char *query_extract_table_name(const char *sql, LaceDriver driver) {
  if (!sql)
    return NULL;

  /* Results of anything but a plain single-table SELECT are not editable:
   * a JOIN/UNION/subquery used to be treated as its first FROM table and
   * edits went to the wrong table or rows. */
  const char *from_pos = NULL;
  if (!query_is_simple_select(sql, query_sql_dialect_of(driver), &from_pos) ||
      !from_pos)
    return NULL;

  /* Skip whitespace after FROM */
  while (*from_pos && isspace((unsigned char)*from_pos))
    from_pos++;

  if (!*from_pos)
    return NULL;

  /* Parse "name" or "schema.name"; each part bare or quoted */
  char *parts[2] = {NULL, NULL};
  size_t num_parts = 0;
  const char *q = from_pos;
  for (;;) {
    if (num_parts == 2) {
      free(parts[0]);
      free(parts[1]);
      return NULL; /* db.schema.table: not supported */
    }
    char *part = query_parse_identifier(&q, driver);
    if (!part) {
      free(parts[0]);
      return NULL;
    }
    parts[num_parts++] = part;
    const char *after = q;
    while (*after && isspace((unsigned char)*after))
      after++;
    if (*after != '.')
      break;
    q = after + 1;
    while (*q && isspace((unsigned char)*q))
      q++;
  }

  /* Check if there's a comma (multiple tables = can't edit) */
  while (*q && isspace((unsigned char)*q))
    q++;
  if (*q == ',') {
    free(parts[0]);
    free(parts[1]);
    return NULL;
  }

  char *table = NULL;
  if (driver == LACE_DRIVER_POSTGRES) {
    table = num_parts == 2 ? lace_table_ref(parts[0], parts[1])
                           : lace_table_ref(NULL, parts[0]);
  } else if (num_parts == 1) {
    table = str_dup(parts[0]);
  } else if (driver == LACE_DRIVER_SQLITE && str_eq_nocase(parts[0], "main")) {
    table = str_dup(parts[1]);
  }
  /* Other qualified names (another MySQL database, an attached SQLite
   * database) cannot be addressed by a table reference: not editable */
  free(parts[0]);
  free(parts[1]);
  return table;
}


/* Execute a SQL query and store results */
/* True if the most recent query_execute() was cancelled by the user.
 * Multi-statement runs must stop (and roll back) instead of continuing. */
static bool g_last_execute_cancelled = false;

bool query_last_execute_cancelled(void) {
  return g_last_execute_cancelled;
}

static void query_execute_impl(TuiState *state, const char *sql);

void query_execute(TuiState *state, const char *sql) {
  query_execute_impl(state, sql);

  /* Record successful user statements in the connection's history */
  Tab *tab = TUI_TAB(state);
  if (!g_last_execute_cancelled && tab && !tab->query_error && TUI_CONN(state)) {
    app_record_history(state->app, TUI_CONN(state), sql, history_detect_type(sql));
  }
}

/* SQL for one page of a paginated query tab */
char *query_page_sql(const Tab *tab, size_t limit, size_t offset) {
  /* The newline keeps a trailing "-- comment" from swallowing the clause */
  if (tab->query_page_wrap) {
    return str_printf("SELECT * FROM (\n%s\n) AS lace_page LIMIT %zu OFFSET %zu",
                      tab->query_base_sql, limit, offset);
  }
  return str_printf("%s\nLIMIT %zu OFFSET %zu", tab->query_base_sql, limit,
                    offset);
}

/* Run a row query on a worker, behind the processing dialog (spinner, Esc
 * cancels). Returns the rows, or NULL with *err set / *cancelled true. */
ResultSet *query_run_modal(TuiState *state, const char *sql,
                                  const char *message, char **err,
                                  bool *cancelled) {
  *err = NULL;
  *cancelled = false;
  AsyncOperation op;
  async_init(&op);
  op.op_type = ASYNC_OP_QUERY;
  op.conn = TUI_CONN(state);
  op.sql = str_dup(sql);
  ResultSet *rs = NULL;
  if (op.sql && async_start(&op)) {
    bool completed = tui_show_processing_dialog(state, &op, message);
    if (completed && op.state == ASYNC_STATE_COMPLETED) {
      rs = (ResultSet *)op.result;
      op.result = NULL;
    } else if (op.state == ASYNC_STATE_ERROR) {
      *err = op.error ? str_dup(op.error) : str_dup("Query failed");
    } else if (op.state == ASYNC_STATE_CANCELLED) {
      *cancelled = true;
    }
  }
  async_free(&op);
  return rs;
}

/* Count the rows of the tab's query in the background. Best effort: the
 * first foreground statement on the connection cancels it (the daemon runs
 * one statement per connection), and paging finds the end anyway. */
static void query_count_start(TuiState *state, Tab *tab) {
  query_count_stop(tab);
  /* Never inside an open transaction: a statement cancelled there (the
   * count is preempted by the next foreground one) would abort it on
   * PostgreSQL */
  if (!TUI_CONN(state) || TUI_CONN(state)->background_op || TUI_CONN(state)->in_transaction) {
    return;
  }
  AsyncOperation *op = safe_calloc(1, sizeof(AsyncOperation));
  async_init(op);
  op->op_type = ASYNC_OP_QUERY;
  op->conn = TUI_CONN(state);
  op->sql = str_printf("SELECT COUNT(*) FROM (\n%s\n) AS lace_count",
                       tab->query_base_sql);
  if (!op->sql || !async_start(op)) {
    async_free(op);
    free(op);
    return;
  }
  tab->query_count_op = op;
  TUI_CONN(state)->background_op = op;
}

void query_poll_count(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !tab->query_count_op) {
    return;
  }
  AsyncOperation *op = (AsyncOperation *)tab->query_count_op;
  AsyncState st = async_poll(op);
  if (st == ASYNC_STATE_RUNNING) {
    return;
  }
  if (st == ASYNC_STATE_COMPLETED && op->result && !tab->query_total_known) {
    ResultSet *r = (ResultSet *)op->result;
    if (r->num_rows > 0 && r->rows && r->rows[0].num_cells > 0 &&
        r->rows[0].cells[0].type == DB_TYPE_INT && r->rows[0].cells[0].int_val >= 0) {
      tab->query_total_rows = (size_t)r->rows[0].cells[0].int_val;
      tab->query_total_known = true;
      tui_set_status(state, "%zu rows", tab->query_total_rows);
    }
  }
  query_count_stop(tab); /* Frees op and its result */
}

/* True on a PostgreSQL connection with a transaction open, where a failed
 * or cancelled statement aborts the whole transaction */
static bool pg_in_transaction(const DbConnection *conn) {
  return conn && conn->driver_type == LACE_DRIVER_POSTGRES &&
         conn->in_transaction;
}

ResultSet *query_run_derived(TuiState *state, const char *sql,
                             const char *message, char **err,
                             bool *cancelled) {
  DbConnection *pc = TUI_CONN(state);
  bool savepoint = false;
  if (pg_in_transaction(pc)) {
    *cancelled = false;
    if (pc->transaction_failed) {
      *err = str_dup("The transaction has failed: only ROLLBACK works");
      return NULL;
    }
    if (db_exec(pc, "SAVEPOINT lace_page", err) < 0) {
      return NULL;
    }
    savepoint = true;
  }
  ResultSet *rs = query_run_modal(state, sql, message, err, cancelled);
  if (savepoint) {
    /* db_exec also records the transaction state: live again after a
     * rollback to the savepoint */
    char *sp_err = NULL;
    db_exec(pc,
            rs ? "RELEASE SAVEPOINT lace_page"
               : "ROLLBACK TO SAVEPOINT lace_page",
            &sp_err);
    free(sp_err);
  }
  return rs;
}

bool query_ensure_total(TuiState *state, Tab *tab) {
  if (!tab || !tab->query_paginated || tab->query_total_known) {
    return tab && tab->query_total_known;
  }
  query_count_stop(tab); /* The modal count replaces the background one */
  char *sql = str_printf("SELECT COUNT(*) FROM (\n%s\n) AS lace_count",
                         tab->query_base_sql);
  char *err = NULL;
  bool cancelled = false;
  ResultSet *r = query_run_derived(state, sql, "Counting rows...", &err, &cancelled);
  free(sql);
  if (r && r->num_rows > 0 && r->rows && r->rows[0].num_cells > 0 &&
      r->rows[0].cells[0].type == DB_TYPE_INT && r->rows[0].cells[0].int_val >= 0) {
    tab->query_total_rows = (size_t)r->rows[0].cells[0].int_val;
    tab->query_total_known = true;
  } else if (err && !cancelled) {
    tui_set_error(state, "Count failed: %s", err);
  }
  free(err);
  db_result_free(r);
  return tab->query_total_known;
}

/* "1000+ rows" until the total is known */
void query_status_loaded(TuiState *state, Tab *tab) {
  size_t end = tab->query_loaded_offset + tab->query_loaded_count;
  if (tab->query_total_known) {
    tui_set_status(state, "Loaded %zu/%zu rows", end, tab->query_total_rows);
  } else {
    tui_set_status(state, "Loaded %zu+ rows (counting...)", end);
  }
}

static void query_execute_impl(TuiState *state, const char *sql) {
  g_last_execute_cancelled = false;
  if (!state || !sql || !*sql)
    return;

  Tab *tab = TUI_TAB(state);
  UITabState *ui = TUI_TAB_UI(state);
  if (!tab || tab->type != TAB_TYPE_QUERY || !ui)
    return;

  /* Free previous results */
  if (tab->query_results) {
    db_result_free(tab->query_results);
    tab->query_results = NULL;
  }
  free(tab->query_error);
  tab->query_error = NULL;
  tab->query_affected = 0;
  tab->query_exec_success = false;
  free(tab->query_result_col_widths);
  tab->query_result_col_widths = NULL;
  tab->query_result_num_cols = 0;
  free(tab->query_source_table);
  tab->query_source_table = NULL;
  db_schema_free(tab->query_source_schema);
  tab->query_source_schema = NULL;
  query_count_stop(tab);
  free(tab->query_base_sql);
  tab->query_base_sql = NULL;
  tab->query_page_wrap = false;
  tab->query_total_known = false;
  tab->query_total_rows = 0;
  tab->query_loaded_offset = 0;
  tab->query_loaded_count = 0;
  tab->query_paginated = false;

  /* Clear any row selections */
  tab_clear_selections(tab);

  /* Reset result cursor */
  tab->query_result_row = 0;
  tab->query_result_col = 0;
  tab->query_result_scroll_row = 0;
  tab->query_result_scroll_col = 0;

  if (!TUI_CONN(state)) {
    tab->query_error = str_dup("Not connected to database");
    return;
  }

  /* Detect query type (simple heuristic) */
  const char *p = sql;
  while (*p && isspace((unsigned char)*p))
    p++;

  /* Statements that return rows (incl. WITH ... SELECT, VALUES, a SELECT
   * after a comment) go through db_query; the rest through db_exec. */
  (void)p;
  char *err = NULL;

  /* Every statement goes to the daemon the same way; what it produced
   * (rows, or a row count) decides how it is shown. Only reads that can
   * take LIMIT are paged. */
  {
    /* Page every statement that can take a LIMIT, so only a window of rows
     * is held (PRAGMA/SHOW/EXPLAIN cannot, and are small). A statement with
     * its own top-level LIMIT is wrapped as a subquery; otherwise LIMIT is
     * appended, which keeps its ORDER BY (MySQL/MariaDB ignore ORDER BY in a
     * derived table without LIMIT). */
    QuerySqlShape shape;
    query_sql_shape(sql, db_conn_driver_name(TUI_CONN(state)), &shape);
    /* Only reads are paged: "WITH ... INSERT/UPDATE/DELETE" or
     * "SELECT ... INTO" run once, as written (paging them cut them off at
     * the first page) */
    bool pageable = query_sql_pageable(&shape);
    bool cancelled = false;

    /* A PostgreSQL transaction that already failed runs the statement as
     * written, which reports the real error (query_run_derived guards the
     * paged attempt in a live one) */
    if (pg_in_transaction(TUI_CONN(state)) && TUI_CONN(state)->transaction_failed) {
      pageable = false;
    }

    if (pageable && shape.end > 0) {
      tab->query_base_sql = str_ndup(sql, shape.end);
      tab->query_page_wrap = query_page_needs_wrap(&shape);
      char *page_sql = query_page_sql(tab, TUI_PAGE_SIZE, 0);
      tab->query_results =
          query_run_derived(state, page_sql, "Executing query...", &err, &cancelled);
      free(page_sql);
      if (tab->query_results) {
        tab->query_paginated = true;
        tab->query_loaded_offset = 0;
        tab->query_loaded_count = tab->query_results->num_rows;
        if (tab->query_results->num_rows < TUI_PAGE_SIZE) {
          /* A short first page is everything */
          tab->query_total_rows = tab->query_results->num_rows;
          tab->query_total_known = true;
        }
      } else if (err && !cancelled) {
        /* The paged form may not parse for this dialect/statement: run it
         * as written (the whole result, up to the daemon's limit) */
        free(err);
        err = NULL;
        free(tab->query_base_sql);
        tab->query_base_sql = NULL;
        tab->query_page_wrap = false;
      }
    }
    if (!tab->query_results && !err && !cancelled) {
      tab->query_results =
          query_run_modal(state, sql, "Executing query...", &err, &cancelled);
    }
    if (cancelled) {
      g_last_execute_cancelled = true;
      tui_set_status(state, "Query cancelled");
    }

    if (!err && tab->query_results && tab->query_results->num_columns == 0) {
      /* No result set: a write, DDL, ... (a statement with RETURNING has
       * columns and shows its rows) */
      tab->query_affected = tab->query_results->rows_affected;
      tab->query_exec_success = true;
      db_result_free(tab->query_results);
      tab->query_results = NULL;
      tab->query_paginated = false;
      free(tab->query_base_sql);
      tab->query_base_sql = NULL;
      tui_set_status(state, "%lld rows affected", (long long)tab->query_affected);
    }

    if (err) {
      tab->query_error = err;
    } else if (tab->query_results) {
      query_calculate_result_widths(tab);
      /* Try to extract table name for editing support */
      tab->query_source_table =
          query_extract_table_name(sql, TUI_CONN(state) ? TUI_CONN(state)->driver_type
                                                    : LACE_DRIVER_SQLITE);
      /* Load table schema for primary key info (needed for SQLite/PostgreSQL)
       */
      if (tab->query_source_table) {
        char *schema_err = NULL;
        tab->query_source_schema = db_get_table_schema(
            TUI_CONN(state), tab->query_source_table, &schema_err);
        free(schema_err); /* Ignore schema errors */
      }
      if (tab->query_paginated) {
        if (!tab->query_total_known) {
          query_count_start(state, tab);
        }
        query_status_loaded(state, tab);
      } else {
        tui_set_status(state, tab->query_results->truncated
                                  ? "%zu rows returned (truncated: result too large)"
                                  : "%zu rows returned",
                       tab->query_results->num_rows);
      }
      /* History is recorded automatically by database layer */
    }
  }

  /* Focus results pane after execution (only if there are results) */
  if (!tab->query_error && tab->query_results &&
      tab->query_results->num_rows > 0) {
    ui->query_focus_results = true;
    ui->query_unfocused = false;
  }
}
