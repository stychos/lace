//
// Lace macOS frontend
// Checks: the SQL wrappers reach liblace's builders (the C tests cover the
// SQL itself; these check the Swift boundary: strings, ranges, arrays)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkSQL() {
  Checks.equal(SQL.quoteIdentifier("a\"b", driver: .sqlite), "\"a\"\"b\"")
  Checks.equal(SQL.quoteIdentifier("a`b", driver: .mysql), "`a``b`")
  Checks.equal(SQL.quoteTable("Sales.orders", driver: .postgres), "\"Sales\".\"orders\"")
  Checks.equal(SQL.stringLiteral("it's", driver: .postgres), "'it''s'")
  Checks.equal(SQL.tableRef(schema: "s", name: "odd.name"), "s.\"odd.name\"")

  // Shape
  let s1 = SQL.shape("SELECT 1; -- c", driver: .sqlite)
  Checks.equal(s1.firstKeyword, "SELECT")
  Checks.equal(s1.verb, "SELECT")
  Checks.equal(s1.end, 8)
  Checks.expect(s1.isPageable && !s1.needsWrap, "SELECT 1 pages by appending")
  Checks.equal(SQL.base(of: "SELECT 1; -- c", shape: s1), "SELECT 1")
  let s2 = SQL.shape(
    "WITH u AS (UPDATE t SET a=1 RETURNING *) SELECT * FROM u", driver: .postgres)
  Checks.expect(s2.nestedWrite && !s2.isPageable, "a writing CTE is not paged")
  let s3 = SQL.shape("SELECT * FROM t LIMIT 5", driver: .sqlite)
  Checks.expect(s3.topLevelLimit && s3.needsWrap, "own LIMIT: wrapped")
  Checks.equal(
    SQL.page(of: "SELECT * FROM t LIMIT 5", wrap: true, limit: 10, offset: 20),
    "SELECT * FROM (\nSELECT * FROM t LIMIT 5\n) AS lace_page LIMIT 10 OFFSET 20")
  Checks.equal(SQL.page(of: "SELECT 1", wrap: false, limit: 10, offset: 0), "SELECT 1\nLIMIT 10 OFFSET 0")
  Checks.equal(SQL.count(of: "SELECT 1"), "SELECT COUNT(*) FROM (\nSELECT 1\n) AS lace_count")
  Checks.expect(!SQL.shape("PRAGMA table_info(t)", driver: .sqlite).isPageable, "PRAGMA runs whole")
  Checks.expect(SQL.isReadOnly("select merge from t", driver: .sqlite), "merge as a column reads")
  Checks.expect(!SQL.isReadOnly("DELETE FROM t", driver: .sqlite), "DELETE writes")
  Checks.expect(SQL.isRollback("rollback work;") && !SQL.isRollback("ROLLBACK TO SAVEPOINT x"), "rollback test")

  // Statement boundaries, as String ranges
  let text = "SELECT 1;\n\nSELECT 2;  \n-- tail\n"
  func stmt(_ byte: Int) -> String? {
    let idx = text.utf8.index(text.utf8.startIndex, offsetBy: byte)
    return SQL.statement(at: idx, in: text, driver: .sqlite).map { String(text[$0]) }
  }
  Checks.equal(stmt(0), "SELECT 1")
  Checks.equal(stmt(10), "SELECT 1", "blank gap: the previous statement")
  Checks.equal(stmt(13), "SELECT 2")
  Checks.equal(stmt(text.utf8.count), "SELECT 2", "at the end: the last statement")
  Checks.equal(SQL.statement(at: "  -- only\n".startIndex, in: "  -- only\n", driver: .sqlite), nil)
  Checks.equal(SQL.statements(in: text, driver: .sqlite).map { String(text[$0]) }, ["SELECT 1", "SELECT 2"])
  let pg = "CREATE FUNCTION f() RETURNS int AS $$ SELECT 1; $$ LANGUAGE sql; SELECT 2"
  Checks.equal(SQL.statements(in: pg, driver: .postgres).count, 2, "a ';' in a $$ body does not split")
  let multi = "SELECT 'é';SELECT 'ü'"
  Checks.equal(SQL.statements(in: multi, driver: .sqlite).map { String(multi[$0]) }, ["SELECT 'é'", "SELECT 'ü'"])

  // Filters and sorts over a schema made in memory
  let schema = Schema(
    name: "t",
    columns: [
      Column(name: "id", type: .int, primaryKey: true),
      Column(name: "name", type: .text),
      Column(name: "note", type: .text),
    ])
  Checks.equal(schema.primaryKey.map(\.name), ["id"])
  let filters = [
    Filter(column: "id", op: .gt, value: "10"),
    Filter(column: "name", op: .contains, value: "o'k"),
    Filter(column: nil, op: .raw, value: " note IS NOT NULL "),
  ]
  Checks.equal(
    SQL.whereClause(filters: filters, schema: schema, driver: .sqlite),
    "\"id\" > 10 AND \"name\" LIKE '%o''k%' AND (note IS NOT NULL)")
  Checks.equal(
    SQL.whereClause(filters: [], search: "bob", schema: schema, driver: .postgres),
    "(\"name\" ILIKE '%bob%' OR \"note\" ILIKE '%bob%')")
  Checks.equal(
    SQL.whereClause(filters: [Filter(column: "nope", op: .eq, value: "1")], schema: schema, driver: .sqlite),
    nil, "unknown column: skipped")
  Checks.equal(
    SQL.whereClause(filters: [Filter(column: "id", op: .between, value: "1", value2: "5")], schema: schema, driver: .mysql),
    "`id` BETWEEN 1 AND 5")
  Checks.equal(
    SQL.orderBy([SortSpec(column: "name", descending: true), SortSpec(column: "id")], driver: .mysql),
    "`name` DESC, `id` ASC")
  Checks.equal(SQL.orderBy([], driver: .mysql), nil)
  Checks.equal(
    SQL.tablePage(driver: .postgres, table: "Sales.orders", where: "\"id\" > 1", orderBy: "\"id\" ASC", limit: 1000, offset: 2000),
    "SELECT * FROM \"Sales\".\"orders\" WHERE \"id\" > 1 ORDER BY \"id\" ASC LIMIT 1000 OFFSET 2000")
  Checks.equal(SQL.tableCount(driver: .mysql, table: "t", where: "x = 1"), "SELECT COUNT(*) FROM `t` WHERE x = 1")
  Checks.equal(SQL.simpleSelectTable("SELECT * FROM Orders", driver: .postgres), "orders")
  Checks.equal(SQL.simpleSelectTable("SELECT * FROM a JOIN b ON a.id = b.id", driver: .sqlite), nil)

  // Filter operators
  Checks.equal(FilterOp(key: "contains"), .contains)
  Checks.equal(FilterOp(key: "bogus"), .eq)
  Checks.equal(FilterOp.notNull.key, "not_null")
  Checks.equal(FilterOp.null.label, "is NULL")
  Checks.expect(FilterOp.between.takesTwoValues && !FilterOp.empty.needsValue, "operator traits")
  Checks.equal(FilterOp.allCases.count, 17, "every liblace operator is mapped")

  // Drivers
  Checks.equal(Driver(connectionString: "postgres+ssh://u:p@h/db_user:pw@dbhost/db"), .postgres)
  Checks.equal(Driver(connectionString: "mariadb://u@h/db"), .mariadb)
  Checks.equal(Driver(connectionString: "sqlite:///tmp/x.db"), .sqlite)
  Checks.equal(Driver(connectionString: "mysql://u@h/db"), .mysql)
  // A bare file path is not a connection string yet (connstr_from_path makes one)
  Checks.equal(Driver(connectionString: "./data.sqlite"), nil)
}
