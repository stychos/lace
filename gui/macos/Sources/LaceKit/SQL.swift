//
// Lace macOS frontend
// SQL synthesis and statement scanning, all from liblace (sql_builder.h):
// quoting, statement shape and boundaries, filters to WHERE, sorts to ORDER
// BY, table pages and counts, the table of a simple SELECT
//
// Nothing here builds SQL from user text on its own: identifiers go through
// lace_quote_identifier()/lace_quote_table(), literals through
// lace_sql_string_literal(), so the frontends share one set of rules.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public enum Driver: Sendable, Hashable, CaseIterable {
  case sqlite, postgres, mysql, mariadb

  init(_ c: LaceDriver) {
    switch c {
    case LACE_DRIVER_POSTGRES: self = .postgres
    case LACE_DRIVER_MYSQL: self = .mysql
    case LACE_DRIVER_MARIADB: self = .mariadb
    default: self = .sqlite
    }
  }

  var c: LaceDriver {
    switch self {
    case .sqlite: LACE_DRIVER_SQLITE
    case .postgres: LACE_DRIVER_POSTGRES
    case .mysql: LACE_DRIVER_MYSQL
    case .mariadb: LACE_DRIVER_MARIADB
    }
  }

  /// The driver a connection string names ("postgres+ssh://..." is postgres)
  public init?(connectionString: String) {
    guard let cs = connstr_parse(connectionString, nil) else { return nil }
    defer { connstr_free(cs) }
    guard let d = cs.pointee.driver,
      let name = borrowCString(lace_driver_normalize(d))
    else { return nil }
    switch name {
    case "sqlite": self = .sqlite
    case "postgres": self = .postgres
    case "mysql": self = .mysql
    case "mariadb": self = .mariadb
    default: return nil
    }
  }

  /// The connection string for a SQLite file (connstr_from_path: the path
  /// percent-encoded where it must be); nil with the reason for a path that
  /// cannot be one
  public static func sqliteConnectionString(path: String) -> Result<String, LaceError> {
    var err: UnsafeMutablePointer<CChar>? = nil
    guard let cs = takeCString(connstr_from_path(path, &err)) else {
      return .failure(
        LaceError(code: LACE_ERR_INVALID_PARAMS, message: takeCString(err) ?? "Not a usable path"))
    }
    return .success(cs)
  }

  /// "sqlite", "postgres", "mysql", "mariadb" (connection strings, config files)
  public var name: String {
    switch self {
    case .sqlite: "sqlite"
    case .postgres: "postgres"
    case .mysql: "mysql"
    case .mariadb: "mariadb"
    }
  }

  public var displayName: String {
    switch self {
    case .sqlite: "SQLite"
    case .postgres: "PostgreSQL"
    case .mysql: "MySQL"
    case .mariadb: "MariaDB"
    }
  }
}

public enum SQL {
  // MARK: Quoting

  /// "name" or `name`, an embedded quote doubled
  public static func quoteIdentifier(_ name: String, driver: Driver) -> String {
    takeCString(lace_quote_identifier(driver.c, name)) ?? ""
  }

  /// A table reference ("schema.table", `Sales."odd.name"`) as SQL
  public static func quoteTable(_ ref: String, driver: Driver) -> String? {
    takeCString(lace_quote_table(driver.c, ref))
  }

  /// A string literal, quotes included, right for the driver in every mode
  public static func stringLiteral(_ text: String, driver: Driver) -> String {
    takeCString(lace_sql_string_literal(driver.c, text)) ?? "NULL"
  }

  /// A table reference from a schema (nil: unqualified) and a name
  public static func tableRef(schema: String?, name: String) -> String {
    takeCString(lace_table_ref(schema, name)) ?? name
  }

  // MARK: Statement shape

