/*
 * Lace
 * liblace unit tests: SQL quoting and literals, connection strings
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "test.h"
#include "lace.h"
#include "util/connstr.h"
#include "util/str.h"
#include "util/sha256.h"
#include "config/saved_connection.h"
#include "../liblace/src/client_internal.h"
#include "../liblace/src/rpc.h"
#include <cjson/cJSON.h>
#include <math.h>

static void test_quote_identifier(void) {
  CHECK_STR_FREE(lace_quote_identifier(LACE_DRIVER_SQLITE, "name"), "\"name\"");
  CHECK_STR_FREE(lace_quote_identifier(LACE_DRIVER_POSTGRES, "a\"b"),
                 "\"a\"\"b\"");
  CHECK_STR_FREE(lace_quote_identifier(LACE_DRIVER_MYSQL, "a`b"), "`a``b`");
  CHECK_STR_FREE(lace_quote_identifier(LACE_DRIVER_MARIADB, "x"), "`x`");
}

static void test_quote_table(void) {
  /* A reference is the plain name everywhere but PostgreSQL */
  CHECK_STR_FREE(lace_quote_table(LACE_DRIVER_SQLITE, "a.b"), "\"a.b\"");
  CHECK_STR_FREE(lace_quote_table(LACE_DRIVER_MYSQL, "orders"), "`orders`");
  /* PostgreSQL: schema.table, a part quoted when it holds '.' or '"' */
  CHECK_STR_FREE(lace_quote_table(LACE_DRIVER_POSTGRES, "users"), "\"users\"");
  CHECK_STR_FREE(lace_quote_table(LACE_DRIVER_POSTGRES, "Sales.orders"),
                 "\"Sales\".\"orders\"");
  CHECK_STR_FREE(lace_quote_table(LACE_DRIVER_POSTGRES, "Sales.\"odd.name\""),
                 "\"Sales\".\"odd.name\"");
}

static void test_string_literals(void) {
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_SQLITE, "it's"),
                 "'it''s'");
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_SQLITE, "C:\\x"),
                 "'C:\\x'");
  /* PostgreSQL: E'' whenever a backslash is present, so the meaning does not
   * depend on standard_conforming_strings */
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_POSTGRES, "plain"),
                 "'plain'");
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_POSTGRES, "a\\'b"),
                 "E'a\\\\''b'");
  /* MySQL: backslash text goes hex, right in every sql_mode */
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_MYSQL, "a\\b"),
                 "_utf8mb4 X'615c62'");
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_MYSQL, "it's"),
                 "'it''s'");
  CHECK_STR_FREE(lace_sql_string_literal(LACE_DRIVER_SQLITE, NULL), "NULL");
}

static void test_value_literals(void) {
  LaceValue v = lace_value_int(-42);
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_SQLITE), "-42");

  v = lace_value_float(0.1);
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_POSTGRES), "0.1");

  v = lace_value_bool(true);
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_SQLITE), "1");
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_POSTGRES), "TRUE");

  const uint8_t bytes[] = {0xde, 0xad};
  v = lace_value_blob(bytes, sizeof(bytes));
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_SQLITE), "X'dead'");
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_POSTGRES),
                 "E'\\\\xdead'::bytea");
  lace_value_free(&v);

  v = lace_value_null();
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_MYSQL), "NULL");

  /* A value too large to load is only a label: never a literal */
  v = lace_value_text("[TEXT: 5000000 bytes]");
  v.omitted = 5000000;
  CHECK_STR_FREE(lace_value_to_sql_literal(&v, LACE_DRIVER_SQLITE), NULL);
  lace_value_free(&v);
}

static void test_format_double(void) {
  char buf[64];
  lace_format_double(0.1, buf, sizeof(buf));
  CHECK(strcmp(buf, "0.1") == 0);
  lace_format_double(1.0 / 3.0, buf, sizeof(buf));
  CHECK(strtod(buf, NULL) == 1.0 / 3.0); /* Round trips */
}

static void test_connstr(void) {
  char *err = NULL;
  ConnString *cs = connstr_parse("postgres://bob:p%40ss@db.local:6543/app", &err);
  CHECK(cs != NULL);
  if (cs) {
    CHECK(strcmp(cs->driver, "postgres") == 0);
    CHECK(strcmp(cs->user, "bob") == 0);
    CHECK(strcmp(cs->password, "p@ss") == 0);
    CHECK(strcmp(cs->host, "db.local") == 0);
    CHECK(cs->port == 6543);
    CHECK(strcmp(cs->database, "app") == 0);
    CHECK(!cs->ssh);
    connstr_free(cs);
  }
  free(err);

  err = NULL;
  cs = connstr_parse("postgres+ssh://sshu:sshp@jump:2222/dbu:p%2Fw%40d@inner:5432/db",
                     &err);
  CHECK(cs != NULL);
  if (cs) {
    CHECK(cs->ssh);
    CHECK(strcmp(cs->ssh_user, "sshu") == 0);
    CHECK(strcmp(cs->ssh_host, "jump") == 0);
    CHECK(cs->ssh_port == 2222);
    CHECK(strcmp(cs->user, "dbu") == 0);
    CHECK(strcmp(cs->password, "p/w@d") == 0); /* Encoded '/' and '@' survive */
    CHECK(strcmp(cs->host, "inner") == 0);
    CHECK(strcmp(cs->database, "db") == 0);
    connstr_free(cs);
  }
  free(err);

  /* '+' in userinfo is a plus, not a space; a '?' in an SQLite path
   * arrives encoded and comes back exactly; bad escapes stay as written */
  err = NULL;
  cs = connstr_parse("postgres://u:pa+ss@h/db%2Bx", &err);
  CHECK(cs && strcmp(cs->password, "pa+ss") == 0 && strcmp(cs->database, "db+x") == 0);
  connstr_free(cs);
  char *enc = connstr_encode_path("/tmp/a?b%c#d");
  CHECK_STR_FREE(str_printf("sqlite://%s?create=1", enc), "sqlite:///tmp/a%3Fb%25c%23d?create=1");
  char *url = str_printf("sqlite://%s?create=1", enc);
  free(enc);
  cs = connstr_parse(url, &err);
  CHECK(cs && strcmp(cs->database, "/tmp/a?b%c#d") == 0 &&
        connstr_get_option(cs, "create") && strcmp(connstr_get_option(cs, "create"), "1") == 0);
  connstr_free(cs);
  free(url);
  CHECK_STR_FREE(str_url_decode("a%2xb%00c% fd%41"), "a%2xb%00c% fdA");

  err = NULL;
  cs = connstr_parse("not a url", &err);
  CHECK(cs == NULL && err != NULL);
  free(err);
}

