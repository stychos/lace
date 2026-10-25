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
#include "core/connstr_display.h"
#include "row_key.h"
#include "config/connections.h"
#include "config/recent.h"
#include "config/config.h"
#include "config/workspace_json.h"
#include "config/session.h"
#include "platform/platform.h"
#include "util/json_helpers.h"
#include <cjson/cJSON.h>
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
  CHECK_STR_FREE(where_for("postgres", FILTER_OP_STARTS, "o'k", NULL),
                 "\"name\" LIKE 'o''k%'");
  CHECK_STR_FREE(where_for("mysql", FILTER_OP_ENDS, "ab", NULL),
                 "`name` LIKE '%ab'");
  /* The new operators sit after RAW so version-1 session integers hold */
  CHECK_SIZE(FILTER_OP_STARTS, 15);
  CHECK_SIZE(FILTER_OP_ENDS, 16);
  CHECK_SIZE(FILTER_OP_COUNT, 17);
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

static void test_connstr_display(void) {
  CHECK_STR_FREE(connstr_display("sqlite:///tmp/a.db"), "sqlite: /tmp/a.db");
  CHECK_STR_FREE(connstr_display("postgres://u:secret@h:5433/d"),
                 "postgres://u@h:5433/d");
  CHECK_STR_FREE(connstr_display("mysql+ssh://su:sp@sh:22/u:p@dbh:3306/d"),
                 "mysql://u@dbh:3306/d  via ssh su@sh");
}

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
/* The connection tab's actions: names, categories, frontends, no clash in their category */
static void test_hotkeys_connection_tab(void) {
  Config *c = config_get_defaults();
  CHECK(hotkey_action_from_key("new_connection") == HOTKEY_NEW_CONNECTION);
  CHECK_STR(hotkey_action_name(HOTKEY_CONN_OPEN), "Open here");
  CHECK(hotkey_get_category(HOTKEY_CONN_OPEN_BACKGROUND) == HOTKEY_CAT_CONNECT);
  CHECK(hotkey_action_frontends(HOTKEY_CONN_CLEAR) == CONFIG_FE_NOTCURSES);
  CHECK(hotkey_action_frontends(HOTKEY_CONN_TEST) == CONFIG_FE_ALL);
  CHECK(hotkey_action_frontends(HOTKEY_CONN_EDIT) == CONFIG_FE_NCURSES);
  c->frontend = CONFIG_FE_NOTCURSES;
  static const HotkeyAction fresh[] = {HOTKEY_NEW_CONNECTION, HOTKEY_CONN_OPEN, HOTKEY_CONN_OPEN_BACKGROUND,
      HOTKEY_CONN_OPEN_NEW_WORKSPACE, HOTKEY_CONN_OPEN_TO_WORKSPACE, HOTKEY_CONN_SAVE_AS_NEW, HOTKEY_CONN_CLEAR};
  static const char *keys[] = {"CTRL+T", "ENTER", "CTRL+B", "CTRL+N", "CTRL+O", "CTRL+Y", "CTRL+L"};
  for (size_t i = 0; i < 7; i++)
    CHECK(hotkey_find_conflict(c, fresh[i], keys[i]) == HOTKEY_COUNT);
  /* The sub-views and following keys are every frontend's; an Alt letter
   * matches regardless of case (the translators report it upper) */
  CHECK(hotkey_action_frontends(HOTKEY_SUBVIEW_INDEXES) == CONFIG_FE_ALL);
  CHECK(hotkey_action_frontends(HOTKEY_FOLLOW_FK) == CONFIG_FE_ALL);
  CHECK(hotkey_action_from_key("show_schema") == HOTKEY_COUNT);
  UiEvent alt_r = {.type = UI_EVENT_KEY, .key = {.key = 'R', .mods = UI_MOD_ALT}};
  CHECK(hotkey_matches(c, &alt_r, HOTKEY_REFERENCED_BY));
  config_free(c);
}

static void test_config_connection_tab_keys(void) {
  char dir[] = "/tmp/lace-cfg-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  char *p = str_printf("%s/config.json", dir);
  char *err = NULL;
  /* Defaults */
  Config *c = config_get_defaults();
  CHECK(c->general.keep_connection_tab);
  CHECK(c->general.last_workspace_close == CONFIG_LAST_WS_QUIT);
  config_free(c);
  /* Migration: the old key on, the new one absent */
  const char *text = "{\"general\":{\"close_conn_on_last_tab\":true}}";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  c = config_load_file(p, &err);
  CHECK(c && !err && !c->general.keep_connection_tab);
  /* Saved: the new keys are written, the old one is gone */
  CHECK(config_save_file(c, p, &err));
  config_free(c);
  char *back = platform_read_file(p, 1 << 20);
  CHECK(back && strstr(back, "\"keep_connection_tab\":\tfalse") && !strstr(back, "close_conn_on_last_tab"));
  CHECK(back && strstr(back, "\"on_last_workspace_close\":\t\"quit\""));
  free(back);
  /* The new key wins over the old one */
  text = "{\"general\":{\"close_conn_on_last_tab\":true,\"keep_connection_tab\":true,"
         "\"on_last_workspace_close\":\"new\"}}";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  c = config_load_file(p, &err);
  CHECK(c && !err && c->general.keep_connection_tab);
  CHECK(c->general.last_workspace_close == CONFIG_LAST_WS_NEW);
  config_free(c);
  /* An unknown or mistyped value is the default, not an error */
  const char *odd[] = {"\"exit\"", "1", "null"};
  for (int i = 0; i < 3; i++) {
    char *t = str_printf("{\"general\":{\"on_last_workspace_close\":%s}}", odd[i]);
    CHECK(platform_write_file_private(p, t, strlen(t)));
    c = config_load_file(p, &err);
    CHECK(c && !err && c->general.last_workspace_close == CONFIG_LAST_WS_QUIT);
    config_free(c);
    free(t);
  }
  unlink(p);
  rmdir(dir);
  free(p);
}

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

