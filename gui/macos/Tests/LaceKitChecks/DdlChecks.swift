//
// Lace macOS frontend
// Checks: the Ddl wrappers reach liblace's builders and carry the refusals
// (tests/test_liblace.c covers the SQL itself; these check the Swift
// boundary: structs to C, arrays, nil, the thrown reason)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkDdl() {
  func refused(_ what: String, _ body: () throws -> String) -> String? {
    do {
      let sql = try body()
      Checks.fail("\(what): built \(sql), wanted a refusal")
      return nil
    } catch let e as DdlRefused {
      return e.reason
    } catch {
      Checks.fail("\(what): threw \(error)")
      return nil
    }
  }
  func built(_ what: String, _ body: () throws -> String) -> String {
    do {
      return try body()
    } catch {
      Checks.fail("\(what): \(error)")
      return ""
    }
  }

  // columns
  var c = DdlColumn(name: "email", type: "varchar(80)", nullable: false, defaultSQL: "''")
  Checks.equal(
    built("add") { try Ddl.addColumn(.postgres, table: "Sales.\"odd.name\"", column: c) },
    "ALTER TABLE \"Sales\".\"odd.name\" ADD COLUMN \"email\" varchar(80) NOT NULL DEFAULT ''")
  c.collation = "utf8mb4_bin"
  c.comment = "mail"
  c.after = ""
  Checks.equal(
    built("add mysql") { try Ddl.addColumn(.mysql, table: "t", column: c) },
    "ALTER TABLE `t` ADD COLUMN `email` varchar(80) COLLATE utf8mb4_bin NOT NULL DEFAULT '' COMMENT 'mail' FIRST")
  Checks.expect(
    refused("sqlite add not null") { try Ddl.addColumn(.sqlite, table: "t", column: DdlColumn(name: "x", type: "TEXT", nullable: false)) }?
      .contains("default") == true, "SQLite needs a default")
  Checks.equal(built("drop") { try Ddl.dropColumn(.mysql, table: "t", column: "a b") }, "ALTER TABLE `t` DROP COLUMN `a b`")
  Checks.equal(
    built("rename") { try Ddl.renameColumn(.sqlite, table: "t", column: "a", to: "b") },
    "ALTER TABLE \"t\" RENAME COLUMN \"a\" TO \"b\"")

  // alter against a schema made in memory
  let schema = Schema(
    name: "t",
    columns: [
      Column(name: "a", type: .int, typeName: "integer", nullable: true),
      Column(name: "g", type: .int, typeName: "int", generated: true),
    ])
  Checks.equal(
    built("alter") {
      try Ddl.alterColumn(
        .postgres, table: "t", schema: schema, column: "a",
        wanted: DdlColumn(name: "a", type: "bigint", nullable: false, defaultSQL: "0"))
    },
    "ALTER TABLE \"t\" ALTER COLUMN \"a\" TYPE bigint, ALTER COLUMN \"a\" SET DEFAULT 0, ALTER COLUMN \"a\" SET NOT NULL")
  Checks.equal(
    refused("no change") { try Ddl.alterColumn(.postgres, table: "t", schema: schema, column: "a", wanted: DdlColumn(name: "a")) },
    "No change")
  Checks.expect(
    refused("unknown column") { try Ddl.alterColumn(.postgres, table: "t", schema: schema, column: "zz", wanted: DdlColumn(name: "zz")) }?
      .contains("zz") == true, "an unknown column is named")
  Checks.expect(
    refused("generated") { try Ddl.alterColumn(.postgres, table: "t", schema: schema, column: "g", wanted: DdlColumn(name: "g", type: "bigint")) }?
      .contains("generated") == true, "a generated column is refused")
  Checks.equal(
    built("pk") { try Ddl.setPrimaryKey(.postgres, table: "t", columns: ["a", "b"], existing: "t_pkey") },
    "ALTER TABLE \"t\" DROP CONSTRAINT \"t_pkey\", ADD PRIMARY KEY (\"a\", \"b\")")
  Checks.equal(built("pk drop") { try Ddl.setPrimaryKey(.mysql, table: "t", columns: [], existing: nil) }, "ALTER TABLE `t` DROP PRIMARY KEY")

  // indexes
  Checks.equal(
    built("index") { try Ddl.createIndex(.postgres, table: "s.t", name: "ix", unique: true, columns: ["a", "b c"], method: "btree", where: "a > 0") },
    "CREATE UNIQUE INDEX \"ix\" ON \"s\".\"t\" USING btree (\"a\", \"b c\") WHERE a > 0")
  Checks.expect(
    refused("mysql partial") { try Ddl.createIndex(.mysql, table: "t", name: "ix", unique: false, columns: ["a"], method: nil, where: "a > 0") } != nil,
    "MySQL has no partial indexes")
  Checks.equal(built("drop index") { try Ddl.dropIndex(.mysql, table: "t", name: "ix", constraint: nil) }, "DROP INDEX `ix` ON `t`")
  Checks.equal(
    built("drop constraint index") { try Ddl.dropIndex(.postgres, table: "t", name: "t_a_key", constraint: "t_a_key") },
    "ALTER TABLE \"t\" DROP CONSTRAINT \"t_a_key\"")
  let ix = IndexInfo(
    name: "ix", columns: ["a", "b"], unique: true, primary: false, type: nil, kind: .unique, whereClause: "a > 0", check: nil,
    constraint: nil)
  Checks.equal(built("rename index pg") { try Ddl.renameIndex(.postgres, table: "t", index: ix, to: "iy") }, "ALTER INDEX \"ix\" RENAME TO \"iy\"")
  Checks.equal(
    built("rename index sqlite") { try Ddl.renameIndex(.sqlite, table: "t", index: ix, to: "iy") },
    "DROP INDEX \"ix\";\nCREATE UNIQUE INDEX \"iy\" ON \"t\" (\"a\", \"b\") WHERE a > 0")
  Checks.equal(built("check") { try Ddl.addCheck(.postgres, table: "t", name: "ck", condition: "a > 0") }, "ALTER TABLE \"t\" ADD CONSTRAINT \"ck\" CHECK (a > 0)")
  Checks.equal(built("drop constraint") { try Ddl.dropConstraint(.mysql, table: "t", name: "ck") }, "ALTER TABLE `t` DROP CONSTRAINT `ck`")
  Checks.equal(
    built("rename constraint") { try Ddl.renameConstraint(.postgres, table: "t", name: "ck", to: "ck2") },
    "ALTER TABLE \"t\" RENAME CONSTRAINT \"ck\" TO \"ck2\"")
  Checks.expect(refused("rename constraint mysql") { try Ddl.renameConstraint(.mysql, table: "t", name: "ck", to: "ck2") } != nil, "MySQL refused")

  // foreign keys
  let fk = DdlForeignKey(
    name: "fk_c", columns: ["customer_id"], refTable: "customers", refColumns: ["id"], onDelete: "CASCADE", onUpdate: "SET NULL")
  Checks.equal(
    built("fk") { try Ddl.addForeignKey(.postgres, table: "orders", key: fk) },
    "ALTER TABLE \"orders\" ADD CONSTRAINT \"fk_c\" FOREIGN KEY (\"customer_id\") REFERENCES \"customers\" (\"id\") ON DELETE CASCADE ON UPDATE SET NULL")
  Checks.expect(refused("fk sqlite") { try Ddl.addForeignKey(.sqlite, table: "orders", key: fk) }?.contains("SQLite") == true, "SQLite refused")
  Checks.equal(built("drop fk") { try Ddl.dropForeignKey(.mysql, table: "t", name: "fk") }, "ALTER TABLE `t` DROP FOREIGN KEY `fk`")

  // the table
  Checks.equal(built("rename table") { try Ddl.renameTable(.postgres, table: "s.t", to: "u") }, "ALTER TABLE \"s\".\"t\" RENAME TO \"u\"")
  Checks.equal(built("truncate") { try Ddl.truncate(.sqlite, table: "t") }, "DELETE FROM \"t\"")
  Checks.equal(built("drop cascade") { try Ddl.dropTable(.postgres, table: "t", cascade: true) }, "DROP TABLE \"t\" CASCADE")
  Checks.expect(refused("cascade mysql") { try Ddl.dropTable(.mysql, table: "t", cascade: true) } != nil, "CASCADE is PostgreSQL's")
  Checks.equal(built("create") { try Ddl.createTableWithID(.sqlite, table: "t") }, "CREATE TABLE \"t\" (id INTEGER PRIMARY KEY AUTOINCREMENT)")
  Checks.equal(built("drop view") { try Ddl.dropView(.postgres, name: "s.v") }, "DROP VIEW \"s\".\"v\"")
  Checks.equal(built("rename view") { try Ddl.renameView(.mysql, name: "v", to: "w") }, "RENAME TABLE `v` TO `w`")
  Checks.equal(
    built("drop routine") { try Ddl.dropRoutine(.postgres, routine: CatalogObject(kind: .routine, name: "add(integer, integer)", detail: "function")) },
    "DROP FUNCTION add(integer, integer)")
  Checks.equal(built("drop trigger") { try Ddl.dropTrigger(.postgres, name: "trg", table: "t") }, "DROP TRIGGER \"trg\" ON \"t\"")
  Checks.equal(
    built("autoinc") { try Ddl.setAutoincrement(.sqlite, table: "it's", counter: AutoIncrement(column: "id"), value: 100) },
    "DELETE FROM sqlite_sequence WHERE name = 'it''s';\nINSERT INTO sqlite_sequence (name, seq) VALUES ('it''s', 99)")
  Checks.equal(
    built("autoinc pg") { try Ddl.setAutoincrement(.postgres, table: "t", counter: AutoIncrement(column: "id", sequence: "public.t_id_seq"), value: 5) },
    "ALTER SEQUENCE \"public\".\"t_id_seq\" RESTART WITH 5")
  Checks.expect(
    refused("no counter") { try Ddl.setAutoincrement(.sqlite, table: "t", counter: AutoIncrement(column: "id", hasCounter: false), value: 5) } != nil,
    "a plain rowid has no counter")
  Checks.equal(built("comment") { try Ddl.setTableOption(.postgres, table: "t", option: .comment, value: "a 'b'") }, "COMMENT ON TABLE \"t\" IS 'a ''b'''")
  Checks.equal(
    built("charset") { try Ddl.setTableOption(.mysql, table: "t", option: .charset, value: "utf8mb4", value2: "utf8mb4_bin") },
    "ALTER TABLE `t` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin")
  Checks.equal(built("analyze") { try Ddl.maintenance(.mysql, table: "t", kind: .analyze) }, "ANALYZE TABLE `t`")
  Checks.equal(built("vacuum") { try Ddl.maintenance(.sqlite, table: "t", kind: .vacuum) }, "VACUUM")
  Checks.expect(refused("optimize pg") { try Ddl.maintenance(.postgres, table: "t", kind: .optimize) } != nil, "OPTIMIZE is MySQL's")
  Checks.equal(built("view tpl") { try Ddl.viewTemplate(.postgres, name: "v") }, "CREATE VIEW \"v\" AS\nSELECT\n  *\nFROM\n  ")
  Checks.expect(refused("routine tpl sqlite") { try Ddl.routineTemplate(.sqlite, name: "f") } != nil, "SQLite has no routines")

  // the column extras the editors read
  let col = Column(name: "a", type: .text, collation: "NOCASE", comment: "c", identity: 2, keyPosition: 1)
  let s2 = Schema(name: "t", columns: [col])
  Checks.equal(s2.columns[0].collation, "NOCASE")
  Checks.equal(s2.columns[0].comment, "c")
  Checks.equal(s2.columns[0].identity, 2)
  Checks.equal(s2.columns[0].keyPosition, 1)
  Checks.equal(s2.indexes.count, 0)
  Checks.equal(CatalogTable(head: nil, rows: []).appending([CatalogRow(cells: ["x"], span: true, tag: 7)]).rows.first?.tag, 7)
}
