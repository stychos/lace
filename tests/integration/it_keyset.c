/*
 * Lace
 * Integration tests: keyset pagination walks reproduce ORDER BY exactly
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdlib.h>
#include <string.h>

#define WALK_PAGE 7

/* Row identity: the key columns' text joined */
static char *row_key(const LaceResult *r, size_t row, const char *const *keys,
                     size_t nk) {
  StringBuilder *sb = sb_new(64);
  for (size_t k = 0; k < nk; k++) {
    for (size_t j = 0; j < r->num_columns; j++) {
      if (strcmp(r->columns[j].name, keys[k]) == 0) {
        char *t = it_text(r, row, j);
        sb_printf(sb, "%s|", t ? t : "NULL");
        free(t);
      }
    }
  }
  return sb_finish(sb);
}

/* Schema index of a column */
static size_t col_index(const LaceSchema *s, const char *name) {
  for (size_t i = 0; i < s->num_columns; i++) {
    if (strcmp(s->columns[i].name, name) == 0)
      return i;
  }
  return SIZE_MAX;
}

typedef struct {
  const char *column;
  LaceSortDir dir;
} WalkSort;

/* Walk table in pages of WALK_PAGE, forward and backward, and compare the
 * rows met with one ORDER BY query */
static void walk(ItDb *db, const char *table, const char *const *keys,
                 size_t nk, const WalkSort *sorts, size_t ns) {
  LaceSchema *schema = NULL;
  CHECK_OK(db, lace_get_schema(db->client, db->conn, table, &schema));
  if (!schema)
    return;
  LaceSort ls[4];
  for (size_t i = 0; i < ns; i++) {
    ls[i].column = col_index(schema, sorts[i].column);
    ls[i].dir = sorts[i].dir;
  }
  LaceKeysetState *ks = lace_keyset_create(schema, ls, ns, db->driver);
  CHECK(ks != NULL);
  if (!ks) {
    lace_schema_free(schema);
    return;
  }
  char *fwd = lace_keyset_build_order(ks, true);
  char *bwd = lace_keyset_build_order(ks, false);
  char *qt = lace_quote_table(db->driver, table);

  LaceResult *full = it_q(db, "SELECT * FROM %s ORDER BY %s", qt, fwd);
  size_t n = full ? full->num_rows : 0;
  char **want = calloc(n + 1, sizeof(char *));
  for (size_t i = 0; i < n; i++)
    want[i] = row_key(full, i, keys, nk);

  for (int dir = 0; dir < 2; dir++) {
    bool forward = dir == 0;
    size_t cap = n + WALK_PAGE * 2, got_n = 0;
    char **got = calloc(cap, sizeof(char *));
    char *where = NULL;
    bool broken = false;
    for (int pages = 0; pages <= (int)n + 2; pages++) {
      LaceResult *p =
          where ? it_q(db, "SELECT * FROM %s WHERE %s ORDER BY %s LIMIT %d", qt,
                       where, forward ? fwd : bwd, WALK_PAGE)
                : it_q(db, "SELECT * FROM %s ORDER BY %s LIMIT %d", qt,
                       forward ? fwd : bwd, WALK_PAGE);
      free(where);
      where = NULL;
      if (!p) {
        broken = true;
        break;
      }
      for (size_t i = 0; i < p->num_rows && got_n < cap; i++)
        got[got_n++] = row_key(p, i, keys, nk);
      if (p->num_rows < WALK_PAGE) {
        lace_result_free(p);
        break;
      }
      /* The page is in walk order: its last row is the boundary */
      where = lace_keyset_build_where(ks, p, p->num_rows - 1, forward);
      lace_result_free(p);
      if (!where) {
        broken = true;
        break;
      }
    }
    free(where);
    bool same = !broken && got_n == n;
    for (size_t i = 0; same && i < n; i++) {
      const char *g = got[forward ? i : n - 1 - i];
      if (strcmp(g, want[i]) != 0) {
        fprintf(stderr, "  [%s] %s %s: row %zu is %s, want %s\n", db->name,
                table, forward ? "forward" : "backward", i, g, want[i]);
        same = false;
      }
    }
    if (got_n != n)
      fprintf(stderr, "  [%s] %s %s: %zu rows, want %zu\n", db->name, table,
              forward ? "forward" : "backward", got_n, n);
    CHECK(same);
    for (size_t i = 0; i < got_n; i++)
      free(got[i]);
    free(got);
  }
  for (size_t i = 0; i < n; i++)
    free(want[i]);
  free(want);
  lace_result_free(full);
  free(qt);
  free(fwd);
  free(bwd);
  lace_keyset_free(ks);
  lace_schema_free(schema);
}