static void test_config_file(void) {
  char dir[] = "/tmp/lace-cfg-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  char *p = str_printf("%s/config.json", dir);
  /* A file another frontend wrote: its own section, an unknown general key,
   * and bindings this TUI cannot receive */
  const char *text =
      "{\"general\":{\"delete_confirmation\":false,\"row_numbers\":2,"
      "\"page_size\":500,\"future_key\":1},"
      "\"hotkeys\":{\"palette\":[\"CTRL+SHIFT+P\"],\"quit\":[\"q\"]},"
      "\"notcurses\":{\"clipboard_osc52\":false},\"gtk\":{}}";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  char *err = NULL;
  Config *c = config_load_file(p, &err);
  CHECK(c && !err);
  CHECK(!c->general.delete_confirmation);
  CHECK(c->general.row_numbers == 2);
  CHECK(c->general.page_size == 500);
  CHECK(c->general.approximate_counts); /* default */
  /* Sections this TUI does not own are kept, by name */
  cJSON *nc = config_section(c, "notcurses");
  CHECK(nc && !json_get_bool(nc, "clipboard_osc52", true));
  /* Change one owned value, save, reload: the others survive */
  c->general.quit_confirmation = true;
  CHECK(config_save_file(c, p, &err));
  config_free(c);
  c = config_load_file(p, &err);
  CHECK(c && !err && c->general.quit_confirmation && c->general.row_numbers == 2);
  CHECK(cJSON_GetObjectItem(c->extra, "gtk") != NULL);
  CHECK(!json_get_bool(config_section(c, "notcurses"), "clipboard_osc52", true));
  /* The file on disk also still has the other section verbatim */
  char *back = platform_read_file(p, 1 << 20);
  CHECK(back && strstr(back, "\"clipboard_osc52\"") && strstr(back, "\"gtk\""));
  /* Only the bindings that differ from the defaults are written: the two the
   * file had (palette is not this frontend's but is kept), nothing else */
  CHECK(back && strstr(back, "\"palette\"") && !strstr(back, "\"move_up\"") && !strstr(back, "\"refresh\""));
  CHECK(back && strstr(back, "\"quit\":\t[\"q\"]")); /* not the default ["q","CTRL+X","F10"] */
  /* An action the user emptied is written as [] so it stays unbound */
  while (c->hotkeys[HOTKEY_REFRESH].num_keys) hotkey_remove_key(c, HOTKEY_REFRESH, 0);
  CHECK(config_save_file(c, p, &err));
  free(back);
  back = platform_read_file(p, 1 << 20);
  CHECK(back && strstr(back, "\"refresh\":\t[]"));
  /* An unknown key inside an owned section is dropped (owner's struct is the truth) */
  CHECK(back && !strstr(back, "future_key"));
  free(back);
  /* A section created on demand is written */
  cJSON_AddBoolToObject(config_section(c, "notcurses"), "new_flag", true);
  CHECK(config_save_file(c, p, &err));
  back = platform_read_file(p, 1 << 20);
  CHECK(back && strstr(back, "new_flag"));
  free(back);
  config_free(c);
  /* config_copy carries extra */
  c = config_load_file(p, &err);
  Config *d = config_copy(c);
  CHECK(d && cJSON_GetObjectItem(d->extra, "notcurses") != NULL);
  config_free(d);
  config_free(c);
  CHECK(!err);

  /* An untouched foreign section comes from the file as it is at save time */
  text = "{\"general\":{},\"gtk\":{\"x\":1},\"notcurses\":{\"y\":1}}";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  c = config_load_file(p, &err);
  CHECK(c && !err);
  c->general.show_header = false;
  cJSON_AddNumberToObject(config_section(c, "notcurses"), "z", 3);
  text = "{\"general\":{},\"gtk\":{\"x\":2},\"notcurses\":{\"y\":2}}";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  CHECK(config_save_file(c, p, &err) && !err);
  config_free(c);
  c = config_load_file(p, &err);
  CHECK(c && !err && !c->general.show_header);
  CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(c->extra, "gtk"), "x")->valueint == 2);
  cJSON *ncs = cJSON_GetObjectItem(c->extra, "notcurses");
  CHECK(ncs && cJSON_GetObjectItem(ncs, "z") && cJSON_GetObjectItem(ncs, "y")->valueint == 1);
  config_free(c);

  /* A non-object member named like a section is replaced by an object */
  text = "{\"notcurses\":5}";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  c = config_load_file(p, &err);
  CHECK(c && !err);
  cJSON *sec = config_section(c, "notcurses");
  CHECK(cJSON_IsObject(sec) && cJSON_GetArraySize(sec) == 0);
  CHECK(config_save_file(c, p, &err) && !err);
  back = platform_read_file(p, 1 << 20);
  CHECK(back && !strstr(back, ": 5"));
  free(back);
  config_free(c);

  /* A corrupt file is never overwritten by a save */
  text = "{\"general\": {\"show_header\": tru";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  c = config_get_defaults();
  CHECK(!config_save_file(c, p, &err));
  CHECK(err && strstr(err, "byte"));
  free(err);
  err = NULL;
  back = platform_read_file(p, 1 << 20);
  CHECK(back && strcmp(back, text) == 0);
  free(back);
  config_free(c);
  c = config_load_file(p, &err);
  CHECK(c && err && strstr(err, "byte") && c->general.show_header);
  config_free(c);
  free(err);
  err = NULL;

  /* A non-object root: load gives defaults and an error; save refuses */
  text = "[1,2]";
  CHECK(platform_write_file_private(p, text, strlen(text)));
  c = config_load_file(p, &err);
  CHECK(c && err && cJSON_IsObject(c->extra) && cJSON_GetArraySize(c->extra) == 0);
  free(err);
  err = NULL;
  CHECK(!config_save_file(c, p, &err) && err);
  back = platform_read_file(p, 1 << 20);
  CHECK(back && strcmp(back, text) == 0);
  free(back);
  config_free(c);
  free(err);
  unlink(p);
  rmdir(dir);
  free(p);
}

