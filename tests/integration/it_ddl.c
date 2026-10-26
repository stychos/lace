/*
 * Lace
 * Integration tests: the DDL liblace synthesizes (ddl.h) runs on every
 * server and does what it says; what a driver cannot do is refused before
 * anything is sent.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "it.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_mysql(const ItDb *db) { return db->driver == LACE_DRIVER_MYSQL || db->driver == LACE_DRIVER_MARIADB; }

static const LaceColumn *col_of(const LaceSchema *s, const char *name) {
  for (size_t i = 0; s && i < s->num_columns; i++)
    if (s->columns[i].name && strcmp(s->columns[i].name, name) == 0) return &s->columns[i];
  return NULL;
}

static const LaceIndex *index_named(const LaceSchema *s, const char *name) {
  for (size_t i = 0; s && i < s->num_indexes; i++)
    if (s->indexes[i].name && strcasecmp(s->indexes[i].name, name) == 0) return &s->indexes[i];
  return NULL;
}

static const LaceIndex *index_of_kind(const LaceSchema *s, LaceIndexKind kind) {
  for (size_t i = 0; s && i < s->num_indexes; i++)
    if (s->indexes[i].kind == kind) return &s->indexes[i];
  return NULL;
}

/* Build the statement with a lace_ddl_* call, check it was built, run it */
#define DDL(db, expr)                                                                                         \
  do {                                                                                                        \
    char *err_ = NULL;                                                                                        \
    char *sql_ = (expr);                                                                                      \
    if (!sql_) fprintf(stderr, "  [%s] %s refused: %s\n", (db)->name, #expr, err_ ? err_ : "(no reason)");    \
    CHECK(sql_ != NULL);                                                                                      \
    if (sql_) it_ex(db, "%s", sql_);                                                                          \
    free(sql_);                                                                                               \
    free(err_);                                                                                               \
  } while (0)

/* The call must refuse with a reason */
#define REFUSED(db, expr)                                                                                     \
  do {                                                                                                        \
    char *err_ = NULL;                                                                                        \
    char *sql_ = (expr);                                                                                      \
    if (sql_) fprintf(stderr, "  [%s] %s built: %s\n", (db)->name, #expr, sql_);                              \
    CHECK(sql_ == NULL && err_ != NULL && *err_);                                                             \
    free(sql_);                                                                                               \
    free(err_);                                                                                               \
  } while (0)

static LaceSchema *schema_of(ItDb *db, const char *table) {
  LaceSchema *s = NULL;
  CHECK_OK(db, lace_get_schema(db->client, db->conn, table, &s));
  return s;
}

static void cleanup(ItDb *db) {
  it_ex(db, "DROP TABLE IF EXISTS it_ddl_child");
  it_ex(db, "DROP TABLE IF EXISTS it_ddl");
  it_ex(db, "DROP TABLE IF EXISTS it_ddl2");
  it_ex(db, "DROP TABLE IF EXISTS it_ddl_nopk");
}

/* Columns: add, rename, alter, drop */
static void test_columns(ItDb *db) {
  LaceDriver d = db->driver;
  cleanup(db);
  DDL(db, lace_ddl_create_table_with_id(d, "it_ddl", &err_));
  LaceSchema *s = schema_of(db, "it_ddl");
  const LaceColumn *id = col_of(s, "id");
  CHECK(id && id->key_position == 1 && (id->auto_increment || id->identity));
  lace_schema_free(s);

  LaceDdlColumn name = {.name = "name", .type = it_dialect(db, "TEXT", "text", "VARCHAR(40)"), .nullable = false,
                        .default_sql = "''"};
  DDL(db, lace_ddl_add_column(d, "it_ddl", &name, &err_));
  LaceDdlColumn n = {.name = "n", .type = it_dialect(db, "INTEGER", "integer", "INT"), .nullable = true,
                     .comment = d == LACE_DRIVER_SQLITE ? NULL : "a number"};
  DDL(db, lace_ddl_add_column(d, "it_ddl", &n, &err_));
  CHECK(it_ex(db, "INSERT INTO it_ddl (name) VALUES ('a')"));
  s = schema_of(db, "it_ddl");
  const LaceColumn *cn = col_of(s, "name"), *cnum = col_of(s, "n");
  CHECK(cn && !cn->nullable && cn->default_val);
  CHECK(cnum && cnum->nullable);
  if (d != LACE_DRIVER_SQLITE) CHECK(cnum && cnum->comment && strcmp(cnum->comment, "a number") == 0);
  lace_schema_free(s);

  DDL(db, lace_ddl_rename_column(d, "it_ddl", "n", "m", &err_));
  s = schema_of(db, "it_ddl");
  CHECK(col_of(s, "m") && !col_of(s, "n"));
  const LaceColumn *cm = col_of(s, "m");
  LaceDdlColumn want = {.name = "m", .type = it_dialect(db, "INTEGER", "bigint", "BIGINT"), .nullable = false,
                        .default_sql = "5", .comment = d == LACE_DRIVER_SQLITE ? NULL : ""};
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, cm ? lace_ddl_alter_column(d, "it_ddl", cm, &want, &err_) : NULL);
  } else if (cm) {
    /* the existing NULL must go first, as a user would do by hand */
    CHECK(it_ex(db, "UPDATE it_ddl SET m = 1"));
    DDL(db, lace_ddl_alter_column(d, "it_ddl", cm, &want, &err_));
    lace_schema_free(s);
    s = schema_of(db, "it_ddl");
    cm = col_of(s, "m");
    CHECK(cm && !cm->nullable && cm->default_val && strstr(cm->default_val, "5"));
    CHECK(cm && cm->type_name && strcasestr(cm->type_name, "bigint"));
    CHECK(cm && (cm->comment == NULL || !*cm->comment));
    /* nothing differs now */
    LaceDdlColumn same = {.name = "m", .type = NULL, .nullable = false, .default_sql = cm ? cm->default_val : NULL};
    char *err = NULL;
    char *sql = cm ? lace_ddl_alter_column(d, "it_ddl", cm, &same, &err) : NULL;
    CHECK(sql == NULL && err && strcmp(err, "No change") == 0);
    free(sql);
    free(err);
    CHECK(it_ex(db, "INSERT INTO it_ddl (name) VALUES ('b')"));
    CHECK(it_scalar(db, "SELECT m FROM it_ddl WHERE name = 'b'") == 5);
  }
  lace_schema_free(s);

  DDL(db, lace_ddl_drop_column(d, "it_ddl", "m", &err_));
  s = schema_of(db, "it_ddl");
  CHECK(s && !col_of(s, "m") && col_of(s, "name"));
  lace_schema_free(s);
  DDL(db, lace_ddl_rename_table(d, "it_ddl", "it_ddl2", &err_));
  s = schema_of(db, "it_ddl2");
  CHECK(s && col_of(s, "name"));
  lace_schema_free(s);
  DDL(db, lace_ddl_drop_table(d, "it_ddl2", false, &err_));
  cleanup(db);
}

