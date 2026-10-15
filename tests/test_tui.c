/*
 * Lace
 * TUI unit tests: SQL scanning in the query tab, WHERE/ORDER BY building
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "test.h"
#include "query_internal.h"
#include "core/app_state.h"
#include "views/text_input.h"
#include "row_key.h"
#include "config/connections.h"
#include "config/recent.h"
#include "app/app.h"
#include <getopt.h>
#include <locale.h>
#include <unistd.h>

/* ---- Statement splitting --------------------------------------------- */

static void test_statement_length(void) {
  /* ';' inside strings, identifiers and comments does not end a statement */
  CHECK_SIZE(query_statement_length("SELECT ';' -- ;\n; SELECT 2", "sqlite"), 16);
  CHECK_SIZE(query_statement_length("SELECT /* ; */ 1; x", "sqlite"), 16);
  CHECK_SIZE(query_statement_length("SELECT \"a;b\"; x", "postgres"), 12);
  /* PostgreSQL dollar quoting, MySQL backslash escapes and # comments */
  CHECK_SIZE(query_statement_length("SELECT $f$ ; $f$; x", "postgres"), 16);
  CHECK_SIZE(query_statement_length("SELECT 'a\\';b'; x", "mysql"), 14);
  CHECK_SIZE(query_statement_length("SELECT 1 # ;\n; x", "mysql"), 13);
  /* Without MySQL rules a backslash is an ordinary character */
  CHECK_SIZE(query_statement_length("SELECT 'a\\'; x", "postgres"), 11);
  /* SQLite accepts `ident` and [ident] */
  CHECK_SIZE(query_statement_length("SELECT [a;b], `c;d`; x", "sqlite"), 19);
}

static void test_sql_shape(void) {
  QuerySqlShape sh;
  query_sql_shape("  select 1 /* c */ ;  -- tail", "sqlite", &sh);
  CHECK(strcmp(sh.first_keyword, "SELECT") == 0);
  CHECK_SIZE(sh.end, 10);
  CHECK(query_sql_pageable(&sh));

  query_sql_shape("WITH x AS (SELECT 1) DELETE FROM t", "postgres", &sh);
  CHECK(strcmp(sh.verb, "DELETE") == 0);
  CHECK(!query_sql_pageable(&sh));

  query_sql_shape("SELECT * FROM t LIMIT 5", "sqlite", &sh);
  CHECK(sh.top_level_limit);
  query_sql_shape("SELECT * FROM (SELECT 1 LIMIT 1) s", "sqlite", &sh);
  CHECK(!sh.top_level_limit);
  query_sql_shape("SELECT 1 INTO OUTFILE '/tmp/x'", "mysql", &sh);
  CHECK(!query_sql_pageable(&sh));
  /* A data-modifying CTE runs once: paging would repeat the write */
  query_sql_shape("WITH u AS (UPDATE t SET a = 1 RETURNING *) SELECT * FROM u",
                  "postgres", &sh);
  CHECK(!query_sql_pageable(&sh));
  query_sql_shape("WITH d AS (delete from t returning id) SELECT count(*) FROM d",
                  "postgres", &sh);
  CHECK(!query_sql_pageable(&sh));
  /* Row locks, function names, strings and comments are not writes */
  query_sql_shape("SELECT * FROM t WHERE id IN (SELECT id FROM u FOR UPDATE) FOR NO KEY UPDATE",
                  "postgres", &sh);
  CHECK(query_sql_pageable(&sh));
  query_sql_shape("SELECT REPLACE(a, 'x', 'y'), INSERT(a, 1, 2, 'z') FROM t", "mysql", &sh);
  CHECK(query_sql_pageable(&sh));
  query_sql_shape("SELECT 'delete', \"update\" /* insert */ FROM t -- merge", "sqlite", &sh);
  CHECK(query_sql_pageable(&sh));
  /* PostgreSQL leaves UPDATE, MERGE unreserved: as columns they are read */
  query_sql_shape("SELECT merge, update FROM t ORDER BY merge", "postgres", &sh);
  CHECK(query_sql_pageable(&sh));
}