static void test_error_reporting(void) {
  /* A bad argument: the code and a message for it */
  CHECK(lace_exec(NULL, 1, "SELECT 1", NULL) == LACE_ERR_INVALID_PARAMS);
  CHECK(strcmp(lace_client_error(NULL), "Invalid client") == 0);

  /* A client that reached no daemon: CONNECTION_CLOSED from every call,
   * with its own message (not a stale one from the previous failure) */
  LaceClientConfig cfg = {0};
  cfg.mode = LACE_CONN_UNIX;
  cfg.socket_path = "/nonexistent/lace-test.sock";
  cfg.connect_timeout_ms = 100;
  lace_client_t *c = lace_client_create_with_config(&cfg);
  CHECK(c != NULL && !lace_client_connected(c));
  if (c) {
    LaceResult *r = (LaceResult *)1;
    CHECK(lace_exec(c, 1, "SELECT 1", &r) == LACE_ERR_CONNECTION_CLOSED);
    CHECK(r == NULL); /* Never left pointing at garbage */
    CHECK(strstr(lace_client_error(c), "Not connected") != NULL);
    CHECK(lace_exec(c, 1, NULL, NULL) == LACE_ERR_INVALID_PARAMS);
    CHECK(strstr(lace_client_error(c), "Not connected") == NULL);
    CHECK(lace_ping(c) == LACE_ERR_CONNECTION_CLOSED);
    lace_client_destroy(c);
  }
}

static void test_type_from_name(void) {
  CHECK(lace_type_from_name("double precision", LACE_DRIVER_POSTGRES) == LACE_TYPE_FLOAT);
  CHECK(lace_type_from_name("character varying(40)", LACE_DRIVER_POSTGRES) == LACE_TYPE_TEXT);
  CHECK(lace_type_from_name("numeric(10,2)", LACE_DRIVER_POSTGRES) == LACE_TYPE_DECIMAL);
  CHECK(lace_type_from_name("bytea", LACE_DRIVER_POSTGRES) == LACE_TYPE_BLOB);
  CHECK(lace_type_from_name("interval", LACE_DRIVER_POSTGRES) == LACE_TYPE_TEXT);
  CHECK(lace_type_from_name("timestamp with time zone", LACE_DRIVER_POSTGRES) == LACE_TYPE_TIMESTAMP);
  CHECK(lace_type_from_name("int(10) unsigned", LACE_DRIVER_MYSQL) == LACE_TYPE_INT);
  CHECK(lace_type_from_name("varbinary(16)", LACE_DRIVER_MYSQL) == LACE_TYPE_BLOB);
  CHECK(lace_type_from_name("bit(8)", LACE_DRIVER_MYSQL) == LACE_TYPE_BLOB);
  CHECK(lace_type_from_name("decimal(40,10)", LACE_DRIVER_MARIADB) == LACE_TYPE_DECIMAL);
  CHECK(lace_type_from_name("VARCHAR(20)", LACE_DRIVER_SQLITE) == LACE_TYPE_TEXT);
  CHECK(lace_type_from_name("BIGINT", LACE_DRIVER_SQLITE) == LACE_TYPE_INT);
  CHECK(lace_type_from_name("REAL", LACE_DRIVER_SQLITE) == LACE_TYPE_FLOAT);
  CHECK(lace_type_from_name("", LACE_DRIVER_SQLITE) == LACE_TYPE_TEXT);
}

static void test_is_rollback(void) {
  CHECK(lace_sql_is_rollback("ROLLBACK"));
  CHECK(lace_sql_is_rollback("  rollback work ; -- done\n"));
  CHECK(lace_sql_is_rollback("/* x */ ABORT TRANSACTION"));
  CHECK(!lace_sql_is_rollback("ROLLBACK TO SAVEPOINT lace_page"));
  CHECK(!lace_sql_is_rollback("ROLLBACK AND CHAIN"));
  CHECK(!lace_sql_is_rollback("ROLLBACK PREPARED 'x'"));
  CHECK(!lace_sql_is_rollback("ROLLBACK; INSERT INTO t VALUES (1)"));
  CHECK(!lace_sql_is_rollback("ROLLBACKS"));
  CHECK(!lace_sql_is_rollback("SELECT 1"));
}