static UiEvent key_ev(int key, bool special, UiKeyMod mods) {
  UiEvent e = {0};
  e.type = UI_EVENT_KEY;
  e.key.key = key;
  e.key.is_special = special;
  e.key.mods = mods;
  return e;
}

static void test_hotkey_parse(void) {
  Config *c = config_get_defaults();
  CHECK(hotkey_add_key(c, HOTKEY_CONN_TAB_3, "ALT+3"));
  UiEvent e = key_ev('3', false, UI_MOD_ALT);
  CHECK(hotkey_matches(c, &e, HOTKEY_CONN_TAB_3));
  e = key_ev('3', false, UI_MOD_NONE);
  CHECK(!hotkey_matches(c, &e, HOTKEY_CONN_TAB_3));
  CHECK(hotkey_add_key(c, HOTKEY_NEXT_CONN_TAB, "CTRL+PGDN"));
  e = key_ev(UI_KEY_PAGEDOWN, true, UI_MOD_CTRL);
  CHECK(hotkey_matches(c, &e, HOTKEY_NEXT_CONN_TAB));
  e = key_ev(UI_KEY_PAGEDOWN, true, UI_MOD_NONE);
  CHECK(!hotkey_matches(c, &e, HOTKEY_NEXT_CONN_TAB));
  CHECK(hotkey_matches(c, &e, HOTKEY_PAGE_DOWN));
  CHECK(hotkey_add_key(c, HOTKEY_EDITOR_SAVE, "SHIFT+ENTER"));
  e = key_ev(UI_KEY_ENTER, true, UI_MOD_SHIFT);
  CHECK(hotkey_matches(c, &e, HOTKEY_EDITOR_SAVE));
  CHECK(hotkey_add_key(c, HOTKEY_CLEAR_FILTERS, "SHIFT+y"));
  e = key_ev('Y', false, UI_MOD_NONE);
  CHECK(hotkey_matches(c, &e, HOTKEY_CLEAR_FILTERS));
  CHECK(hotkey_add_key(c, HOTKEY_VIEW_SWITCHER, "SHIFT+CTRL+P"));
  e = key_ev('P', false, UI_MOD_CTRL | UI_MOD_SHIFT);
  CHECK(hotkey_matches(c, &e, HOTKEY_VIEW_SWITCHER));
  e = key_ev('P', false, UI_MOD_CTRL);
  CHECK(!hotkey_matches(c, &e, HOTKEY_VIEW_SWITCHER));
  e = key_ev('G', false, UI_MOD_CTRL);
  CHECK(hotkey_matches(c, &e, HOTKEY_GOTO_ROW));
  CHECK_STR_FREE(hotkey_get_display(c, HOTKEY_CONN_TAB_3), "Alt+3");
  CHECK_STR_FREE(hotkey_get_display(c, HOTKEY_NEXT_CONN_TAB), "Ctrl+PgDn");
  CHECK(hotkey_action_frontends(HOTKEY_QUIT) == CONFIG_FE_ALL);
  CHECK(hotkey_action_frontends(HOTKEY_PALETTE) == CONFIG_FE_NOTCURSES);
  CHECK(hotkey_action_frontends(HOTKEY_SIDEBAR_FILTER) == CONFIG_FE_ALL); /* the notcurses sidebar's search key */
  CHECK(hotkey_action_frontends(HOTKEY_TOGGLE_HEADER) == CONFIG_FE_NCURSES);
  CHECK_STR_FREE(hotkey_get_display(c, HOTKEY_QUIT), "q, Ctrl+X, F10");
  CHECK_STR_FREE(hotkey_get_display(c, HOTKEY_TOGGLE_LOG), "Ctrl+`, F8");
  config_free(c);
}

static void test_hotkey_key_string(void) {
  UiEvent e = key_ev('A', false, UI_MOD_CTRL);
  CHECK_STR_FREE(hotkey_key_string(&e), "CTRL+A");
  e = key_ev('P', false, UI_MOD_CTRL | UI_MOD_SHIFT);
  CHECK_STR_FREE(hotkey_key_string(&e), "CTRL+SHIFT+P");
  e = key_ev('X', false, UI_MOD_NONE); /* a capital is its own key */
  CHECK_STR_FREE(hotkey_key_string(&e), "X");
  e = key_ev('x', false, UI_MOD_SHIFT); /* Shift alone is the case, never a prefix */
  CHECK_STR_FREE(hotkey_key_string(&e), "x");
  e = key_ev(' ', false, UI_MOD_NONE);
  CHECK_STR_FREE(hotkey_key_string(&e), "SPACE");
  e = key_ev(',', false, UI_MOD_NONE);
  CHECK_STR_FREE(hotkey_key_string(&e), "COMMA");
  e = key_ev(UI_KEY_F5, true, UI_MOD_NONE);
  CHECK_STR_FREE(hotkey_key_string(&e), "F5");
  e = key_ev(UI_KEY_F12, true, UI_MOD_ALT);
  CHECK_STR_FREE(hotkey_key_string(&e), "ALT+F12");
  e = key_ev(UI_KEY_BTAB, true, UI_MOD_NONE);
  CHECK_STR_FREE(hotkey_key_string(&e), "SHIFT+TAB");
  e = key_ev(UI_KEY_LEFT, true, UI_MOD_ALT);
  CHECK_STR_FREE(hotkey_key_string(&e), "ALT+LEFT");
  e = key_ev(UI_KEY_PAGEDOWN, true, UI_MOD_CTRL);
  CHECK_STR_FREE(hotkey_key_string(&e), "CTRL+PGDN");
  e = key_ev(0xe9, false, UI_MOD_NONE); /* a non-ASCII character cannot be bound */
  CHECK(hotkey_key_string(&e) == NULL);
  e = key_ev('C', false, UI_MOD_SUPER); /* the Command key is CMD+ ("SUPER+" parses too) */
  CHECK_STR_FREE(hotkey_key_string(&e), "CMD+C");
  {
    Config *c = config_get_defaults();
    CHECK(hotkey_add_key(c, HOTKEY_TOGGLE_LOG, "SUPER+c"));
    CHECK(hotkey_matches(c, &e, HOTKEY_TOGGLE_LOG));
    e = key_ev('C', false, UI_MOD_CTRL);
    CHECK(!hotkey_matches(c, &e, HOTKEY_TOGGLE_LOG)); /* Cmd is not Ctrl */
    char *d = hotkey_get_display(c, HOTKEY_TOGGLE_LOG);
    CHECK(d && strstr(d, "Cmd+C"));
    free(d);
    config_free(c);
  }
  /* what it names matches the event again */
  Config *c = config_get_defaults();
  const UiEvent evs[] = {key_ev('A', false, UI_MOD_CTRL), key_ev(UI_KEY_F12, true, UI_MOD_ALT),
                         key_ev(UI_KEY_BTAB, true, UI_MOD_NONE), key_ev('{', false, UI_MOD_NONE),
                         key_ev(' ', false, UI_MOD_NONE)};
  for (size_t i = 0; i < sizeof evs / sizeof *evs; i++) {
    char *k = hotkey_key_string(&evs[i]);
    CHECK(k && hotkey_add_key(c, HOTKEY_TOGGLE_LOG, k));
    CHECK(hotkey_matches(c, &evs[i], HOTKEY_TOGGLE_LOG));
    free(k);
  }
  config_free(c);
}