static void test_statement_at_cursor(void) {
  const char *text = "SELECT 1;\nSELECT 'a;b';\n\n";
  CHECK_STR_FREE(query_find_at_cursor(text, 3, "sqlite"), "SELECT 1");
  CHECK_STR_FREE(query_find_at_cursor(text, 15, "sqlite"), "SELECT 'a;b'");
  /* Blank space after the last ';' means the previous statement */
  CHECK_STR_FREE(query_find_at_cursor(text, strlen(text), "sqlite"),
                 "SELECT 'a;b'");

  /* The highlight covers exactly the statement Ctrl+R runs */
  size_t s, e;
  const char *pg = "SELECT $$;$$; SELECT 2";
  CHECK(query_find_bounds_at_cursor(pg, 2, "postgres", &s, &e));
  CHECK_SIZE(s, 0);
  CHECK_SIZE(e, 13);
  CHECK_STR_FREE(query_find_at_cursor(pg, 2, "postgres"), "SELECT $$;$$");
}

/* ---- Editable query results ------------------------------------------ */

static void test_extract_table(void) {
  CHECK_STR_FREE(query_extract_table_name("select id from users", LACE_DRIVER_SQLITE),
                 "users");
  /* FROM inside comments, strings and quoted identifiers is not the table */
  CHECK_STR_FREE(query_extract_table_name("SELECT * -- FROM other\nFROM t",
                                          LACE_DRIVER_SQLITE), "t");
  CHECK_STR_FREE(query_extract_table_name("SELECT `x FROM y` FROM t",
                                          LACE_DRIVER_MYSQL), "t");
  CHECK_STR_FREE(query_extract_table_name("SELECT 'a\\' FROM o' FROM t",
                                          LACE_DRIVER_MYSQL), "t");
  CHECK_STR_FREE(query_extract_table_name("SELECT $$ FROM o $$ FROM t",
                                          LACE_DRIVER_POSTGRES), "t");
  /* PostgreSQL folds bare identifiers; quoted parts keep case and dots */
  CHECK_STR_FREE(query_extract_table_name("SELECT * FROM Sales.\"odd.name\"",
                                          LACE_DRIVER_POSTGRES),
                 "sales.\"odd.name\"");
  /* Not a single-table read: not editable */
  CHECK_STR_FREE(query_extract_table_name("SELECT * FROM a JOIN b ON 1",
                                          LACE_DRIVER_SQLITE), NULL);
  CHECK_STR_FREE(query_extract_table_name("SELECT count(*) FROM a",
                                          LACE_DRIVER_SQLITE), NULL);
  CHECK_STR_FREE(query_extract_table_name("SELECT * FROM a, b",
                                          LACE_DRIVER_SQLITE), NULL);
}

/* ---- Filters ------------------------------------------------------------ */

static char *where_for(const char *driver, FilterOperator op, const char *v,
                       const char *v2) {
  LaceColumn cols[2] = {{0}};
  cols[0].name = "name";
  cols[1].name = "id";
  LaceSchema schema = {0};
  schema.columns = cols;
  schema.num_columns = 2;
  ColumnFilter f[2] = {{0}};
  /* A filter on a column the schema lacks is skipped without a dangling AND */
  f[0].column_index = 9;
  f[0].op = FILTER_OP_EQ;
  strcpy(f[0].value, "skip");
  f[1].column_index = op == FILTER_OP_IN ? 1 : 0;
  f[1].op = op;
  strcpy(f[1].value, v);
  strcpy(f[1].value2, v2 ? v2 : "");
  TableFilters tf = {f, 2, 2};
  char *err = NULL;
  char *w = filters_build_where(&tf, &schema, driver, &err);
  free(err);
  return w;
}

static void test_filters_where(void) {
  CHECK_STR_FREE(where_for("sqlite", FILTER_OP_EQ, "it's", NULL),
                 "\"name\" = 'it''s'");
  /* MySQL backslash text cannot break out of the literal */
  CHECK_STR_FREE(where_for("mysql", FILTER_OP_EQ, "x\\' OR 1=1 -- ", NULL),
                 "`name` = _utf8mb4 X'785c27204f5220313d31202d2d20'");
  CHECK_STR_FREE(where_for("postgres", FILTER_OP_CONTAINS, "o'k", NULL),
                 "\"name\" LIKE '%o''k%'");
  CHECK_STR_FREE(where_for("sqlite", FILTER_OP_REGEX, "ab", NULL),
                 "\"name\" GLOB '*ab*'");
  CHECK_STR_FREE(where_for("mysql", FILTER_OP_BETWEEN, "a", "z"),
                 "`name` BETWEEN 'a' AND 'z'");
  /* IN: numbers inline, anything else (even "--") a quoted literal */
  CHECK_STR_FREE(where_for("sqlite", FILTER_OP_IN, "1, -2.5, --, 'q''t'", NULL),
                 "\"id\" IN (1, -2.5, '--', 'q''t')");
}