static void test_keyset(void) {
  /* t(id PK, name, score REAL): ordered by name DESC, then the key */
  LaceColumn cols[3] = {{0}};
  cols[0].name = "id";
  cols[0].type = LACE_TYPE_INT;
  cols[0].primary_key = true;
  cols[1].name = "name";
  cols[1].type = LACE_TYPE_TEXT;
  cols[1].nullable = true;
  cols[2].name = "score";
  cols[2].type = LACE_TYPE_FLOAT;
  LaceSchema schema = {0};
  schema.columns = cols;
  schema.num_columns = 3;

  LaceSort by_name = {1, LACE_SORT_DESC};
  LaceKeysetState *ks = lace_keyset_create(&schema, &by_name, 1, LACE_DRIVER_SQLITE);
  CHECK(ks != NULL && lace_keyset_num_columns(ks) == 2);
  CHECK_STR_FREE(lace_keyset_build_order(ks, true), "\"name\" DESC, \"id\" ASC");
  CHECK_STR_FREE(lace_keyset_build_order(ks, false), "\"name\" ASC, \"id\" DESC");

  /* A result whose columns are in another order than the schema's */
  LaceColumn rcols[2] = {{0}};
  rcols[0].name = "name";
  rcols[1].name = "id";
  LaceValue cells[2] = {lace_value_text("bo'b"), lace_value_int(7)};
  LaceRow row = {cells, 2};
  LaceResult rs = {0};
  rs.columns = rcols;
  rs.num_columns = 2;
  rs.rows = &row;
  rs.num_rows = 1;
  /* SQLite: NULLs first for ASC, so last for DESC: after 'bo''b' going
   * down come smaller names and then the NULLs */
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, true),
                 "(((\"name\" < 'bo''b' OR \"name\" IS NULL)) OR "
                 "(\"name\" = 'bo''b' AND \"id\" > 7))");
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, false),
                 "((\"name\" > 'bo''b') OR (\"name\" = 'bo''b' AND \"id\" < 7))");

  /* A NULL boundary: on SQLite (NULLs last for DESC) nothing sorts after
   * it in that column, only later keys among the NULLs */
  lace_value_free(&cells[0]);
  cells[0] = lace_value_null();
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, true),
                 "((1 = 0) OR (\"name\" IS NULL AND \"id\" > 7))");

  /* A value too large to load cannot be a boundary */
  cells[0] = lace_value_text("[TEXT]");
  cells[0].omitted = 5000000;
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, true), NULL);
  lace_value_free(&cells[0]);

  /* A key column missing from the result: no boundary either */
  rs.num_columns = 1;
  cells[0] = lace_value_text("x");
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, true), NULL);
  lace_value_free(&cells[0]);
  lace_keyset_free(ks);

  /* PostgreSQL: NULLs last for ASC */
  LaceSort by_name_asc = {1, LACE_SORT_ASC};
  ks = lace_keyset_create(&schema, &by_name_asc, 1, LACE_DRIVER_POSTGRES);
  rs.num_columns = 2;
  cells[0] = lace_value_text("m");
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, true),
                 "(((\"name\" > 'm' OR \"name\" IS NULL)) OR "
                 "(\"name\" = 'm' AND \"id\" > 7))");
  lace_value_free(&cells[0]);
  lace_keyset_free(ks);

  /* NOT NULL columns going the same way: a row comparison (one index
   * range), except on MySQL */
  cols[1].nullable = false;
  ks = lace_keyset_create(&schema, &by_name_asc, 1, LACE_DRIVER_POSTGRES);
  cells[0] = lace_value_text("m");
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, false),
                 "(\"name\", \"id\") < ('m', 7)");
  lace_keyset_free(ks);
  ks = lace_keyset_create(&schema, &by_name_asc, 1, LACE_DRIVER_MYSQL);
  CHECK_STR_FREE(lace_keyset_build_where(ks, &rs, 0, true),
                 "((`name` > 'm') OR (`name` = 'm' AND `id` > 7))");
  lace_value_free(&cells[0]);
  lace_keyset_free(ks);
  cols[1].nullable = true;

  /* A float sort column: exact on SQLite (double), not on PostgreSQL or
   * MySQL (single-precision real/FLOAT) */
  LaceSort by_score = {2, LACE_SORT_ASC};
  ks = lace_keyset_create(&schema, &by_score, 1, LACE_DRIVER_SQLITE);
  CHECK(ks != NULL);
  lace_keyset_free(ks);
  CHECK(lace_keyset_create(&schema, &by_score, 1, LACE_DRIVER_POSTGRES) == NULL);
  CHECK(lace_keyset_create(&schema, &by_score, 1, LACE_DRIVER_MYSQL) == NULL);

  /* No primary key: no keyset */
  cols[0].primary_key = false;
  CHECK(lace_keyset_create(&schema, NULL, 0, LACE_DRIVER_SQLITE) == NULL);
}

static void sha_hex(const uint8_t *d, char *out) {
  for (int i = 0; i < LACE_SHA256_LEN; i++)
    sprintf(out + 2 * i, "%02x", d[i]);
}