/* Indexes, checks and the primary key */
static void test_indexes(ItDb *db) {
  LaceDriver d = db->driver;
  cleanup(db);
  DDL(db, lace_ddl_create_table_with_id(d, "it_ddl", &err_));
  LaceDdlColumn code = {.name = "code", .type = it_dialect(db, "TEXT", "text", "VARCHAR(20)"), .nullable = true};
  DDL(db, lace_ddl_add_column(d, "it_ddl", &code, &err_));
  LaceDdlColumn v = {.name = "v", .type = it_dialect(db, "INTEGER", "integer", "INT"), .nullable = true};
  DDL(db, lace_ddl_add_column(d, "it_ddl", &v, &err_));
  const char *cols[] = {"code"};
  DDL(db, lace_ddl_create_index(d, "it_ddl", "it_ddl_code", true, cols, 1, NULL,
                                d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB ? NULL : "v > 0", &err_));
  LaceSchema *s = schema_of(db, "it_ddl");
  const LaceIndex *ix = index_named(s, "it_ddl_code");
  CHECK(ix && ix->unique && ix->num_columns == 1 && strcmp(ix->columns[0], "code") == 0);
  if (!is_mysql(db)) CHECK(ix && ix->where && strstr(ix->where, "0"));
  if (ix) {
    DDL(db, lace_ddl_rename_index(d, "it_ddl", ix, "it_ddl_code2", &err_));
    lace_schema_free(s);
    s = schema_of(db, "it_ddl");
    const LaceIndex *ix2 = index_named(s, "it_ddl_code2");
    CHECK(ix2 && ix2->unique && !index_named(s, "it_ddl_code"));
    if (!is_mysql(db)) CHECK(ix2 && ix2->where);
    DDL(db, lace_ddl_drop_index(d, "it_ddl", "it_ddl_code2", ix2 ? ix2->constraint : NULL, &err_));
  }
  lace_schema_free(s);
  s = schema_of(db, "it_ddl");
  CHECK(s && !index_named(s, "it_ddl_code2"));
  /* the primary key's index backs a constraint: a drop is refused on SQLite */
  const LaceIndex *pk = index_of_kind(s, LACE_INDEX_PRIMARY);
  if (d == LACE_DRIVER_SQLITE && pk) REFUSED(db, lace_ddl_drop_index(d, "it_ddl", pk->name, pk->constraint ? pk->constraint : "PRIMARY", &err_));
  lace_schema_free(s);

  /* a plain index with a method */
  DDL(db, lace_ddl_create_index(d, "it_ddl", "it_ddl_v", false, (const char *[]){"v"}, 1,
                                it_dialect(db, NULL, "btree", "BTREE"), NULL, &err_));
  s = schema_of(db, "it_ddl");
  CHECK(index_named(s, "it_ddl_v") != NULL);
  lace_schema_free(s);
  DDL(db, lace_ddl_drop_index(d, "it_ddl", "it_ddl_v", NULL, &err_));

  /* checks */
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, lace_ddl_add_check(d, "it_ddl", "it_ddl_pos", "v > 0", &err_));
    REFUSED(db, lace_ddl_drop_constraint(d, "it_ddl", "it_ddl_pos", &err_));
  } else {
    DDL(db, lace_ddl_add_check(d, "it_ddl", "it_ddl_pos", "v > 0", &err_));
    CHECK(it_fail(db, "INSERT INTO it_ddl (v) VALUES (-1)") != 0);
    s = schema_of(db, "it_ddl");
    const LaceIndex *chk = index_named(s, "it_ddl_pos");
    CHECK(chk && chk->kind == LACE_INDEX_CHECK && chk->check);
    lace_schema_free(s);
    if (d == LACE_DRIVER_POSTGRES) {
      DDL(db, lace_ddl_rename_constraint(d, "it_ddl", "it_ddl_pos", "it_ddl_pos2", &err_));
      s = schema_of(db, "it_ddl");
      CHECK(index_named(s, "it_ddl_pos2") && !index_named(s, "it_ddl_pos"));
      lace_schema_free(s);
      DDL(db, lace_ddl_rename_constraint(d, "it_ddl", "it_ddl_pos2", "it_ddl_pos", &err_));
    } else {
      REFUSED(db, lace_ddl_rename_constraint(d, "it_ddl", "it_ddl_pos", "it_ddl_pos2", &err_));
    }
    DDL(db, lace_ddl_drop_constraint(d, "it_ddl", "it_ddl_pos", &err_));
    CHECK(it_ex(db, "INSERT INTO it_ddl (v) VALUES (-1)"));
  }

  /* the primary key of a table without one */
  it_ex(db, "%s", it_dialect(db, "CREATE TABLE it_ddl_nopk (a INTEGER NOT NULL, b INTEGER NOT NULL)",
                             "CREATE TABLE it_ddl_nopk (a integer NOT NULL, b integer NOT NULL)",
                             "CREATE TABLE it_ddl_nopk (a INT NOT NULL, b INT NOT NULL)"));
  const char *ab[] = {"a", "b"};
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, lace_ddl_set_primary_key(d, "it_ddl_nopk", ab, 2, NULL, &err_));
  } else {
    DDL(db, lace_ddl_set_primary_key(d, "it_ddl_nopk", ab, 2, NULL, &err_));
    s = schema_of(db, "it_ddl_nopk");
    const LaceColumn *a = col_of(s, "a"), *b = col_of(s, "b");
    CHECK(a && a->key_position == 1 && b && b->key_position == 2);
    pk = index_of_kind(s, LACE_INDEX_PRIMARY);
    CHECK(pk != NULL);
    /* back to one column */
    DDL(db, lace_ddl_set_primary_key(d, "it_ddl_nopk", ab, 1, pk ? (pk->constraint ? pk->constraint : pk->name) : "PRIMARY", &err_));
    lace_schema_free(s);
    s = schema_of(db, "it_ddl_nopk");
    a = col_of(s, "a");
    b = col_of(s, "b");
    CHECK(a && a->key_position == 1 && b && b->key_position == 0);
    lace_schema_free(s);
  }
  cleanup(db);
}

