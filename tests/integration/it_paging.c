/*
 * Lace
 * Integration tests: the query tab's paged and counted forms of a user's
 * statement (built by the TUI's own functions) return exactly the rows of
 * the statement as written, on every database
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include "query_internal.h"
#include <stdlib.h>
#include <string.h>

#define PAGE 7

/* Every row of r as one line of its cells */
static char *row_text(const LaceResult *r, size_t row) {
  StringBuilder *sb = sb_new(64);
  for (size_t c = 0; c < r->num_columns; c++) {
    char *t = it_text(r, row, c);
    sb_printf(sb, "%s|", t ? t : "NULL");
    free(t);
  }
  return sb_finish(sb);
}

/* Page sql the way the query tab does, and compare the pages joined, and
 * the count, with the statement run as written */
static void check_paging(ItDb *db, const char *sql) {
  QuerySqlShape shape;
  query_sql_shape(sql, it_driver_name(db), &shape);
  CHECK(query_sql_pageable(&shape));
  if (!query_sql_pageable(&shape) || shape.end == 0)
    return;

  Tab tab;
  memset(&tab, 0, sizeof(tab));
  tab.query_base_sql = str_ndup(sql, shape.end);
  tab.query_page_wrap = query_page_needs_wrap(&shape);

  LaceResult *full = it_q(db, "%s", sql);
  size_t n = full ? full->num_rows : 0;
  size_t got = 0;
  bool same = full != NULL;
  for (size_t offset = 0; same; offset += PAGE) {
    char *page_sql = query_page_sql(&tab, PAGE, offset);
    LaceResult *page = it_q(db, "%s", page_sql);
    free(page_sql);
    if (!page) {
      same = false;
      break;
    }
    for (size_t i = 0; i < page->num_rows; i++, got++) {
      char *g = row_text(page, i);
      char *w = got < n ? row_text(full, got) : NULL;
      if (!w || strcmp(g, w) != 0) {
        fprintf(stderr, "  [%s] row %zu of \"%s\": %s, want %s\n", db->name,
                got, sql, g, w ? w : "(none)");
        same = false;
      }
      free(g);
      free(w);
    }
    size_t rows = page->num_rows;
    lace_result_free(page);
    if (rows < PAGE)
      break;
  }
  CHECK(same && got == n);

  /* The background count's form */
  int64_t count = it_scalar(db, "SELECT COUNT(*) FROM (\n%s\n) AS lace_count",
                            tab.query_base_sql);
  CHECK(count == (int64_t)n);
  lace_result_free(full);
  free(tab.query_base_sql);
}

static bool fill(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_qp");
  if (!it_ex(db, "CREATE TABLE it_qp (id INTEGER PRIMARY KEY, v INTEGER, s VARCHAR(10))"))
    return false;
  StringBuilder *sb = sb_new(2048);
  sb_append(sb, "INSERT INTO it_qp VALUES ");
  for (int i = 1; i <= 60; i++)
    sb_printf(sb, "%s(%d, %d, %s)", i > 1 ? ", " : "", i, (i * 7) % 11,
              i % 9 == 0 ? "NULL" : "'x'");
  char *q = sb_finish(sb);
  bool ok = it_ex(db, "%s", q);
  free(q);
  return ok;
}

static void test_paging_forms(ItDb *db) {
  if (!fill(db))
    return;
  check_paging(db, "SELECT id, v FROM it_qp ORDER BY v DESC, id");
  /* Its own LIMIT: wrapped in a derived table */
  check_paging(db, "SELECT id, v FROM it_qp WHERE v > 3 ORDER BY v, id LIMIT 25");
  check_paging(db, "SELECT id FROM it_qp ORDER BY id LIMIT 30 OFFSET 5");
  /* Trailing ';' and comments are not part of the paged statement */
  check_paging(db, "SELECT id, s FROM it_qp ORDER BY id; -- the end");
  check_paging(db, "SELECT id FROM it_qp ORDER BY id /* block */ ;");
  check_paging(db, "WITH t AS (SELECT id, v FROM it_qp WHERE s IS NOT NULL) "
                   "SELECT * FROM t ORDER BY id");
  check_paging(db, "SELECT id FROM it_qp WHERE v < 3 UNION ALL "
                   "SELECT id FROM it_qp WHERE v > 8 ORDER BY 1");
  check_paging(db, "SELECT v, COUNT(*) AS n FROM it_qp GROUP BY v ORDER BY v");
  /* A string that looks like a clause is just data */
  check_paging(db, "SELECT id, 'LIMIT 1; -- not a comment' AS t FROM it_qp ORDER BY id");
  if (db->driver != LACE_DRIVER_MYSQL && db->driver != LACE_DRIVER_MARIADB)
    check_paging(db, "VALUES (1, 'a'), (2, 'b'), (3, 'c')");
  it_ex(db, "DROP TABLE it_qp");
}

/* Writes and SELECT ... INTO run once, as written: never paged */
static void test_not_pageable(ItDb *db) {
  const char *writes[] = {
      "INSERT INTO it_qp SELECT * FROM it_qp",
      "WITH x AS (SELECT 1) DELETE FROM it_qp",
      "UPDATE it_qp SET v = 1",
      "SELECT id INTO it_qp_copy FROM it_qp",
      NULL,
  };
  for (int i = 0; writes[i]; i++) {
    QuerySqlShape shape;
    query_sql_shape(writes[i], it_driver_name(db), &shape);
    if (query_sql_pageable(&shape))
      fprintf(stderr, "  [%s] pageable: %s\n", db->name, writes[i]);
    CHECK(!query_sql_pageable(&shape));
  }
}

const ItTest it_paging_tests[] = {
    {"paging_forms", test_paging_forms, IT_ALL, false},
    {"not_pageable", test_not_pageable, IT_SQLITE | IT_SOCKET_ONLY, false},
    {NULL, NULL, 0, false},
};