/* FIPS 180-4 and RFC 4231 vectors; the handshake proof is an HMAC */
static void test_sha256(void) {
  uint8_t d[LACE_SHA256_LEN];
  char hex[2 * LACE_SHA256_LEN + 1];
  lace_sha256("", 0, d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  lace_sha256("abc", 3, d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  lace_sha256(two, strlen(two), d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  /* A million 'a': many blocks through the buffer */
  char *million = malloc(1000000);
  memset(million, 'a', 1000000);
  lace_sha256(million, 1000000, d);
  free(million);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  lace_hmac_sha256(key, sizeof(key), "Hi There", 8, d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  lace_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  /* Key longer than a block is hashed first */
  uint8_t big[131];
  memset(big, 0xaa, sizeof(big));
  const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
  lace_hmac_sha256(big, sizeof(big), msg, strlen(msg), d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(hex), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

  /* The proof is HMAC(token, label ":" Nc ":" Nd) */
  char proof[2 * LACE_SHA256_LEN + 1];
  lace_auth_proof("Jefe", "what do ya want", "for", "nothing?", proof);
  lace_hmac_sha256("Jefe", 4, "what do ya want:for:nothing?", 28, d);
  sha_hex(d, hex);
  CHECK_STR_FREE(str_dup(proof), hex);
  char other[2 * LACE_SHA256_LEN + 1];
  lace_auth_proof("Jefe", "lace-client", "for", "nothing?", other);
  CHECK(!lace_secure_equal(proof, other));
  CHECK(lace_secure_equal(proof, proof));
  CHECK(!lace_secure_equal("abc", "abcd"));
}

/* Numbers from the wire are range-checked before they become integers
 * (found by fuzzing: a huge "bytes" was converted to size_t unchecked) */
static void test_wire_numbers(void) {
  const char *cases[] = {"{\"$omitted\":\"TEXT\",\"bytes\":2e67}",
                         "{\"$omitted\":\"TEXT\",\"bytes\":-5}",
                         "{\"$omitted\":\"TEXT\",\"bytes\":1.5}"};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    cJSON *j = cJSON_Parse(cases[i]);
    LaceValue v = {0};
    CHECK(lace_rpc_parse_value(j, &v) && v.omitted == 1);
    lace_value_free(&v);
    cJSON_Delete(j);
  }
  cJSON *j = cJSON_Parse("{\"$omitted\":\"TEXT\",\"bytes\":2000000}");
  LaceValue v = {0};
  CHECK(lace_rpc_parse_value(j, &v) && v.omitted == 2000000);
  lace_value_free(&v);
  cJSON_Delete(j);
  j = cJSON_Parse("1e300");
  CHECK(lace_rpc_parse_value(j, &v) && v.type == LACE_TYPE_FLOAT);
  cJSON_Delete(j);
  j = cJSON_Parse("-9223372036854775808");
  CHECK(lace_rpc_parse_value(j, &v) && v.type == LACE_TYPE_INT && v.int_val == INT64_MIN);
  cJSON_Delete(j);
}

/* The lexer agrees with the servers on the corners that decide statement
 * boundaries, and the read-only test that guards row-limit cancels */
static void test_lexer_corners(void) {
  /* PostgreSQL E'...' has backslash escapes: the ';' is inside the string */
  CHECK(!lace_sql_single_statement("INSERT INTO t VALUES (E'\\''); SELECT 1",
                                   LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_single_statement("SELECT E'a\\';b'", LACE_DRIVER_POSTGRES));
  /* A plain PostgreSQL string has none */
  CHECK(lace_sql_single_statement("SELECT 'a\\'", LACE_DRIVER_POSTGRES));
  /* MySQL: "--" starts a comment only before whitespace */
  CHECK(!lace_sql_single_statement("SELECT a=b--1; INSERT INTO l VALUES (1)",
                                   LACE_DRIVER_MYSQL));
  CHECK(lace_sql_single_statement("SELECT 1 -- ; not a statement", LACE_DRIVER_MYSQL));
  /* PostgreSQL: '$' in an identifier and $1 open no $tag$ */
  CHECK(!lace_sql_single_statement("SELECT x$y$ FROM t; SELECT 2", LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_single_statement("SELECT $1; SELECT $2", LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_single_statement("SELECT $f$ ; $f$", LACE_DRIVER_POSTGRES));

  CHECK(lace_sql_read_only("SELECT * FROM t", LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_read_only("WITH x AS (SELECT 1) SELECT * FROM x", LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_read_only("SELECT * FROM t FOR NO KEY UPDATE", LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_read_only("SELECT REPLACE(a, 'x', 'y') FROM t", LACE_DRIVER_MYSQL));
  CHECK(lace_sql_read_only("SELECT 'delete' /* insert */ FROM t", LACE_DRIVER_SQLITE));
  CHECK(!lace_sql_read_only("INSERT INTO t SELECT * FROM s RETURNING *", LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_read_only("WITH u AS (UPDATE t SET a = 1 RETURNING *) SELECT * FROM u",
                            LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_read_only("DELETE FROM t RETURNING *", LACE_DRIVER_MARIADB));
  CHECK(!lace_sql_read_only("SELECT * INTO new_t FROM t", LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_read_only("EXPLAIN ANALYZE UPDATE t SET a = 1", LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_read_only("CALL proc()", LACE_DRIVER_MYSQL));
  CHECK(!lace_sql_read_only("", LACE_DRIVER_SQLITE));
  /* Writing verbs as column names do not count; only where a verb stands */
  CHECK(lace_sql_read_only("SELECT merge, upsert, update FROM t", LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_read_only("SELECT t.delete FROM t WHERE insert > 0", LACE_DRIVER_POSTGRES));
  CHECK(lace_sql_read_only("SELECT * FROM (SELECT merge FROM t) s", LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_read_only("WITH d AS MATERIALIZED (DELETE FROM t RETURNING *) SELECT 1",
                            LACE_DRIVER_POSTGRES));
  CHECK(!lace_sql_read_only("SELECT 1; DELETE FROM t", LACE_DRIVER_SQLITE));
}

/* Saved connections become URLs that mean what the fields say */
static void test_saved_conn_urls(void) {
  LaceSavedConnection c = {0};
  c.driver = "postgres";
  c.user = "u";
  c.host = "::1";
  c.port = 5432;
  c.database = "db";
  CHECK_STR_FREE(lace_saved_conn_to_connstr(&c), "postgres://u@[::1]:5432/db");
  c.host = "h?x"; /* Would turn the rest into options */
  CHECK(lace_saved_conn_to_connstr(&c) == NULL);
  c.host = "evil@good";
  CHECK(lace_saved_conn_to_connstr(&c) == NULL);
  c.host = "h";
  c.port = 0;
  CHECK_STR_FREE(lace_saved_conn_to_connstr(&c), "postgres://u@h/db");
  /* SQLite: a user left over from another driver is not part of the path */
  c.driver = "sqlite";
  c.user = "bob";
  c.database = "/tmp/x.db";
  CHECK_STR_FREE(lace_saved_conn_to_connstr(&c), "sqlite:///tmp/x.db");
  /* SSH with IPv6 at both ends, and back */
  LaceSavedConnection t = {0};
  t.driver = "postgres";
  t.ssh = true;
  t.ssh_host = "fe80::2";
  t.ssh_port = 22;
  t.ssh_user = "s";
  t.host = "::1";
  t.port = 5432;
  t.user = "u";
  t.database = "db";
  char *url = lace_saved_conn_to_connstr(&t);
  CHECK_STR_FREE(str_dup(url), "postgres+ssh://s@[fe80::2]:22/u@[::1]:5432/db");
  ConnString *cs = url ? connstr_parse(url, NULL) : NULL;
  CHECK(cs && cs->ssh && strcmp(cs->ssh_host, "fe80::2") == 0 && cs->ssh_port == 22 &&
        strcmp(cs->host, "::1") == 0 && cs->port == 5432 && strcmp(cs->database, "db") == 0);
  connstr_free(cs);
  free(url);
  /* An SSH key file travels as an option, encoded */
  t.ssh_key = "/home/me/.ssh/id work";
  url = lace_saved_conn_to_connstr(&t);
  cs = url ? connstr_parse(url, NULL) : NULL;
  CHECK(cs && connstr_get_option(cs, "ssh_key") &&
        strcmp(connstr_get_option(cs, "ssh_key"), "/home/me/.ssh/id work") == 0);
  LaceSavedConnection *back = url ? lace_saved_conn_from_connstr(url, NULL) : NULL;
  CHECK(back && back->ssh_key && strcmp(back->ssh_key, "/home/me/.ssh/id work") == 0);
  lace_saved_conn_free(back);
  connstr_free(cs);
  free(url);
  /* UUIDs: version 4, unique */
  char *a = lace_generate_uuid(), *b = lace_generate_uuid();
  CHECK(a && b && strlen(a) == 36 && a[14] == '4' && strcmp(a, b) != 0);
  free(a);
  free(b);
}

/* --------------------------------------------------------------------------
 * Frontend SQL builders and row keys (sql_builder.h, row_key.h): the GTK
 * frontend's rules, kept here so every native frontend shares one copy
 * -------------------------------------------------------------------------- */

static void test_display(void) {
  LaceValue v = lace_value_null();
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "NULL");
  v = lace_value_int(-42);
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "-42");
  v = lace_value_float(0.1);
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "0.1");
  v = lace_value_bool(true);
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "true");
  v = lace_value_text("a\nb\r\nc");
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "a⏎b⏎c");
  CHECK_STR_FREE(lace_value_display_text(&v, 2), "a⏎…");
  lace_value_free(&v);
  v = lace_value_text("héllo wörld");
  CHECK_STR_FREE(lace_value_display_text(&v, 5), "héllo…");
  lace_value_free(&v);
  uint8_t bytes[3] = {0xde, 0xad, 0x01};
  v = lace_value_blob(bytes, 3);
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "0xdead01 (3 bytes)");
  lace_value_free(&v);
  v = lace_value_blob(bytes, 0);
  CHECK_STR_FREE(lace_value_display_text(&v, 0), "0x (0 bytes)");
  lace_value_free(&v);
  CHECK(lace_type_is_numeric(LACE_TYPE_DECIMAL));
  CHECK(!lace_type_is_numeric(LACE_TYPE_TEXT));
}

static void test_edit_value(void) {
  LaceValue out;
  bool unchanged;
  char *err = NULL;
  LaceValue orig = lace_value_text("abc");
  CHECK(lace_edit_value(&orig, LACE_TYPE_TEXT, "abc", &out, &unchanged, &err) && unchanged);
  CHECK(lace_edit_value(&orig, LACE_TYPE_TEXT, "", &out, &unchanged, &err) && !unchanged);
  CHECK(out.type == LACE_TYPE_TEXT && !out.is_null && out.text.len == 0);
  lace_value_free(&out);
  CHECK(lace_edit_value(&orig, LACE_TYPE_TEXT, NULL, &out, &unchanged, &err) && !unchanged);
  CHECK(out.is_null);
  lace_value_free(&orig);
  orig = lace_value_null();
  CHECK(lace_edit_value(&orig, LACE_TYPE_INT, "", &out, &unchanged, &err) && unchanged);
  CHECK(lace_edit_value(&orig, LACE_TYPE_INT, NULL, &out, &unchanged, &err) && unchanged);
  CHECK(lace_edit_value(&orig, LACE_TYPE_INT, "42", &out, &unchanged, &err) && !unchanged);
  CHECK(out.type == LACE_TYPE_INT && out.int_val == 42);
  uint8_t b[1] = {1};
  orig = lace_value_blob(b, 1);
  CHECK(!lace_edit_value(&orig, LACE_TYPE_BLOB, "zz", &out, &unchanged, &err));
  CHECK(err != NULL);
  free(err);
  lace_value_free(&orig);
}

static void test_shape(void) {
  LaceSqlShape s;
  lace_sql_shape("SELECT 1; -- c", LACE_DRIVER_SQLITE, &s);
  CHECK_STR(s.first_keyword, "SELECT");
  CHECK_STR(s.verb, "SELECT");
  CHECK(s.end == 8);
  CHECK(lace_sql_shape_pageable(&s));
  CHECK(!lace_sql_shape_needs_wrap(&s));

  lace_sql_shape("WITH u AS (UPDATE t SET a=1 RETURNING *) SELECT * FROM u",
            LACE_DRIVER_POSTGRES, &s);
  CHECK_STR(s.first_keyword, "WITH");
  CHECK_STR(s.verb, "SELECT");
  CHECK(s.nested_write);
  CHECK(!lace_sql_shape_pageable(&s));

  lace_sql_shape("SELECT * FROM t LIMIT 5", LACE_DRIVER_SQLITE, &s);
  CHECK(s.top_level_limit);
  CHECK(lace_sql_shape_needs_wrap(&s));
  CHECK_STR_FREE(lace_sql_page_of_statement("SELECT * FROM t LIMIT 5", true, 10, 20),
             "SELECT * FROM (\nSELECT * FROM t LIMIT 5\n) AS lace_page LIMIT 10 OFFSET 20");
  CHECK_STR_FREE(lace_sql_page_of_statement("SELECT 1", false, 10, 0),
             "SELECT 1\nLIMIT 10 OFFSET 0");
  CHECK_STR_FREE(lace_sql_count_of_statement("SELECT 1"),
             "SELECT COUNT(*) FROM (\nSELECT 1\n) AS lace_count");

  lace_sql_shape("SELECT * FROM (SELECT 1 LIMIT 1) x", LACE_DRIVER_SQLITE, &s);
  CHECK(!s.top_level_limit);

  lace_sql_shape("VALUES (1), (2)", LACE_DRIVER_SQLITE, &s);
  CHECK_STR(s.verb, "VALUES");
  CHECK(lace_sql_shape_pageable(&s));
  CHECK(lace_sql_shape_needs_wrap(&s));

  lace_sql_shape("INSERT INTO t VALUES (1)", LACE_DRIVER_SQLITE, &s);
  CHECK_STR(s.verb, "INSERT");
  CHECK(!lace_sql_shape_pageable(&s));

  lace_sql_shape("SELECT * INTO new_t FROM t", LACE_DRIVER_POSTGRES, &s);
  CHECK(s.top_level_into);
  CHECK(!lace_sql_shape_pageable(&s));

  lace_sql_shape("select merge from t", LACE_DRIVER_SQLITE, &s);
  CHECK(lace_sql_shape_pageable(&s));

  lace_sql_shape("PRAGMA table_info(t)", LACE_DRIVER_SQLITE, &s);
  CHECK(!lace_sql_shape_pageable(&s));

  /* A string holding a semicolon and a comment keeps the end past it */
  lace_sql_shape("SELECT 'a;b' -- x\n", LACE_DRIVER_SQLITE, &s);
  CHECK(s.end == 12);
}

static void test_statement_at(void) {
  const char *t = "SELECT 1;\n\nSELECT 2;  \n-- tail\n";
  size_t s, e;
  CHECK(lace_sql_statement_at(t, 0, LACE_DRIVER_SQLITE, &s, &e));
  CHECK(s == 0 && e == 8);
  CHECK(lace_sql_statement_at(t, 8, LACE_DRIVER_SQLITE, &s, &e)); /* at ';' */
  CHECK(s == 0 && e == 8);
  CHECK(lace_sql_statement_at(t, 10, LACE_DRIVER_SQLITE, &s, &e)); /* blank gap */
  CHECK(s == 0 && e == 8);
  CHECK(lace_sql_statement_at(t, 13, LACE_DRIVER_SQLITE, &s, &e));
  CHECK(s == 11 && e == 19);
  CHECK(lace_sql_statement_at(t, strlen(t), LACE_DRIVER_SQLITE, &s, &e));
  CHECK(s == 11 && e == 19);
  CHECK(!lace_sql_statement_at("  -- only\n", 3, LACE_DRIVER_SQLITE, &s, &e));
  CHECK(!lace_sql_statement_at("", 0, LACE_DRIVER_SQLITE, &s, &e));

  /* Iteration */
  size_t next = 0;
  int n = 0;
  while (lace_sql_next_statement(t, next, LACE_DRIVER_SQLITE, &s, &e, &next)) {
    n++;
    if (n == 1)
      CHECK(s == 0 && e == 8);
    if (n == 2)
      CHECK(s == 11 && e == 19);
  }
  CHECK(n == 2);

  /* A ';' inside a dollar-quoted body does not split on PostgreSQL */
  const char *pg = "CREATE FUNCTION f() RETURNS int AS $$ SELECT 1; $$ LANGUAGE sql; SELECT 2";
  next = 0;
  n = 0;
  while (lace_sql_next_statement(pg, next, LACE_DRIVER_POSTGRES, &s, &e, &next))
    n++;
  CHECK(n == 2);
}

static LaceSchema *make_schema(void) {
  LaceSchema *sc = calloc(1, sizeof(*sc));
  sc->num_columns = 3;
  sc->columns = calloc(3, sizeof(LaceColumn));
  sc->columns[0].name = strdup("id");
  sc->columns[0].type = LACE_TYPE_INT;
  sc->columns[0].primary_key = true;
  sc->columns[1].name = strdup("name");
  sc->columns[1].type = LACE_TYPE_TEXT;
  sc->columns[2].name = strdup("note");
  sc->columns[2].type = LACE_TYPE_TEXT;
  return sc;
}

static void free_schema(LaceSchema *sc) {
  for (size_t i = 0; i < sc->num_columns; i++)
    free(sc->columns[i].name);
  free(sc->columns);
  free(sc);
}

static void test_filters(void) {
  LaceSchema *sc = make_schema();
  LaceFilter f[3] = {
      {strdup("id"), LACE_FILTER_GT, strdup("10"), NULL},
      {strdup("name"), LACE_FILTER_CONTAINS, strdup("o'k"), NULL},
      {NULL, LACE_FILTER_RAW, strdup(" note IS NOT NULL "), NULL},
  };
  CHECK_STR_FREE(lace_filter_build_where(f, 3, NULL, sc, LACE_DRIVER_SQLITE),
             "\"id\" > 10 AND \"name\" LIKE '%o''k%' AND (note IS NOT NULL)");
  CHECK_STR_FREE(lace_filter_build_where(f, 3, NULL, sc, LACE_DRIVER_MYSQL),
             "`id` > 10 AND `name` LIKE '%o''k%' AND (note IS NOT NULL)");

  /* Operators */
  LaceFilter one = {strdup("name"), LACE_FILTER_EQ, strdup("x"), NULL};
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_POSTGRES), "\"name\" = 'x'");
  one.op = LACE_FILTER_IN;
  free(one.value);
  one.value = strdup("a, 'b c', 3");
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE),
             "\"name\" IN ('a', 'b c', 3)");
  one.op = LACE_FILTER_BETWEEN;
  free(one.value);
  one.value = strdup("1");
  one.value2 = strdup("5");
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE),
             "\"name\" BETWEEN 1 AND 5");
  free(one.value2);
  one.value2 = NULL;
  CHECK(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE) == NULL);
  one.op = LACE_FILTER_NULL;
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE), "\"name\" IS NULL");
  one.op = LACE_FILTER_STARTS;
  free(one.value);
  one.value = strdup("ab");
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE), "\"name\" LIKE 'ab%'");
  one.op = LACE_FILTER_REGEX;
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_POSTGRES), "\"name\" ~ 'ab'");
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_MARIADB), "`name` REGEXP 'ab'");
  CHECK_STR_FREE(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE), "\"name\" GLOB '*ab*'");
  /* Unknown column: skipped */
  free(one.column);
  one.column = strdup("nope");
  one.op = LACE_FILTER_EQ;
  CHECK(lace_filter_build_where(&one, 1, NULL, sc, LACE_DRIVER_SQLITE) == NULL);
  lace_filter_clear(&one);

  /* Search over text columns */
  CHECK_STR_FREE(lace_filter_build_where(NULL, 0, "bob", sc, LACE_DRIVER_SQLITE),
             "(\"name\" LIKE '%bob%' OR \"note\" LIKE '%bob%')");
  CHECK_STR_FREE(lace_filter_build_where(f, 1, "bob", sc, LACE_DRIVER_POSTGRES),
             "\"id\" > 10 AND (\"name\" ILIKE '%bob%' OR \"note\" ILIKE '%bob%')");
  CHECK(lace_filter_build_where(NULL, 0, "", sc, LACE_DRIVER_SQLITE) == NULL);

  for (int i = 0; i < 3; i++)
    lace_filter_clear(&f[i]);

  LaceSortSpec sorts[2] = {{"name", true}, {"id", false}};
  CHECK_STR_FREE(lace_sort_build_order_by(sorts, 2, LACE_DRIVER_MYSQL), "`name` DESC, `id` ASC");
  CHECK(lace_sort_build_order_by(NULL, 0, LACE_DRIVER_MYSQL) == NULL);

  CHECK_STR_FREE(lace_sql_table_page(LACE_DRIVER_POSTGRES, "Sales.orders", "\"id\" > 1",
                            "\"id\" ASC", 1000, 2000),
             "SELECT * FROM \"Sales\".\"orders\" WHERE \"id\" > 1 ORDER BY \"id\" ASC LIMIT 1000 OFFSET 2000");
  CHECK_STR_FREE(lace_sql_table_page(LACE_DRIVER_SQLITE, "t", NULL, NULL, 10, 0),
             "SELECT * FROM \"t\" LIMIT 10 OFFSET 0");
  CHECK_STR_FREE(lace_sql_table_count(LACE_DRIVER_MYSQL, "t", NULL), "SELECT COUNT(*) FROM `t`");
  CHECK_STR_FREE(lace_sql_table_count(LACE_DRIVER_MYSQL, "t", "x = 1"),
             "SELECT COUNT(*) FROM `t` WHERE x = 1");

  CHECK(lace_filter_op_from_key("contains") == LACE_FILTER_CONTAINS);
  CHECK(lace_filter_op_from_key("bogus") == LACE_FILTER_EQ);
  CHECK_STR(lace_filter_op_key(LACE_FILTER_NOT_NULL), "not_null");
  CHECK(lace_filter_op_two_values(LACE_FILTER_BETWEEN));
  CHECK(!lace_filter_op_needs_value(LACE_FILTER_EMPTY));
  free_schema(sc);
}