static void test_hotkey_frontend_mask(void) {
  char *err = NULL;
  Config *c = config_get_defaults();
  CHECK(c->frontend == CONFIG_FE_NCURSES);
  CHECK(config_validate(c, &err) && !err);
  /* palette (notcurses only, CTRL+P) is not this frontend's */
  CHECK(hotkey_add_key(c, HOTKEY_QUIT, "CTRL+P"));
  CHECK(config_validate(c, &err) && !err);
  CHECK(hotkey_find_conflict(c, HOTKEY_QUIT, "CTRL+P") == HOTKEY_COUNT);
  CHECK(hotkey_find_conflict(c, HOTKEY_PALETTE, "CTRL+P") == HOTKEY_COUNT);
  Config *d = config_copy(c);
  CHECK(d && d->frontend == CONFIG_FE_NCURSES);
  config_free(d);
  /* In the notcurses frontend both exist: a conflict */
  c->frontend = CONFIG_FE_NOTCURSES;
  CHECK(hotkey_find_conflict(c, HOTKEY_QUIT, "CTRL+P") == HOTKEY_PALETTE);
  CHECK(!config_validate(c, &err) && err && strstr(err, "CTRL+P"));
  free(err);
  err = NULL;
  config_free(c);
  /* ALT+3 is not 3 */
  c = config_get_defaults();
  c->frontend = CONFIG_FE_NOTCURSES;
  CHECK(hotkey_add_key(c, HOTKEY_QUIT, "3"));
  CHECK(hotkey_find_conflict(c, HOTKEY_QUIT, "3") == HOTKEY_COUNT);
  CHECK(hotkey_find_conflict(c, HOTKEY_CONN_TAB_3, "ALT+3") == HOTKEY_COUNT);
  config_free(c);
  /* The shared general ranges */
  c = config_get_defaults();
  c->general.page_size = CONFIG_PAGE_SIZE_MIN - 1;
  CHECK(!config_validate(c, &err) && err);
  free(err);
  err = NULL;
  c->general.page_size = CONFIG_PAGE_SIZE_MAX;
  CHECK(config_validate(c, &err) && !err);
  c->general.page_size = CONFIG_PAGE_SIZE_MAX + 1;
  CHECK(!config_validate(c, NULL));
  c->general.page_size = CONFIG_PAGE_SIZE_DEFAULT;
  c->general.row_numbers = 3;
  CHECK(!config_validate(c, NULL));
  c->general.row_numbers = CONFIG_ROW_NUMBERS_ALWAYS;
  c->general.color_scheme = -1;
  CHECK(!config_validate(c, NULL));
  c->general.color_scheme = 2;
  CHECK(config_validate(c, NULL));
  config_free(c);
}

static int json_to_int(cJSON *n) { return cJSON_IsNumber(n) ? (int)n->valuedouble : -1; }

static const char *FLAT_WS =
    "{\"name\":\"W\",\"current_tab\":2,\"tabs\":["
    "{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t1\","
    " \"cursor\":[5,1],\"scroll\":[3,0],"
    " \"sort\":[{\"column\":\"id\",\"direction\":2}],"
    " \"filters\":[{\"column\":\"s\",\"op\":7,\"value\":\"x\"},"
    "             {\"column\":\"\",\"raw\":true,\"op\":14,\"value\":\"a > 1\"},"
    "             {\"column\":\"n\",\"op\":9,\"value\":\"1\",\"value2\":\"9\"}],"
    " \"ui\":{\"filters_visible\":true,\"filters_focused\":true}},"
    "{\"type\":\"QUERY\",\"connection_id\":\"B\",\"query_text\":\"SELECT 1\","
    " \"query_cursor\":3,\"query_scroll_line\":1,\"query_scroll_col\":2,"
    " \"ui\":{\"query_focus_results\":true}},"
    "{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t2\",\"ui\":{}},"
    "{\"type\":\"TABLE\",\"connection_id\":\"\",\"table_name\":\"lost\",\"ui\":{}}"
    "]}";

