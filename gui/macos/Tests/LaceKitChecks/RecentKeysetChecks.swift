//
// Lace macOS frontend
// Checks: recent connections (the TUI's recent.json under the private
// HOME) and the keyset ordering
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkRecents() {
  let r = RecentConnections()
  Checks.equal(r.entries.count, 0, "fresh list")
  var changes = 0
  r.onChanged = { changes += 1 }
  Checks.expect(r.add("postgres://app:secret@db.example.com:5433/shop"), "add a URL")
  Checks.expect(r.add("sqlite:///tmp/x.sqlite"), "add a file")
  Checks.equal(r.entries.map(\.url), ["sqlite:///tmp/x.sqlite", "postgres://app@db.example.com:5433/shop"], "newest first, no password")
  Checks.expect(r.lastError == nil, "written: \(r.lastError ?? "")")
  Checks.expect(r.add("postgres://app:other@db.example.com:5433/shop"), "add again")
  Checks.equal(r.entries.count, 2, "an older copy is dropped")
  Checks.equal(r.entries.first?.url, "postgres://app@db.example.com:5433/shop", "moved first")
  Checks.expect(!r.add("nonsense"), "garbage is refused")
  Checks.equal(changes, 3)
  for i in 0..<12 {
    r.add("sqlite:///tmp/f\(i).sqlite")
  }
  Checks.equal(r.entries.count, RecentConnections.maxCount, "trimmed to \(RecentConnections.maxCount)")
  Checks.equal(r.entries.first?.url, "sqlite:///tmp/f11.sqlite")
  Checks.equal(RecentConnections().entries.count, RecentConnections.maxCount, "reloaded from the file")
  r.remove(url: "sqlite:///tmp/f11.sqlite")
  Checks.equal(r.entries.first?.url, "sqlite:///tmp/f10.sqlite", "remove")
  Checks.equal(RecentConnections.stripPasswords("mysql://u:p@h/db"), "mysql://u@h/db")
  Checks.expect(RecentConnections.stripPasswords("::") == nil, "strip of garbage")
}

func checkKeyset() {
  let schema = Schema(
    name: "t",
    columns: [
      Column(name: "id", type: .int, typeName: "INTEGER", nullable: false, primaryKey: true, autoIncrement: true),
      Column(name: "name", type: .text, typeName: "TEXT", nullable: true),
      Column(name: "score", type: .float, typeName: "REAL", nullable: true),
    ])
  let rows = ResultSet(
    columns: schema.columns,
    rows: [[.int(1), .text("alice"), .float(1.5)], [.int(2), .null, .float(.nan)]])

  guard let plain = Keyset(schema: schema, sorts: [], driver: .sqlite) else {
    Checks.fail("keyset on a keyed table")
    return
  }
  Checks.equal(plain.columnCount, 1, "the key alone")
  Checks.equal(plain.orderBy(forward: true), "\"id\" ASC")
  Checks.equal(plain.orderBy(forward: false), "\"id\" DESC")
  Checks.equal(plain.whereClause(rows: rows, row: 0, forward: true), "((\"id\" > 1))", "after row 0")
  Checks.equal(plain.whereClause(rows: rows, row: 1, forward: false), "((\"id\" < 2))", "before row 1")
  Checks.expect(plain.whereClause(rows: rows, row: 2, forward: true) == nil, "no such row")

  guard let sorted = Keyset(schema: schema, sorts: [SortSpec(column: "name", descending: true)], driver: .sqlite)
  else {
    Checks.fail("keyset with a sort")
    return
  }
  Checks.equal(sorted.columnCount, 2, "sort column plus the key")
  Checks.equal(sorted.orderBy(forward: true), "\"name\" DESC, \"id\" ASC")
  Checks.equal(sorted.orderBy(forward: false), "\"name\" ASC, \"id\" DESC")
  let after = sorted.whereClause(rows: rows, row: 0, forward: true) ?? ""
  Checks.expect(after.contains("'alice'") && after.contains("\"id\" > 1"), "after alice: \(after)")
  let afterNull = sorted.whereClause(rows: rows, row: 1, forward: true) ?? ""
  Checks.expect(afterNull.contains("IS NULL") && afterNull.contains("\"id\" > 2"), "after a NULL sort value: \(afterNull)")

  // A NaN in a keyset column is no boundary; a float sort column is fine
  // on SQLite but refused on PostgreSQL and MySQL
  guard let byScore = Keyset(schema: schema, sorts: [SortSpec(column: "score")], driver: .sqlite) else {
    Checks.fail("float sort on SQLite")
    return
  }
  Checks.expect(byScore.whereClause(rows: rows, row: 1, forward: true) == nil, "NaN is no boundary")
  Checks.expect(byScore.whereClause(rows: rows, row: 0, forward: true) != nil, "1.5 is")
  Checks.expect(Keyset(schema: schema, sorts: [SortSpec(column: "score")], driver: .postgres) == nil, "float sort on PostgreSQL refused")
  Checks.expect(Keyset(schema: schema, sorts: [SortSpec(column: "nope")], driver: .sqlite) == nil, "unknown sort column")
  let keyless = Schema(name: "u", columns: [Column(name: "a", type: .int, typeName: "INT", nullable: true)])
  Checks.expect(Keyset(schema: keyless, sorts: [], driver: .sqlite) == nil, "no primary key")

  // The driver's quoting
  Checks.equal(Keyset(schema: schema, sorts: [], driver: .mysql)?.orderBy(forward: true), "`id` ASC")
  Checks.equal(rows.reversed().value(row: 0, column: 0), .int(2), "reversed rows")
}