static void test_order_by(void) {
  LaceColumn cols[2] = {{0}};
  cols[0].name = "a`b";
  cols[1].name = "c";
  LaceSchema schema = {0};
  schema.columns = cols;
  schema.num_columns = 2;
  SortEntry e[3] = {{1, SORT_DESC}, {7, SORT_ASC}, {0, SORT_ASC}};
  CHECK_STR_FREE(filters_build_order_by(e, 3, &schema, "mysql"),
                 "`c` DESC, `a``b` ASC");
  CHECK_STR_FREE(filters_build_order_by(e, 3, &schema, "sqlite"),
                 "\"c\" DESC, \"a`b\" ASC");
  CHECK_STR_FREE(filters_build_order_by(&e[1], 1, &schema, "sqlite"), NULL);
}

/* ---- Text input widget ------------------------------------------------ */

static UiEvent key_char(int c) {
  UiEvent e = {0};
  e.type = UI_EVENT_KEY;
  e.key.key = c;
  return e;
}

static UiEvent key_special(UiKeyCode code) {
  UiEvent e = {0};
  e.type = UI_EVENT_KEY;
  e.key.key = (int)code;
  e.key.is_special = true;
  return e;
}

static UiEvent key_ctrl(char letter) {
  UiEvent e = {0};
  e.type = UI_EVENT_KEY;
  e.key.key = letter;
  e.key.mods = UI_MOD_CTRL;
  return e;
}

static void type_text(TextInput *in, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    UiEvent e = key_char(*p);
    text_input_handle_key(in, &e);
  }
}

static void press(TextInput *in, UiEvent e) { text_input_handle_key(in, &e); }

static void test_text_input(void) {
  TextInput in;
  text_input_init(&in, "héllo", false);
  CHECK_SIZE(in.cursor, 6); /* Bytes: é is two */
  press(&in, key_special(UI_KEY_LEFT));
  press(&in, key_special(UI_KEY_LEFT));
  press(&in, key_special(UI_KEY_LEFT));
  press(&in, key_special(UI_KEY_LEFT));
  CHECK_SIZE(in.cursor, 1); /* Before é, not inside it */
  press(&in, key_special(UI_KEY_DELETE));
  CHECK(strcmp(in.buf, "hllo") == 0);
  type_text(&in, "ö"); /* Arrives as two bytes */
  CHECK(strcmp(in.buf, "höllo") == 0);
  press(&in, key_special(UI_KEY_BACKSPACE));
  CHECK(strcmp(in.buf, "hllo") == 0);
  press(&in, key_ctrl('K'));
  CHECK(strcmp(in.buf, "h") == 0);
  press(&in, key_ctrl('A'));
  type_text(&in, "a long enough text to make the owned buffer grow past 64 bytes");
  CHECK_SIZE(in.len, 63);
  CHECK(in.buf[in.len] == '\0');
  press(&in, key_ctrl('U'));
  CHECK(strcmp(in.buf, "h") == 0 && in.cursor == 0);
  /* A non-editing key is left to the caller */
  UiEvent f5 = key_special(UI_KEY_F5);
  CHECK(!text_input_handle_key(&in, &f5));
  text_input_free(&in);

  /* Bound to a fixed array: never overflows, never splits a character */
  char small[6] = "abc";
  text_input_bind(&in, small, sizeof(small), false);
  type_text(&in, "dé"); /* "abcd" + 2 bytes would need 7 */
  CHECK(strcmp(small, "abcd") == 0);
  type_text(&in, "e");
  CHECK(strcmp(small, "abcde") == 0);
  type_text(&in, "f");
  CHECK(strcmp(small, "abcde") == 0);
  text_input_set(&in, "xyzé!"); /* 5 bytes fit: "xyzé", not the '!' */
  CHECK(strcmp(small, "xyzé") == 0);
  text_input_set(&in, "wxyzé"); /* Would cut é in half: dropped whole */
  CHECK(strcmp(small, "wxyz") == 0);
  text_input_free(&in);
  CHECK(strcmp(small, "wxyz") == 0); /* Bound text stays the caller's */
}