  /// The lexical shape of one statement (strings, comments and $tag$ bodies skipped)
  public struct Shape: Sendable, Equatable {
    /// Its first keyword, upper case ("SELECT", "WITH", "PRAGMA")
    public let firstKeyword: String
    /// The first top-level SELECT/INSERT/UPDATE/DELETE/MERGE/REPLACE/VALUES/TABLE/UPSERT
    public let verb: String
    public let topLevelInto: Bool
    /// Its own top-level LIMIT/OFFSET/FETCH
    public let topLevelLimit: Bool
    /// Something in it writes (lace_sql_read_only)
    public let nestedWrite: Bool
    /// Where the statement really ends: before trailing ';', whitespace or
    /// comments (a UTF-8 byte offset)
    public let end: Int
    /// Only reads and can take LIMIT/OFFSET
    public let isPageable: Bool
    /// A page must wrap it in SELECT * FROM (...) instead of appending LIMIT
    public let needsWrap: Bool

    init(_ s: LaceSqlShape) {
      firstKeyword = withUnsafeBytes(of: s.first_keyword) {
        String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
      }
      verb = withUnsafeBytes(of: s.verb) {
        String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
      }
      topLevelInto = s.top_level_into
      topLevelLimit = s.top_level_limit
      nestedWrite = s.nested_write
      end = Int(s.end)
      var copy = s
      isPageable = lace_sql_shape_pageable(&copy)
      needsWrap = lace_sql_shape_needs_wrap(&copy)
    }
  }

  public static func shape(_ sql: String, driver: Driver) -> Shape {
    var s = LaceSqlShape()
    lace_sql_shape(sql, driver.c, &s)
    return Shape(s)
  }

  /// The statement cut where its shape ends (no trailing ';' or comment)
  public static func base(of sql: String, shape: Shape) -> String {
    String(decoding: Array(sql.utf8.prefix(shape.end)), as: UTF8.self)
  }

  /// One page of a statement: "\nLIMIT n OFFSET m" appended, or the statement
  /// wrapped in SELECT * FROM (...) AS lace_page when wrap is set
  public static func page(of base: String, wrap: Bool, limit: Int, offset: Int) -> String {
    takeCString(lace_sql_page_of_statement(base, wrap, limit, offset)) ?? base
  }

  /// SELECT COUNT(*) FROM (...) AS lace_count
  public static func count(of base: String) -> String {
    takeCString(lace_sql_count_of_statement(base)) ?? base
  }

  /// Only reads, as far as the text shows (the daemon's row-limit rule)
  public static func isReadOnly(_ sql: String, driver: Driver) -> Bool {
    lace_sql_read_only(sql, Int32(driver.c.rawValue))
  }

  /// ROLLBACK or ABORT and nothing else (what acknowledges a lost transaction)
  public static func isRollback(_ sql: String) -> Bool {
    lace_sql_is_rollback(sql)
  }

  // MARK: Statement boundaries in editor text

  /// The statement at cursor, its ';' excluded and whitespace trimmed; in
  /// blank space after a ';' it is the previous one. nil when there is none.
  public static func statement(at cursor: String.Index, in text: String, driver: Driver)
    -> Range<String.Index>?
  {
    let offset = text.utf8.distance(from: text.startIndex, to: cursor)
    var start = 0
    var end = 0
    guard lace_sql_statement_at(text, offset, driver.c, &start, &end) else { return nil }
    return byteRange(start..<end, in: text)
  }

  /// Every statement of the text, trimmed and without its ';'; fragments
  /// holding only whitespace and comments are skipped
  public static func statements(in text: String, driver: Driver) -> [Range<String.Index>] {
    var out: [Range<String.Index>] = []
    var from = 0
    var start = 0
    var end = 0
    var next = 0
    while lace_sql_next_statement(text, from, driver.c, &start, &end, &next) {
      out.append(byteRange(start..<end, in: text))
      if next <= from { break }
      from = next
    }
    return out
  }

  private static func byteRange(_ r: Range<Int>, in text: String) -> Range<String.Index> {
    let utf8 = text.utf8
    let s = utf8.index(utf8.startIndex, offsetBy: min(r.lowerBound, utf8.count))
    let e = utf8.index(utf8.startIndex, offsetBy: min(r.upperBound, utf8.count))
    return s..<e
  }

  // MARK: Table pages