static void test_workspace_json_forms(void) {
  char *err = NULL;
  /* Flat: two forms around a table tab, the second form current */
  cJSON *flat = cJSON_Parse(
      "{\"name\":\"F\",\"current_tab\":2,\"tabs\":["
      "{\"type\":\"FORM\",\"connection_id\":\"\"},"
      "{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t\",\"ui\":{}},"
      "{\"type\":\"FORM\",\"connection_id\":\"\"}]}");
  cJSON *n = workspace_json_to_nested(flat, &err);
  CHECK(n && !err);
  cJSON *conns = json_get_array(n, "connections");
  CHECK_SIZE(json_array_size(conns), 3); /* the two forms are not merged */
  cJSON *f0 = json_get_array_item(conns, 0);
  CHECK(json_get_bool(f0, "form", false));
  CHECK_SIZE(json_get_int(f0, "order", -1), 0);
  CHECK(!cJSON_GetObjectItem(f0, "saved_id") && !cJSON_GetObjectItem(f0, "connstr") &&
        !cJSON_GetObjectItem(f0, "tabs"));
  CHECK_STR(json_get_string(json_get_array_item(conns, 1), "saved_id", ""), "A");
  CHECK_SIZE(json_get_int(json_get_array_item(conns, 2), "order", -1), 2);
  CHECK_SIZE(json_get_int(n, "current", -1), 2); /* the current tab was the second form */
  /* And back: the same three tabs in the same order, the same current tab */
  cJSON *back = workspace_json_to_flat(n, &err);
  CHECK(back && !err);
  cJSON *tabs = json_get_array(back, "tabs");
  CHECK_SIZE(json_array_size(tabs), 3);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "type", ""), "FORM");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "connection_id", "x"), "");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "type", ""), "TABLE");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 2), "type", ""), "FORM");
  CHECK_SIZE(json_get_int(back, "current_tab", -1), 2);
  cJSON_Delete(back);
  cJSON_Delete(n);
  cJSON_Delete(flat);

  /* Nested, GUI-like: no order; "form" wins over a stray saved_id; a
   * workspace of only a form converts (not "no workspace could be converted") */
  cJSON *only = cJSON_Parse(
      "{\"version\":2,\"current\":0,\"workspaces\":[{\"name\":\"O\",\"current\":0,"
      "\"connections\":[{\"form\":true,\"saved_id\":\"A\"}]}]}");
  cJSON *v1 = session_json_to_v1(only, &err);
  CHECK(v1 && !err);
  cJSON *ws = json_get_array_item(json_get_array(v1, "workspaces"), 0);
  tabs = json_get_array(ws, "tabs");
  CHECK_SIZE(json_array_size(tabs), 1);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "type", ""), "FORM");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "connection_id", "x"), "");
  cJSON_Delete(v1);
  cJSON_Delete(only);
}