/* Foreign keys both ways */
static void test_foreign_keys(ItDb *db) {
  LaceDriver d = db->driver;
  cleanup(db);
  DDL(db, lace_ddl_create_table_with_id(d, "it_ddl", &err_));
  DDL(db, lace_ddl_create_table_with_id(d, "it_ddl_child", &err_));
  LaceDdlColumn pc = {.name = "parent_id", .type = it_dialect(db, "INTEGER", "bigint", "BIGINT"), .nullable = true};
  DDL(db, lace_ddl_add_column(d, "it_ddl_child", &pc, &err_));
  const char *cols[] = {"parent_id"}, *ref[] = {"id"};
  LaceDdlForeignKey fk = {.name = "it_ddl_fk", .columns = cols, .num_columns = 1, .ref_table = "it_ddl",
                          .ref_columns = ref, .num_ref_columns = 1, .on_delete = "CASCADE"};
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, lace_ddl_add_foreign_key(d, "it_ddl_child", &fk, &err_));
    REFUSED(db, lace_ddl_drop_foreign_key(d, "it_ddl_child", "it_ddl_fk", &err_));
    cleanup(db);
    return;
  }
  DDL(db, lace_ddl_add_foreign_key(d, "it_ddl_child", &fk, &err_));
  LaceSchema *s = schema_of(db, "it_ddl_child");
  CHECK(s && s->num_foreign_keys == 1);
  if (s && s->num_foreign_keys == 1) {
    CHECK(strcmp(s->foreign_keys[0].name, "it_ddl_fk") == 0);
    CHECK(strcasecmp(s->foreign_keys[0].ref_table, "it_ddl") == 0);
    CHECK(s->foreign_keys[0].on_delete && strcasecmp(s->foreign_keys[0].on_delete, "CASCADE") == 0);
  }
  const LaceColumn *c = col_of(s, "parent_id");
  CHECK(c && c->foreign_key && strcasestr(c->foreign_key, "it_ddl.id"));
  lace_schema_free(s);
  LaceDependent *deps = NULL;
  size_t nd = 0;
  CHECK_OK(db, lace_table_dependents(db->client, db->conn, "it_ddl", &deps, &nd));
  CHECK(nd == 1 && deps && strcasecmp(deps[0].table, "it_ddl_child") == 0);
  lace_dependents_free(deps, nd);
  /* it holds */
  CHECK(it_fail(db, "INSERT INTO it_ddl_child (parent_id) VALUES (12345)") != 0);
  DDL(db, lace_ddl_drop_foreign_key(d, "it_ddl_child", "it_ddl_fk", &err_));
  s = schema_of(db, "it_ddl_child");
  CHECK(s && s->num_foreign_keys == 0);
  lace_schema_free(s);
  CHECK(it_ex(db, "INSERT INTO it_ddl_child (parent_id) VALUES (12345)"));
  cleanup(db);
}