  /// SELECT * FROM t [WHERE w] [ORDER BY o] LIMIT n OFFSET m (nil: bad table reference)
  public static func tablePage(
    driver: Driver, table: String, where whereBody: String? = nil, orderBy: String? = nil,
    limit: Int, offset: Int
  ) -> String? {
    takeCString(lace_sql_table_page(driver.c, table, whereBody, orderBy, limit, offset))
  }

  /// SELECT COUNT(*) FROM t [WHERE w]
  public static func tableCount(driver: Driver, table: String, where whereBody: String? = nil)
    -> String?
  {
    takeCString(lace_sql_table_count(driver.c, table, whereBody))
  }

  /// The table a plain single-table SELECT reads, as a table reference, or
  /// nil when its rows cannot be addressed by key (JOIN, set operations,
  /// subqueries, GROUP BY, DISTINCT, aggregates, several tables)
  public static func simpleSelectTable(_ sql: String, driver: Driver) -> String? {
    takeCString(lace_sql_simple_select_table(sql, driver.c))
  }
}

// MARK: Filters and sorts

public enum FilterOp: Int32, Sendable, Hashable, CaseIterable {
  case eq = 0, ne, gt, ge, lt, le
  case `in`, contains, starts, ends, regex, between
  case empty, notEmpty, null, notNull
  /// A raw SQL condition held in the filter's value
  case raw

  var c: LaceFilterOp { LaceFilterOp(rawValue: UInt32(rawValue)) }

  /// "=", "contains", "is NULL"
  public var label: String { borrowCString(lace_filter_op_label(c)) ?? "?" }

  /// "eq", "contains": stable, for session files
  public var key: String { borrowCString(lace_filter_op_key(c)) ?? "eq" }

  /// From a session file's key; unknown keys are "="
  public init(key: String) {
    self = FilterOp(rawValue: Int32(lace_filter_op_from_key(key).rawValue)) ?? .eq
  }

  public var needsValue: Bool { lace_filter_op_needs_value(c) }
  public var takesTwoValues: Bool { lace_filter_op_two_values(c) }
}

/// A column filter; column nil (or op .raw) is a raw SQL condition in value
public struct Filter: Sendable, Hashable {
  public var column: String?
  public var op: FilterOp
  public var value: String
  /// BETWEEN's upper bound
  public var value2: String?

  public init(column: String?, op: FilterOp, value: String, value2: String? = nil) {
    self.column = column
    self.op = op
    self.value = value
    self.value2 = value2
  }
}

/// A sort column by name
public struct SortSpec: Sendable, Hashable {
  public var column: String
  public var descending: Bool

  public init(column: String, descending: Bool = false) {
    self.column = column
    self.descending = descending
  }
}

extension SQL {
  /// WHERE body (no keyword) for the filters, AND-ed, plus a case-insensitive
  /// text search over every text column of the schema when search is
  /// non-empty. Filters whose column is not in the schema, or that need a
  /// value and have none, are skipped. nil when nothing applies.
  public static func whereClause(
    filters: [Filter], search: String? = nil, schema: Schema, driver: Driver
  ) -> String? {
    var cf = filters.map { f in
      LaceFilter(
        column: f.column.map { strdup($0) }, op: f.op.c, value: strdup(f.value),
        value2: f.value2.map { strdup($0) })
    }
    defer {
      for i in cf.indices {
        lace_filter_clear(&cf[i])
      }
    }
    return cf.withUnsafeBufferPointer { buf in
      takeCString(lace_filter_build_where(buf.baseAddress, buf.count, search, schema.raw, driver.c))
    }
  }

  /// ORDER BY body (no keyword); nil when there is no sort
  public static func orderBy(_ sorts: [SortSpec], driver: Driver) -> String? {
    var cs = sorts.map { LaceSortSpec(column: strdup($0.column), desc: $0.descending) }
    defer {
      for s in cs {
        free(s.column)
      }
    }
    return cs.withUnsafeMutableBufferPointer { buf in
      takeCString(lace_sort_build_order_by(buf.baseAddress, buf.count, driver.c))
    }
  }
}