/* ---- Row keys ------------------------------------------------------------- */

static void test_row_key(void) {
  /* Table (id PK, v); the data has them in another order, plus an alias */
  LaceColumn scols[2] = {{0}};
  scols[0].name = "id";
  scols[0].primary_key = true;
  scols[1].name = "v";
  LaceSchema schema = {0};
  schema.columns = scols;
  schema.num_columns = 2;

  LaceColumn dcols[3] = {{0}};
  dcols[0].name = "v";
  dcols[1].name = "id";
  dcols[2].name = "id"; /* SELECT v, id, id */
  LaceValue cells[2][3];
  cells[0][0] = lace_value_text("a");
  cells[0][1] = lace_value_int(7);
  cells[0][2] = lace_value_int(7);
  cells[1][0] = lace_value_text("b");
  cells[1][1] = lace_value_null();
  cells[1][2] = lace_value_null();
  Row rows[2] = {{cells[0], 3}, {cells[1], 3}};
  LaceResult data = {0};
  data.columns = dcols;
  data.num_columns = 2; /* First without the repeated id */
  data.rows = rows;
  data.num_rows = 2;

  RowKey key;
  const char *why = NULL;
  CHECK(row_key_build(&key, &data, 0, &schema, true, &why) == ROW_KEY_OK);
  CHECK(key.count == 1 && strcmp(key.col_names[0], "id") == 0 &&
        key.values[0].int_val == 7); /* Found by name, not position */
  row_key_free(&key);

  CHECK(row_key_build(&key, &data, 1, &schema, true, &why) == ROW_KEY_BAD);
  CHECK(why && strstr(why, "NULL"));

  data.num_columns = 3; /* Now id appears twice */
  CHECK(row_key_build(&key, &data, 0, &schema, true, &why) == ROW_KEY_NONE);
  CHECK(row_key_build(&key, &data, 0, &schema, false, &why) == ROW_KEY_OK);
  row_key_free(&key);

  data.num_columns = 1; /* Key column not loaded */
  CHECK(row_key_build(&key, &data, 0, &schema, false, &why) == ROW_KEY_NONE);
  CHECK(row_key_build(&key, &data, 0, NULL, false, &why) == ROW_KEY_NONE);

  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 3; c++)
      lace_value_free(&cells[r][c]);
}

/* Every item's parent is the folder whose array holds it */
static bool tree_linked(const ConnectionItem *folder) {
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    const ConnectionItem *c = &folder->folder.children[i];
    if (c->parent != folder)
      return false;
    if (c->type == CONN_ITEM_FOLDER && !tree_linked(c))
      return false;
  }
  return true;
}

static ConnectionItem *child_named(ConnectionItem *folder, const char *name) {
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    const char *n = connmgr_item_name(&folder->folder.children[i]);
    if (n && strcmp(n, name) == 0)
      return &folder->folder.children[i];
  }
  return NULL;
}

/* Items live by value in their folder's array: growing, shifting and moving
 * must leave every parent pointer right */
static void test_connection_tree(void) {
  ConnectionManager *mgr = connmgr_new();
  CHECK(connmgr_add_folder(&mgr->root, connmgr_new_folder("A")));
  ConnectionItem *a = child_named(&mgr->root, "A");
  SavedConnection *c = connmgr_new_connection();
  free(c->name);
  c->name = str_dup("in-a");
  CHECK(connmgr_add_connection(a, c));
  CHECK(connmgr_add_folder(a, connmgr_new_folder("A1")));
  /* Grow the root's array past its first capacity: A moves */
  for (int i = 0; i < 20; i++) {
    char name[16];
    snprintf(name, sizeof(name), "f%d", i);
    CHECK(connmgr_add_folder(&mgr->root, connmgr_new_folder(name)));
  }
  CHECK(tree_linked(&mgr->root));
  /* Removing the first item shifts A's siblings */
  CHECK(connmgr_remove_item(mgr, child_named(&mgr->root, "f0")));
  CHECK(tree_linked(&mgr->root));
  /* Move A (with children) into a later sibling, then a folder up */
  a = child_named(&mgr->root, "A");
  CHECK(connmgr_move_item(mgr, a, child_named(&mgr->root, "f5"), NULL));
  CHECK(tree_linked(&mgr->root));
  ConnectionItem *f5 = child_named(&mgr->root, "f5");
  CHECK(f5 && child_named(f5, "A") && child_named(child_named(f5, "A"), "in-a"));
  ConnectionItem *a1 = child_named(child_named(f5, "A"), "A1");
  CHECK(a1 && connmgr_move_item(mgr, a1, &mgr->root, NULL));
  CHECK(tree_linked(&mgr->root));
  /* Reorder within one folder */
  CHECK(connmgr_move_item(mgr, child_named(&mgr->root, "f19"), &mgr->root, NULL));
  CHECK(tree_linked(&mgr->root));
  connmgr_free(mgr);
}