static void test_workspace_json(void) {
  char *err = NULL;
  cJSON *flat = cJSON_Parse(FLAT_WS);
  CHECK(flat && workspace_json_shape(flat) == WORKSPACE_JSON_FLAT);
  cJSON *nested = workspace_json_to_nested(flat, &err);
  CHECK(nested && !err);
  CHECK(workspace_json_shape(nested) == WORKSPACE_JSON_NESTED);
  CHECK_STR(json_get_string(nested, "name", ""), "W");
  cJSON *conns = json_get_array(nested, "connections");
  CHECK_SIZE(json_array_size(conns), 2); /* A and B; the tab without an id is dropped */
  cJSON *a = json_get_array_item(conns, 0);
  CHECK_STR(json_get_string(a, "saved_id", ""), "A");
  cJSON *atabs = json_get_array(a, "tabs");
  CHECK_SIZE(json_array_size(atabs), 2);
  cJSON *t1 = json_get_array_item(atabs, 0);
  CHECK_STR(json_get_string(t1, "type", ""), "table");
  CHECK_STR(json_get_string(t1, "table", ""), "t1");
  CHECK_SIZE(json_get_int(t1, "order", -1), 0);
  CHECK(json_get_bool(t1, "filters_visible", false));
  CHECK(json_get_bool(json_get_object(t1, "ncurses"), "filters_focused", false));
  CHECK(!json_get_bool(json_get_object(t1, "ncurses"), "query_focus_results", true));
  cJSON *sorts = json_get_array(t1, "sorts");
  CHECK(json_get_bool(json_get_array_item(sorts, 0), "desc", false));
  cJSON *filters = json_get_array(t1, "filters");
  CHECK_STR(json_get_string(json_get_array_item(filters, 0), "op", ""), "contains");
  CHECK_STR(json_get_string(json_get_array_item(filters, 1), "op", ""), "raw");
  CHECK(cJSON_GetObjectItem(json_get_array_item(filters, 1), "column") == NULL);
  CHECK_STR(json_get_string(json_get_array_item(filters, 2), "op", ""), "between");
  CHECK_STR(json_get_string(json_get_array_item(filters, 2), "value2", ""), "9");
  cJSON *cur = json_get_array(t1, "cursor");
  CHECK_SIZE(json_to_int(json_get_array_item(cur, 0)), 5);
  cJSON *t2 = json_get_array_item(atabs, 1);
  CHECK_SIZE(json_get_int(t2, "order", -1), 2);
  /* current_tab 2 was t2 of A: A is current, and t2 within it */
  CHECK_SIZE(json_get_int(nested, "current", -1), 0);
  CHECK_SIZE(json_get_int(a, "current", -1), 1);
  cJSON *b = json_get_array_item(conns, 1);
  cJSON *q = json_get_array_item(json_get_array(b, "tabs"), 0);
  CHECK_STR(json_get_string(q, "type", ""), "query");
  CHECK_STR(json_get_string(q, "text", ""), "SELECT 1");
  CHECK_SIZE(json_get_int(q, "cursor", -1), 3);
  CHECK_SIZE(json_get_int(q, "order", -1), 1);
  CHECK(json_get_bool(json_get_object(q, "ncurses"), "query_focus_results", false));

  /* Back to flat: interleaving restored by order, current tab the same */
  cJSON *back = workspace_json_to_flat(nested, &err);
  CHECK(back && !err);
  cJSON *tabs = json_get_array(back, "tabs");
  CHECK_SIZE(json_array_size(tabs), 3);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "connection_id", ""), "A");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "connection_id", ""), "B");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "type", ""), "QUERY");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "query_text", ""), "SELECT 1");
  CHECK_SIZE(json_get_int(json_get_array_item(tabs, 1), "query_scroll_col", -1), 2);
  CHECK(json_get_bool(json_get_object(json_get_array_item(tabs, 0), "ui"), "filters_focused", false));
  CHECK(json_get_bool(json_get_object(json_get_array_item(tabs, 0), "ui"), "filters_visible", false));
  CHECK(json_get_bool(json_get_object(json_get_array_item(tabs, 1), "ui"), "query_focus_results", false));
  CHECK(!json_get_bool(json_get_object(json_get_array_item(tabs, 2), "ui"), "filters_focused", true));
  CHECK_STR(json_get_string(json_get_array_item(tabs, 2), "table_name", ""), "t2");
  CHECK_SIZE(json_get_int(back, "current_tab", -1), 2);
  cJSON *f0 = json_get_array_item(json_get_array(json_get_array_item(tabs, 0), "filters"), 0);
  CHECK_SIZE(json_get_int(f0, "op", -1), FILTER_OP_CONTAINS);
  cJSON *f1 = json_get_array_item(json_get_array(json_get_array_item(tabs, 0), "filters"), 1);
  CHECK(json_get_bool(f1, "raw", false) && json_get_int(f1, "op", -1) == FILTER_OP_RAW);
  cJSON *s0 = json_get_array_item(json_get_array(json_get_array_item(tabs, 0), "sort"), 0);
  CHECK_SIZE(json_get_int(s0, "direction", -1), 2);
  cJSON_Delete(back);

  /* A GUI-written workspace: no order, a connstr-only connection, the new ops */
  cJSON *gui = cJSON_Parse(
      "{\"name\":\"G\",\"connections\":[{\"connstr\":\"sqlite:///x.db\",\"name\":\"x\","
      "\"tabs\":[{\"type\":\"table\",\"table\":\"t\",\"filters\":[{\"column\":\"s\",\"op\":\"starts\",\"value\":\"a\"}],\"sorts\":[]}],"
      "\"current\":0},{\"saved_id\":\"B\",\"tabs\":[{\"type\":\"query\",\"text\":\"x\"}],\"current\":0}],\"current\":1}");
  back = workspace_json_to_flat(gui, &err);
  CHECK(back && !err);
  tabs = json_get_array(back, "tabs");
  CHECK_SIZE(json_array_size(tabs), 2);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "connection_id", ""), "");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "connstr", ""), "sqlite:///x.db");
  CHECK_SIZE(json_get_int(json_get_array_item(json_get_array(json_get_array_item(tabs, 0), "filters"), 0), "op", -1), FILTER_OP_STARTS);
  CHECK_SIZE(json_get_int(back, "current_tab", -1), 1);
  cJSON_Delete(back);
  cJSON_Delete(gui);

  /* An op key nobody knows fails the conversion with a message */
  cJSON *bad = cJSON_Parse("{\"connections\":[{\"saved_id\":\"A\",\"tabs\":[{\"type\":\"table\",\"table\":\"t\","
                           "\"filters\":[{\"column\":\"s\",\"op\":\"someday\",\"value\":\"a\"}]}]}]}");
  back = workspace_json_to_flat(bad, &err);
  CHECK(!back && err && strstr(err, "someday"));
  free(err); err = NULL;
  cJSON_Delete(bad);

  /* A CONNECTION tab as the current tab makes its connection current */
  cJSON *ct = cJSON_Parse(
      "{\"name\":\"C\",\"current_tab\":2,\"tabs\":["
      "{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t\",\"ui\":{}},"
      "{\"type\":\"TABLE\",\"connection_id\":\"B\",\"table_name\":\"u\",\"ui\":{}},"
      "{\"type\":\"CONNECTION\",\"connection_id\":\"B\"}]}");
  cJSON *cn = workspace_json_to_nested(ct, &err);
  CHECK(cn && !err);
  CHECK_SIZE(json_get_int(cn, "current", -1), 1);
  /* B's CONNECTION tab is no view: B holds only its table tab, and the
   * connection tab's position */
  cJSON *cb = json_get_array_item(json_get_array(cn, "connections"), 1);
  CHECK_SIZE(json_array_size(json_get_array(cb, "tabs")), 1);
  CHECK_SIZE(json_get_int(cb, "order", -1), 2);
  cJSON_Delete(cn);
  cJSON_Delete(ct);

  /* An unknown operator integer fails, with or without an error slot */
  cJSON *op99 = cJSON_Parse(
      "{\"tabs\":[{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t\","
      "\"filters\":[{\"column\":\"s\",\"op\":99,\"value\":\"a\"}]}]}");
  CHECK(!workspace_json_to_nested(op99, &err) && err && strstr(err, "99"));
  free(err); err = NULL;
  CHECK(!workspace_json_to_nested(op99, NULL));
  cJSON_Delete(op99);

  /* A sort without a direction is dropped */
  cJSON *nosort = cJSON_Parse(
      "{\"tabs\":[{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t\","
      "\"sort\":[{\"column\":\"id\",\"direction\":0}]}]}");
  cJSON *ns = workspace_json_to_nested(nosort, &err);
  CHECK(ns && !err);
  CHECK_SIZE(json_array_size(json_get_array(json_get_array_item(json_get_array(
                 json_get_array_item(json_get_array(ns, "connections"), 0), "tabs"), 0), "sorts")), 0);
  cJSON_Delete(ns);
  cJSON_Delete(nosort);

  /* A connection with only a connection tab survives the round trip */
  cJSON *only = cJSON_Parse(
      "{\"name\":\"O\",\"current_tab\":1,\"tabs\":["
      "{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t\",\"ui\":{}},"
      "{\"type\":\"CONNECTION\",\"connection_id\":\"B\"},"
      "{\"type\":\"QUERY\",\"connection_id\":\"A\",\"query_text\":\"q\",\"ui\":{}}]}");
  cJSON *on = workspace_json_to_nested(only, &err);
  CHECK(on && !err);
  cJSON *oconns = json_get_array(on, "connections");
  CHECK_SIZE(json_array_size(oconns), 2);
  cJSON *ob = json_get_array_item(oconns, 1);
  CHECK_STR(json_get_string(ob, "saved_id", ""), "B");
  CHECK(json_get_array(ob, "tabs") != NULL);
  CHECK_SIZE(json_array_size(json_get_array(ob, "tabs")), 0);
  CHECK_SIZE(json_get_int(ob, "order", -1), 1);
  CHECK(cJSON_GetObjectItem(json_get_array_item(oconns, 0), "order") == NULL);
  CHECK_SIZE(json_get_int(on, "current", -1), 1);
  back = workspace_json_to_flat(on, &err);
  CHECK(back && !err);
  tabs = json_get_array(back, "tabs");
  CHECK_SIZE(json_array_size(tabs), 3);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "type", ""), "TABLE");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "type", ""), "CONNECTION");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "connection_id", ""), "B");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 2), "type", ""), "QUERY");
  CHECK_SIZE(json_get_int(back, "current_tab", -1), 1);
  cJSON_Delete(back);
  cJSON_Delete(on);
  cJSON_Delete(only);

  /* A GUI connection with no tabs and no order: a connection tab after the
   * others; the current one */
  cJSON *gnt = cJSON_Parse(
      "{\"connections\":[{\"saved_id\":\"A\",\"tabs\":[{\"type\":\"table\",\"table\":\"t\"}]},"
      "{\"connstr\":\"sqlite:///g.db\",\"tabs\":[]}],\"current\":1}");
  back = workspace_json_to_flat(gnt, &err);
  CHECK(back && !err);
  tabs = json_get_array(back, "tabs");
  CHECK_SIZE(json_array_size(tabs), 2);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "type", ""), "CONNECTION");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "connstr", ""), "sqlite:///g.db");
  CHECK_SIZE(json_get_int(back, "current_tab", -1), 1);
  cJSON_Delete(back);
  cJSON_Delete(gnt);

  /* Tabs of a connection opened by URL: grouped by connstr, and back */
  cJSON *url = cJSON_Parse(
      "{\"name\":\"U\",\"current_tab\":1,\"tabs\":["
      "{\"type\":\"TABLE\",\"connection_id\":\"\",\"connstr\":\"sqlite:///u.db\",\"table_name\":\"t\",\"ui\":{}},"
      "{\"type\":\"QUERY\",\"connstr\":\"sqlite:///v.db\",\"query_text\":\"q\",\"ui\":{}},"
      "{\"type\":\"TABLE\",\"connection_id\":\"\",\"connstr\":\"sqlite:///u.db\",\"table_name\":\"w\",\"ui\":{}},"
      "{\"type\":\"TABLE\",\"connection_id\":\"\",\"table_name\":\"gone\",\"ui\":{}}]}");
  cJSON *un = workspace_json_to_nested(url, &err);
  CHECK(un && !err);
  cJSON *uconns = json_get_array(un, "connections");
  CHECK_SIZE(json_array_size(uconns), 2);
  cJSON *u0 = json_get_array_item(uconns, 0);
  CHECK_STR(json_get_string(u0, "connstr", ""), "sqlite:///u.db");
  CHECK(cJSON_GetObjectItem(u0, "saved_id") == NULL);
  CHECK_SIZE(json_array_size(json_get_array(u0, "tabs")), 2);
  CHECK_STR(json_get_string(json_get_array_item(uconns, 1), "connstr", ""), "sqlite:///v.db");
  CHECK_SIZE(json_get_int(un, "current", -1), 1);
  back = workspace_json_to_flat(un, &err);
  CHECK(back && !err);
  tabs = json_get_array(back, "tabs");
  CHECK_SIZE(json_array_size(tabs), 3);
  CHECK_STR(json_get_string(json_get_array_item(tabs, 0), "connstr", ""), "sqlite:///u.db");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 1), "connstr", ""), "sqlite:///v.db");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 2), "table_name", ""), "w");
  CHECK_STR(json_get_string(json_get_array_item(tabs, 2), "connection_id", "x"), "");
  CHECK_SIZE(json_get_int(back, "current_tab", -1), 1);
  cJSON_Delete(back);
  cJSON_Delete(un);
  cJSON_Delete(url);

  /* An empty saved_id is no saved_id: the connstr applies */
  cJSON *esid = cJSON_Parse(
      "{\"connections\":[{\"saved_id\":\"\",\"connstr\":\"sqlite:///e.db\","
      "\"tabs\":[{\"type\":\"query\",\"text\":\"x\"}]}]}");
  back = workspace_json_to_flat(esid, &err);
  CHECK(back && !err);
  CHECK_STR(json_get_string(json_get_array_item(json_get_array(back, "tabs"), 0), "connstr", ""), "sqlite:///e.db");
  cJSON_Delete(back);
  cJSON_Delete(esid);

  /* A workspace that does not convert is skipped, with a warning; the others stay */
  cJSON *mixed = cJSON_Parse(
      "{\"version\":2,\"current\":1,\"gtk\":{\"x\":1},\"workspaces\":["
      "{\"name\":\"Good\",\"connections\":[{\"saved_id\":\"A\",\"tabs\":[{\"type\":\"table\",\"table\":\"t\"}]}]},"
      "{\"name\":\"Bad\",\"connections\":[{\"saved_id\":\"A\",\"tabs\":[{\"type\":\"table\",\"table\":\"t\","
      "\"filters\":[{\"column\":\"s\",\"op\":\"someday\",\"value\":\"a\"}]}]}]}]}");
  cJSON *mv1 = session_json_to_v1(mixed, &err);
  CHECK(mv1 != NULL);
  CHECK_SIZE(json_array_size(json_get_array(mv1, "workspaces")), 1);
  CHECK_SIZE(json_get_int(mv1, "current_workspace", -1), 0);
  CHECK(err && strstr(err, "someday") && strstr(err, "Bad"));
  CHECK(cJSON_GetObjectItem(mv1, "gtk") != NULL);
  free(err); err = NULL;
  cJSON_Delete(mv1);
  mv1 = session_json_to_v1(mixed, NULL);
  CHECK(mv1 && json_array_size(json_get_array(mv1, "workspaces")) == 1);
  cJSON_Delete(mv1);
  /* Every workspace bad: an error */
  cJSON_DeleteItemFromArray(json_get_array(mixed, "workspaces"), 0);
  CHECK(!session_json_to_v1(mixed, &err) && err && strstr(err, "someday"));
  free(err); err = NULL;
  cJSON_Delete(mixed);
  /* No workspaces at all is no error */
  cJSON *none = cJSON_Parse("{\"version\":2,\"workspaces\":[],\"gtk\":{}}");
  cJSON *nv1 = session_json_to_v1(none, &err);
  CHECK(nv1 && !err && json_array_size(json_get_array(nv1, "workspaces")) == 0);
  cJSON_Delete(nv1);
  cJSON_Delete(none);
  /* The same for v1 -> v2; the current one later than the skipped one moves down */
  char *mtext = str_printf(
      "{\"current_workspace\":1,\"workspaces\":["
      "{\"tabs\":[{\"type\":\"TABLE\",\"connection_id\":\"A\",\"table_name\":\"t\","
      "\"filters\":[{\"column\":\"s\",\"op\":99,\"value\":\"a\"}]}]},%s]}", FLAT_WS);
  cJSON *mixed1 = cJSON_Parse(mtext);
  free(mtext);
  cJSON *mv2 = session_json_to_v2(mixed1, &err);
  CHECK(mv2 && err && strstr(err, "99") && strstr(err, "Workspace 1"));
  CHECK_SIZE(json_array_size(json_get_array(mv2, "workspaces")), 1);
  CHECK_SIZE(json_get_int(mv2, "current", -1), 0);
  CHECK_STR(json_get_string(json_get_array_item(json_get_array(mv2, "workspaces"), 0), "name", ""), "W");
  free(err); err = NULL;
  cJSON_Delete(mv2);
  cJSON_Delete(mixed1);

  /* Whole-session roots */
  char *v1_text = str_printf("{\"settings\":{\"sidebar_visible\":false},\"workspaces\":[%s],\"current_workspace\":0}", FLAT_WS);
  cJSON *v1 = cJSON_Parse(v1_text);
  free(v1_text);
  cJSON *v2 = session_json_to_v2(v1, &err);
  CHECK(v2 && !err && json_get_int(v2, "version", 0) == 2);
  CHECK(!json_get_bool(json_get_object(v2, "ncurses"), "sidebar_visible", true));
  CHECK(cJSON_GetObjectItem(v2, "settings") == NULL);
  CHECK_SIZE(json_get_int(v2, "current", -1), 0);
  CHECK(workspace_json_shape(json_get_array_item(json_get_array(v2, "workspaces"), 0)) == WORKSPACE_JSON_NESTED);
  cJSON_AddBoolToObject(v2, "other_frontend_flag", true);
  cJSON *v1b = session_json_to_v1(v2, &err);
  CHECK(v1b && !err);
  CHECK(!json_get_bool(json_get_object(v1b, "settings"), "sidebar_visible", true));
  CHECK_SIZE(json_get_int(v1b, "current_workspace", -1), 0);
  CHECK(workspace_json_shape(json_get_array_item(json_get_array(v1b, "workspaces"), 0)) == WORKSPACE_JSON_FLAT);
  /* to_v2 of a v2 root is a copy */
  cJSON *v2b = session_json_to_v2(v2, &err);
  CHECK(v2b && json_get_int(v2b, "version", 0) == 2 && json_get_bool(v2b, "other_frontend_flag", false));
  cJSON_Delete(v2b);
  cJSON_Delete(v1b);
  cJSON_Delete(v2);
  cJSON_Delete(v1);
  cJSON_Delete(nested);
  cJSON_Delete(flat);
}