/* 97 rows with NULLs and many duplicates in the sort columns, and a
 * composite-key table */
static bool fill(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_kw1");
  it_ex(db, "DROP TABLE IF EXISTS it_kw2");
  if (!it_ex(db, "CREATE TABLE it_kw1 (id INTEGER PRIMARY KEY, grp INTEGER, "
                 "name VARCHAR(20), score INTEGER NOT NULL)") ||
      !it_ex(db, "CREATE TABLE it_kw2 (a INTEGER NOT NULL, b VARCHAR(10) NOT "
                 "NULL, v INTEGER, PRIMARY KEY (a, b))"))
    return false;
  StringBuilder *k1 = sb_new(4096), *k2 = sb_new(4096);
  sb_append(k1, "INSERT INTO it_kw1 VALUES ");
  sb_append(k2, "INSERT INTO it_kw2 VALUES ");
  unsigned s = 12345;
  for (int i = 1; i <= 97; i++) {
    s = s * 1103515245u + 12345u;
    int g = (int)((s >> 16) % 6);
    s = s * 1103515245u + 12345u;
    int nm = (int)((s >> 16) % 9);
    char name[16];
    if (nm == 8)
      strcpy(name, "NULL");
    else if (nm == 7)
      strcpy(name, "'it''s'");
    else
      snprintf(name, sizeof(name), "'N%c'", 'a' + nm);
    char grp[8];
    if (g == 5)
      strcpy(grp, "NULL");
    else
      snprintf(grp, sizeof(grp), "%d", g);
    sb_printf(k1, "%s(%d, %s, %s, %d)", i > 1 ? ", " : "", i, grp, name,
              (int)((s >> 8) % 4));
    sb_printf(k2, "%s(%d, '%c%d', %s)", i > 1 ? ", " : "", i % 5, 'a' + i % 7,
              i, i % 4 ? "1" : "NULL");
  }
  char *q1 = sb_finish(k1), *q2 = sb_finish(k2);
  bool ok = it_ex(db, "%s", q1) && it_ex(db, "%s", q2);
  free(q1);
  free(q2);
  return ok;
}

static void test_keyset_walks(ItDb *db) {
  if (!fill(db))
    return;
  const char *k1[] = {"id"}, *k2[] = {"a", "b"};
  walk(db, "it_kw1", k1, 1, NULL, 0);
  walk(db, "it_kw1", k1, 1, (WalkSort[]){{"grp", LACE_SORT_ASC}}, 1);
  walk(db, "it_kw1", k1, 1, (WalkSort[]){{"grp", LACE_SORT_DESC}}, 1);
  walk(db, "it_kw1", k1, 1,
       (WalkSort[]){{"grp", LACE_SORT_ASC}, {"name", LACE_SORT_DESC}}, 2);
  walk(db, "it_kw1", k1, 1, (WalkSort[]){{"score", LACE_SORT_DESC}}, 1);
  walk(db, "it_kw1", k1, 1, (WalkSort[]){{"id", LACE_SORT_DESC}}, 1);
  walk(db, "it_kw2", k2, 2, NULL, 0);
  walk(db, "it_kw2", k2, 2, (WalkSort[]){{"v", LACE_SORT_DESC}}, 1);
  walk(db, "it_kw2", k2, 2, (WalkSort[]){{"b", LACE_SORT_DESC}}, 1);
  it_ex(db, "DROP TABLE it_kw1");
  it_ex(db, "DROP TABLE it_kw2");
}

/* A text sort column whose collation folds case (MySQL default): ties by
 * collation are ordered by the key, and the walk follows the server */
static void test_keyset_collation(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_kwc");
  it_ex(db, "CREATE TABLE it_kwc (id INTEGER PRIMARY KEY, name VARCHAR(10))");
  it_ex(db, "INSERT INTO it_kwc VALUES (1, 'b'), (2, 'B'), (3, 'a'), (4, 'A'), "
            "(5, 'b '), (6, 'c'), (7, 'C'), (8, 'a'), (9, 'B'), (10, 'b'), "
            "(11, NULL), (12, 'Ab'), (13, 'aB'), (14, 'ab'), (15, 'b')");
  const char *k[] = {"id"};
  walk(db, "it_kwc", k, 1, (WalkSort[]){{"name", LACE_SORT_ASC}}, 1);
  walk(db, "it_kwc", k, 1, (WalkSort[]){{"name", LACE_SORT_DESC}}, 1);
  it_ex(db, "DROP TABLE it_kwc");
}

const ItTest it_keyset_tests[] = {
    {"keyset_walks", test_keyset_walks, IT_ALL, false},
    {"keyset_collation", test_keyset_collation, IT_ALL, false},
    {NULL, NULL, 0, false},
};