static void test_simple_select(void) {
  CHECK_STR_FREE(lace_sql_simple_select_table("select * from \"Orders\" where x = 1", LACE_DRIVER_SQLITE), "Orders");
  CHECK_STR_FREE(lace_sql_simple_select_table("SELECT a, b FROM main.t", LACE_DRIVER_SQLITE), "t");
  CHECK_STR_FREE(lace_sql_simple_select_table("SELECT * FROM Orders", LACE_DRIVER_POSTGRES), "orders");
  CHECK_STR_FREE(lace_sql_simple_select_table("SELECT * FROM sales.\"Odd.Name\" LIMIT 5", LACE_DRIVER_POSTGRES), "sales.\"Odd.Name\"");
  CHECK_STR_FREE(lace_sql_simple_select_table("SELECT * FROM `t`", LACE_DRIVER_MYSQL), "t");
  CHECK(lace_sql_simple_select_table("SELECT * FROM a JOIN b ON a.id = b.id", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT * FROM a UNION SELECT * FROM b", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT * FROM (SELECT 1) x", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT count(*) FROM t", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT a FROM t GROUP BY a", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT DISTINCT a FROM t", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT * FROM a, b", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT * FROM other.t", LACE_DRIVER_MYSQL) == NULL);
  CHECK(lace_sql_simple_select_table("INSERT INTO t VALUES (1)", LACE_DRIVER_SQLITE) == NULL);
  CHECK(lace_sql_simple_select_table("SELECT 1", LACE_DRIVER_SQLITE) == NULL);
  /* "JOIN" inside a string does not count */
  CHECK_STR_FREE(lace_sql_simple_select_table("SELECT * FROM t WHERE n = 'JOIN'", LACE_DRIVER_SQLITE), "t");
}

static LaceResult *make_result(void) {
  /* Columns in a different order than the schema: name, id, extra */
  LaceResult *r = calloc(1, sizeof(*r));
  r->num_columns = 3;
  r->columns = calloc(3, sizeof(LaceColumn));
  r->columns[0].name = strdup("name");
  r->columns[1].name = strdup("id");
  r->columns[2].name = strdup("extra");
  r->num_rows = 2;
  r->rows = calloc(2, sizeof(LaceRow));
  for (size_t i = 0; i < 2; i++) {
    r->rows[i].num_cells = 3;
    r->rows[i].cells = calloc(3, sizeof(LaceValue));
    r->rows[i].cells[0] = lace_value_text(i == 0 ? "alice" : "bob");
    r->rows[i].cells[1] = i == 0 ? lace_value_int(7) : lace_value_null();
    r->rows[i].cells[2] = lace_value_float(1.5);
  }
  return r;
}

static void test_row_key(void) {
  LaceSchema *sc = make_schema();
  LaceResult *r = make_result();
  LaceRowKey key;
  const char *why = NULL;
  CHECK(lace_row_key_build(&key, r, 0, sc, true, &why) == LACE_ROW_KEY_OK);
  CHECK(key.count == 1);
  CHECK_STR(key.names[0], "id");
  CHECK(key.values[0].type == LACE_TYPE_INT && key.values[0].int_val == 7);
  LacePkValue *pk = lace_row_key_to_pk(&key);
  CHECK_STR(pk[0].column, "id");
  free(pk);
  LaceRowKey copy;
  lace_row_key_copy(&copy, &key);
  CHECK(copy.count == 1 && copy.values[0].int_val == 7);
  lace_row_key_free(&copy);
  lace_row_key_free(&key);

  /* NULL key part */
  CHECK(lace_row_key_build(&key, r, 1, sc, true, &why) == LACE_ROW_KEY_BAD);
  CHECK(why && strstr(why, "NULL"));

  /* Snapshot: every column of the result that belongs to the table */
  LaceRowSnapshot snap;
  lace_row_snapshot_build(&snap, r, 0, SIZE_MAX, sc);
  CHECK(snap.count == 2); /* name and id, not extra */
  CHECK_STR(snap.names[0], "name");
  CHECK_STR(snap.names[1], "id");
  lace_row_snapshot_free(&snap);
  lace_row_snapshot_build(&snap, r, 0, 2, sc);
  CHECK(snap.count == 1);
  CHECK_STR(snap.names[0], "extra");
  lace_row_snapshot_free(&snap);

  /* Missing pk column */
  free(r->columns[1].name);
  r->columns[1].name = strdup("ident");
  CHECK(lace_row_key_build(&key, r, 0, sc, true, &why) == LACE_ROW_KEY_NONE);
  /* Duplicate pk column with unique_names */
  free(r->columns[1].name);
  r->columns[1].name = strdup("id");
  free(r->columns[2].name);
  r->columns[2].name = strdup("id");
  CHECK(lace_row_key_build(&key, r, 0, sc, true, &why) == LACE_ROW_KEY_NONE);
  CHECK(lace_row_key_build(&key, r, 0, sc, false, &why) == LACE_ROW_KEY_OK);
  lace_row_key_free(&key);

  /* No primary key in the schema */
  sc->columns[0].primary_key = false;
  CHECK(lace_row_key_build(&key, r, 0, sc, true, &why) == LACE_ROW_KEY_NONE);

  lace_result_free(r);
  free_schema(sc);
}

int main(void) {
  test_quote_identifier();
  test_quote_table();
  test_string_literals();
  test_value_literals();
  test_format_double();
  test_connstr();
  test_error_reporting();
  test_keyset();
  test_is_rollback();
  test_type_from_name();
  test_sha256();
  test_wire_numbers();
  test_lexer_corners();
  test_saved_conn_urls();
  test_display();
  test_edit_value();
  test_shape();
  test_statement_at();
  test_filters();
  test_simple_select();
  test_row_key();
  return test_report("liblace");
}