static void test_session_connstr_password(void) {
  /* A tunnelled string keeps its +ssh form, SSH host/user and key */
  char *a = session_connstr_with_password(
      "postgres+ssh://su@sh:22/u@dbh:5432/d?ssh_key=%2Fk%2Fid", "p", NULL);
  CHECK(a != NULL);
  if (a) {
    CHECK(strncmp(a, "postgres+ssh://", 15) == 0);
    CHECK(strstr(a, "su@sh:22") != NULL);
    CHECK(strstr(a, "u:p@dbh:5432") != NULL);
    CHECK(strstr(a, "ssh_key=%2Fk%2Fid") != NULL);
    LaceSavedConnection *m = lace_saved_conn_from_connstr(
        "postgres+ssh://su@sh:22/u@dbh:5432/d?ssh_key=%2Fk%2Fid", NULL);
    char *want = m ? lace_saved_conn_build_connstr(m, "p", NULL) : NULL;
    CHECK(want && strcmp(a, want) == 0);
    str_secure_free(want);
    lace_saved_conn_free(m);
  }
  str_secure_free(a);

  char *b = session_connstr_with_password("postgres://u@h/d", "p", NULL);
  CHECK(b != NULL);
  if (b)
    CHECK(strstr(b, "u:p@h") != NULL && strstr(b, "+ssh") == NULL);
  str_secure_free(b);
  CHECK(session_connstr_with_password(NULL, "p", NULL) == NULL);
}

int main(void) {
  test_recent_passwords();
  test_session_connstr_password();
  test_config_file();
  test_config_connection_tab_keys();
  test_hotkeys_connection_tab();
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
  test_connstr_display();
  test_row_key();
  test_hotkey_parse();
  test_hotkey_frontend_mask();
  test_hotkey_key_string();
  test_workspace_json();
  test_workspace_json_forms();
  return test_report("tui");
}