/* Passwords on the command line are blanked in argv (ps, /proc/PID/cmdline),
 * both of them in a +ssh URL; the parsed copy keeps them */
static void scrub_case(const char *url, const char *want_argv) {
  char *arg = str_dup(url);
  char prog[] = "lace";
  char *argv[] = {prog, arg, NULL};
  AppConfig cfg = {0};
  optind = 1;
#ifdef __APPLE__
  optreset = 1;
#endif
  CHECK(app_parse_args(2, argv, &cfg));
  CHECK_STR_FREE(str_dup(arg), want_argv);
  CHECK_STR_FREE(str_dup(cfg.connstr), url);
  app_config_free(&cfg);
  free(arg);
}

static void test_argv_scrub(void) {
  scrub_case("postgres://u:secret@h/db", "postgres://u:xxxxxx@h/db");
  scrub_case("postgres+ssh://su:sshpw@jump:22/du:dbpw@db:5432/app",
             "postgres+ssh://su:xxxxx@jump:22/du:xxxx@db:5432/app");
  scrub_case("mysql://u@h:3306/db", "mysql://u@h:3306/db");
  scrub_case("postgres://u:p@ss@h/db", "postgres://u:xxxx@h/db");
  /* A raw '/' in a password (the URL then does not parse, but the password
   * must still not show in ps) */
  scrub_case("postgres://u:pa/ss@h/db", "postgres://u:xxxxx@h/db");
  scrub_case("postgres+ssh://su:s/p@j:22/du:d/p@h:5432/db",
             "postgres+ssh://su:xxx@j:22/du:xxx@h:5432/db");
  /* Password-like query parameters */
  scrub_case("postgres://u@h/db?sslmode=require&password=sec&x=1",
             "postgres://u@h/db?sslmode=require&password=xxx&x=1");
  scrub_case("mysql://u@h/db?PWD=topsecret", "mysql://u@h/db?PWD=xxxxxxxxx");
  /* A '/' or ':' in the database name is not a password */
  scrub_case("sqlite:///tmp/a:b/c.db", "sqlite:///tmp/a:b/c.db");
}