/* Truncate, the counter, options, maintenance, and the other objects */
static void test_table_ops(ItDb *db) {
  LaceDriver d = db->driver;
  cleanup(db);
  it_ex(db, "DROP VIEW IF EXISTS it_ddl_view");
  DDL(db, lace_ddl_create_table_with_id(d, "it_ddl", &err_));
  LaceDdlColumn v = {.name = "v", .type = it_dialect(db, "INTEGER", "integer", "INT"), .nullable = true};
  DDL(db, lace_ddl_add_column(d, "it_ddl", &v, &err_));
  CHECK(it_ex(db, "INSERT INTO it_ddl (v) VALUES (1)"));
  CHECK(it_ex(db, "INSERT INTO it_ddl (v) VALUES (2)"));
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_ddl") == 2);
  DDL(db, lace_ddl_truncate(d, "it_ddl", &err_));
  CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_ddl") == 0);

  /* the counter: the next row gets 100 */
  LaceAutoInc *a = NULL;
  CHECK_OK(db, lace_autoincrement_get(db->client, db->conn, "it_ddl", &a));
  CHECK(a && a->column && strcmp(a->column, "id") == 0 && a->has_counter);
  if (a && a->column) {
    DDL(db, lace_ddl_set_autoincrement(d, "it_ddl", a, 100, &err_));
    CHECK(it_ex(db, "INSERT INTO it_ddl (v) VALUES (3)"));
    CHECK(it_scalar(db, "SELECT id FROM it_ddl WHERE v = 3") == 100);
    lace_autoinc_free(a);
    a = NULL;
    CHECK_OK(db, lace_autoincrement_get(db->client, db->conn, "it_ddl", &a));
    CHECK(a && a->next == 101 && a->max == 100);
  }
  lace_autoinc_free(a);

  /* options */
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, lace_ddl_set_table_option(d, "it_ddl", LACE_DDL_OPT_COMMENT, "x", NULL, &err_));
  } else {
    DDL(db, lace_ddl_set_table_option(d, "it_ddl", LACE_DDL_OPT_COMMENT, "it's a table", NULL, &err_));
    LaceTableInfo *info = NULL;
    CHECK_OK(db, lace_table_info(db->client, db->conn, "it_ddl", &info));
    CHECK(info && info->comment && strcmp(info->comment, "it's a table") == 0);
    lace_table_info_free(info);
    DDL(db, lace_ddl_set_table_option(d, "it_ddl", LACE_DDL_OPT_COMMENT, "", NULL, &err_));
    info = NULL;
    CHECK_OK(db, lace_table_info(db->client, db->conn, "it_ddl", &info));
    CHECK(info && (!info->comment || !*info->comment));
    lace_table_info_free(info);
  }
  if (is_mysql(db)) {
    char **engines = NULL;
    size_t ne = 0;
    CHECK_OK(db, lace_list_engines(db->client, db->conn, &engines, &ne));
    CHECK(ne > 0 && engines && strcasecmp(engines[0], "InnoDB") == 0);
    if (ne) DDL(db, lace_ddl_set_table_option(d, "it_ddl", LACE_DDL_OPT_ENGINE, engines[0], NULL, &err_));
    lace_tables_free(engines, ne);
    DDL(db, lace_ddl_set_table_option(d, "it_ddl", LACE_DDL_OPT_CHARSET, "utf8mb4", "utf8mb4_bin", &err_));
    LaceTableInfo *info = NULL;
    CHECK_OK(db, lace_table_info(db->client, db->conn, "it_ddl", &info));
    CHECK(info && info->collation && strcmp(info->collation, "utf8mb4_bin") == 0);
    lace_table_info_free(info);
  } else {
    REFUSED(db, lace_ddl_set_table_option(d, "it_ddl", LACE_DDL_OPT_ENGINE, "InnoDB", NULL, &err_));
  }

  /* maintenance */
  DDL(db, lace_ddl_maintenance(d, "it_ddl", LACE_DDL_ANALYZE, &err_));
  if (is_mysql(db)) {
    DDL(db, lace_ddl_maintenance(d, "it_ddl", LACE_DDL_OPTIMIZE, &err_));
    REFUSED(db, lace_ddl_maintenance(d, "it_ddl", LACE_DDL_VACUUM, &err_));
  } else {
    DDL(db, lace_ddl_maintenance(d, "it_ddl", LACE_DDL_VACUUM, &err_));
    REFUSED(db, lace_ddl_maintenance(d, "it_ddl", LACE_DDL_OPTIMIZE, &err_));
  }

  /* pickers */
  char **names = NULL;
  size_t n = 0;
  CHECK_OK(db, lace_list_collations(db->client, db->conn, NULL, &names, &n));
  CHECK(n >= 3);
  lace_tables_free(names, n);
  names = NULL;
  n = 0;
  CHECK_OK(db, lace_list_types(db->client, db->conn, &names, &n));
  CHECK(n >= 5);
  lace_tables_free(names, n);

  /* a view from the template, then dropped */
  {
    char *err = NULL;
    char *tpl = lace_ddl_view_template(d, "it_ddl_view", &err);
    CHECK(tpl != NULL);
    if (tpl) {
      CHECK(it_ex(db, "%sit_ddl", tpl));
      CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_ddl_view") == 1);
    }
    free(tpl);
    free(err);
  }
  LaceObject *objs = NULL;
  size_t no = 0;
  CHECK_OK(db, lace_list_objects(db->client, db->conn, &objs, &no));
  bool seen = false;
  for (size_t i = 0; i < no; i++)
    if (objs[i].kind == LACE_OBJ_VIEW && objs[i].name && strcasecmp(objs[i].name, "it_ddl_view") == 0) seen = true;
  CHECK(seen);
  lace_objects_free(objs, no);
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, lace_ddl_rename_view(d, "it_ddl_view", "it_ddl_view2", &err_));
    DDL(db, lace_ddl_drop_view(d, "it_ddl_view", &err_));
  } else {
    DDL(db, lace_ddl_rename_view(d, "it_ddl_view", "it_ddl_view2", &err_));
    CHECK(it_scalar(db, "SELECT COUNT(*) FROM it_ddl_view2") == 1);
    DDL(db, lace_ddl_drop_view(d, "it_ddl_view2", &err_));
  }

  /* a trigger */
  bool have_trigger = true;
  if (d == LACE_DRIVER_POSTGRES) {
    it_ex(db, "CREATE OR REPLACE FUNCTION it_ddl_trgfn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$");
    it_ex(db, "CREATE TRIGGER it_ddl_trg BEFORE INSERT ON it_ddl FOR EACH ROW EXECUTE FUNCTION it_ddl_trgfn()");
  } else if (d == LACE_DRIVER_SQLITE) {
    it_ex(db, "CREATE TRIGGER it_ddl_trg AFTER INSERT ON it_ddl BEGIN UPDATE it_ddl SET v = v WHERE id = NEW.id; END");
  } else if (lace_exec(db->client, db->conn, "CREATE TRIGGER it_ddl_trg BEFORE INSERT ON it_ddl FOR EACH ROW SET NEW.v = NEW.v", NULL) != LACE_OK) {
    have_trigger = false; /* binary logging without log_bin_trust_function_creators */
  }
  if (have_trigger) {
    DDL(db, lace_ddl_drop_trigger(d, "it_ddl_trg", "it_ddl", &err_));
    objs = NULL;
    no = 0;
    CHECK_OK(db, lace_list_objects(db->client, db->conn, &objs, &no));
    for (size_t i = 0; i < no; i++)
      CHECK(!(objs[i].kind == LACE_OBJ_TRIGGER && objs[i].name && strcmp(objs[i].name, "it_ddl_trg") == 0));
    lace_objects_free(objs, no);
  }

  /* a routine from the template, dropped through the object list's entry */
  if (d == LACE_DRIVER_SQLITE) {
    REFUSED(db, lace_ddl_routine_template(d, "f", &err_));
    LaceObject fake = {.kind = LACE_OBJ_ROUTINE, .name = "f"};
    REFUSED(db, lace_ddl_drop_routine(d, &fake, &err_));
  } else {
    char *err = NULL;
    char *tpl = lace_ddl_routine_template(d, "it_ddl_fn", &err);
    CHECK(tpl != NULL);
    free(err);
    it_ex(db, "DROP FUNCTION IF EXISTS it_ddl_fn");
    if (tpl && lace_exec(db->client, db->conn, tpl, NULL) == LACE_OK) {
      objs = NULL;
      no = 0;
      CHECK_OK(db, lace_list_objects(db->client, db->conn, &objs, &no));
      const LaceObject *fn = NULL;
      for (size_t i = 0; i < no; i++)
        if (objs[i].kind == LACE_OBJ_ROUTINE && objs[i].name && strncmp(objs[i].name, "it_ddl_fn", 9) == 0) fn = &objs[i];
      CHECK(fn != NULL);
      if (fn) DDL(db, lace_ddl_drop_routine(d, fn, &err_));
      lace_objects_free(objs, no);
      objs = NULL;
      no = 0;
      CHECK_OK(db, lace_list_objects(db->client, db->conn, &objs, &no));
      for (size_t i = 0; i < no; i++)
        CHECK(!(objs[i].kind == LACE_OBJ_ROUTINE && objs[i].name && strncmp(objs[i].name, "it_ddl_fn", 9) == 0));
      lace_objects_free(objs, no);
    } else if (tpl) {
      fprintf(stderr, "  [%s] cannot create functions here: %s\n", db->name, lace_client_error(db->client));
    }
    free(tpl);
  }
  if (d == LACE_DRIVER_POSTGRES) it_ex(db, "DROP FUNCTION IF EXISTS it_ddl_trgfn()");
  DDL(db, lace_ddl_drop_table(d, "it_ddl", d == LACE_DRIVER_POSTGRES, &err_));
  cleanup(db);
}

const ItTest it_ddl_tests[] = {
    {"ddl_columns", test_columns, IT_ALL, false},
    {"ddl_indexes", test_indexes, IT_ALL, false},
    {"ddl_foreign_keys", test_foreign_keys, IT_ALL, false},
    {"ddl_table_ops", test_table_ops, IT_ALL, false},
    {NULL, NULL, 0, false},
};