static char *write_temp(const char *name, const char *content) {
  char *dir = str_dup("/tmp/lace-tt-XXXXXX");
  if (!mkdtemp(dir)) {
    free(dir);
    return NULL;
  }
  char *path = str_printf("%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  fputs(content, f);
  fclose(f);
  free(dir);
  return path;
}

static void remove_temp(char *path) {
  unlink(path);
  char *slash = strrchr(path, '/');
  *slash = '\0';
  rmdir(path);
  free(path);
}

static void count_conn(ConnectionItem *item, void *ctx) {
  (void)item;
  (*(int *)ctx)++;
}

/* connections.json that is malformed, hostile or from another version
 * loads safely, or is refused and then never overwritten */
static void test_connections_file(void) {
  char *err = NULL;
  /* A root that is not a folder is refused (it was read as one) */
  char *p = write_temp("c.json", "{\"root\":{\"type\":\"connection\",\"id\":\"x\","
                                 "\"driver\":\"postgres\",\"host\":\"h\"}}");
  ConnectionManager *m = connmgr_load_file(p, &err);
  CHECK(m == NULL && err != NULL);
  free(err);
  err = NULL;
  remove_temp(p);

  /* Unknown item kinds are skipped; missing, unsafe and repeated ids are
   * replaced */
  p = write_temp("c.json",
                 "{\"root\":{\"type\":\"folder\",\"name\":\"r\",\"children\":["
                 "{\"type\":\"bogus\"},"
                 "{\"type\":\"connection\",\"name\":\"a\",\"driver\":\"sqlite\"},"
                 "{\"type\":\"connection\",\"id\":\"../../x\",\"name\":\"b\"},"
                 "{\"type\":\"connection\",\"id\":\"same\",\"name\":\"c\"},"
                 "{\"type\":\"connection\",\"id\":\"same\",\"name\":\"d\"}]}}");
  m = connmgr_load_file(p, &err);
  CHECK(m != NULL);
  if (m) {
    int n = 0;
    connmgr_for_each_connection(m, count_conn, &n);
    CHECK(n == 4);
    CHECK(m->root.folder.num_children == 4);
    const char *ids[4];
    for (int i = 0; i < 4 && i < (int)m->root.folder.num_children; i++) {
      ids[i] = m->root.folder.children[i].connection.id;
      CHECK(connmgr_id_valid(ids[i]));
    }
    CHECK(strcmp(ids[2], "same") == 0 && strcmp(ids[3], "same") != 0);
    CHECK(m->modified); /* Saved back with the new ids */
    /* Another process changes the file: our save refuses */
    sleep(1);
    FILE *f = fopen(p, "a");
    fputs(" ", f);
    fclose(f);
    CHECK(!connmgr_save(m, &err));
    free(err);
    err = NULL;
    connmgr_free(m);
  }
  remove_temp(p);

  /* Nesting past the depth limit is refused, not half-loaded */
  char *deep = str_dup("");
  for (int i = 0; i < 120; i++) {
    char *next = str_printf("%s{\"type\":\"folder\",\"name\":\"f\",\"children\":[", deep);
    free(deep);
    deep = next;
  }
  char *doc = str_printf("{\"root\":%s", deep);
  for (int i = 0; i < 120; i++) {
    char *next = str_printf("%s]}", doc);
    free(doc);
    doc = next;
  }
  char *closed = str_printf("%s}", doc);
  p = write_temp("c.json", closed);
  m = connmgr_load_file(p, &err);
  CHECK(m == NULL);
  free(err);
  err = NULL;
  remove_temp(p);
  free(deep);
  free(doc);
  free(closed);

  CHECK(!connmgr_id_valid("../x") && !connmgr_id_valid("") && connmgr_id_valid("a-1"));
}

/* The recent list never keeps a password, also an SSH one, and keeps the
 * tunnel and its key */
static void test_recent_passwords(void) {
  CHECK_STR_FREE(recent_strip_passwords("postgres://app:s3cret@db.internal:5432/app"),
                 "postgres://app@db.internal:5432/app");
  CHECK_STR_FREE(recent_strip_passwords("postgres+ssh://me:sshpw@bastion/app:pw@db/app"
                                        "?ssh_key=%7E%2F.ssh%2Fk"),
                 "postgres+ssh://me@bastion/app@db/app?ssh_key=~%2F.ssh%2Fk");
  CHECK_STR_FREE(recent_strip_passwords("sqlite:///tmp/a%20b.db"), "sqlite:///tmp/a b.db");
  CHECK(recent_strip_passwords("not a url") == NULL);

  RecentList l = {0};
  for (int i = 0; i < RECENT_MAX + 3; i++) {
    char url[64];
    snprintf(url, sizeof(url), "sqlite:///tmp/%d.db", i);
    CHECK(recent_add(&l, url));
  }
  CHECK_SIZE(l.count, RECENT_MAX);
  CHECK(str_eq(l.items[0].url, "sqlite:///tmp/12.db"));
  CHECK(recent_add(&l, "sqlite:///tmp/5.db")); /* Again: moves to the top */
  CHECK_SIZE(l.count, RECENT_MAX);
  CHECK(str_eq(l.items[0].url, "sqlite:///tmp/5.db"));
  CHECK(!str_eq(l.items[1].url, "sqlite:///tmp/5.db"));
  recent_free(&l);
}

int main(void) {
  test_recent_passwords();
  test_connection_tree();
  test_connections_file();
  test_argv_scrub();
  /* The TUI runs in the user's UTF-8 locale (setlocale(LC_ALL, "")); the
   * character functions decode with mbrtowc, so the tests need one too */
  if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8")) {
    fprintf(stderr, "no UTF-8 locale available\n");
    return 1;
  }
  test_statement_length();
  test_sql_shape();
  test_statement_at_cursor();
  test_extract_table();
  test_filters_where();
  test_order_by();
  test_text_input();
  test_row_key();
  return test_report("tui");
}
