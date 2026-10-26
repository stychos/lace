//
// Lace macOS frontend
// Columns, schemas and result sets
//
// Schema and ResultSet keep liblace's C structures alive underneath their
// Swift view, because the row-key functions (lace_row_key_build,
// lace_row_snapshot_build, lace_row_key_verify) read those directly. Both are
// immutable once built (a ResultSet cell changes only through the row
// model's setCell, on the main thread, after a verified write), so sharing
// them between threads is safe.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public struct Column: Sendable, Hashable {
  public let name: String
  public let type: ValueType
  /// The database's own type name ("VARCHAR(255)"), if it said
  public let typeName: String?
  public let nullable: Bool
  public let primaryKey: Bool
  public let autoIncrement: Bool
  /// Computed by the database (GENERATED ... AS): never inserted or updated
  public let generated: Bool
  public let defaultValue: String?
  /// "table.column" when the column references another table
  public let foreignKey: String?
  /// For VARCHAR and friends; -1 when unlimited or unknown
  public let maxLength: Int32
  /// The column's own collation; nil for the type's or table's default
  public let collation: String?
  /// MySQL and PostgreSQL column comment; nil when none
  public let comment: String?
  /// PostgreSQL identity column: 0 none, 1 ALWAYS, 2 BY DEFAULT
  public let identity: Int32
  /// 1-based position in the primary key, 0 when not part of it
  public let keyPosition: Int32

  public init(
    name: String, type: ValueType, typeName: String? = nil, nullable: Bool = true,
    primaryKey: Bool = false, autoIncrement: Bool = false, generated: Bool = false,
    defaultValue: String? = nil, foreignKey: String? = nil, maxLength: Int32 = -1,
    collation: String? = nil, comment: String? = nil, identity: Int32 = 0, keyPosition: Int32 = 0
  ) {
    self.name = name
    self.type = type
    self.typeName = typeName
    self.nullable = nullable
    self.primaryKey = primaryKey
    self.autoIncrement = autoIncrement
    self.generated = generated
    self.defaultValue = defaultValue
    self.foreignKey = foreignKey
    self.maxLength = maxLength
    self.collation = collation
    self.comment = comment
    self.identity = identity
    self.keyPosition = keyPosition
  }

  init(_ c: LaceColumn) {
    name = borrowCString(c.name) ?? ""
    type = ValueType(c.type)
    typeName = borrowCString(c.type_name)
    nullable = c.nullable
    primaryKey = c.primary_key
    autoIncrement = c.auto_increment
    generated = c.generated
    defaultValue = borrowCString(c.default_val)
    foreignKey = borrowCString(c.foreign_key)
    maxLength = c.max_length
    collation = borrowCString(c.collation)
    comment = borrowCString(c.comment)
    identity = c.identity
    keyPosition = c.key_position
  }

  /// Fill a LaceColumn (strings strdup'ed; lace_schema_free frees them)
  func fill(_ c: inout LaceColumn) {
    c.name = strdup(name)
    c.type = type.c
    c.type_name = typeName.map { strdup($0) }
    c.nullable = nullable
    c.primary_key = primaryKey
    c.auto_increment = autoIncrement
    c.generated = generated
    c.default_val = defaultValue.map { strdup($0) }
    c.foreign_key = foreignKey.map { strdup($0) }
    c.max_length = maxLength
    c.collation = collation.map { strdup($0) }
    c.comment = comment.map { strdup($0) }
    c.identity = identity
    c.key_position = keyPosition
  }
}

/// A table's schema as lace_get_schema() returned it
public final class Schema: @unchecked Sendable {
  let raw: UnsafeMutablePointer<LaceSchema>
  public let name: String?
  public let schemaName: String?
  public let columns: [Column]
  /// The database's approximate row count, -1 when unknown
  public let rowCount: Int64

  init(owning raw: UnsafeMutablePointer<LaceSchema>) {
    self.raw = raw
    let s = raw.pointee
    name = borrowCString(s.name)
    schemaName = borrowCString(s.schema)
    rowCount = s.row_count
    columns = (0..<Int(s.num_columns)).map { Column(s.columns[$0]) }
  }

  /// A schema made in memory (synthesized tables, tests)
  public convenience init(name: String? = nil, columns: [Column]) {
    let raw = calloc(1, MemoryLayout<LaceSchema>.stride)!
      .bindMemory(to: LaceSchema.self, capacity: 1)
    raw.pointee.name = name.map { strdup($0) }
    raw.pointee.row_count = -1
    raw.pointee.num_columns = columns.count
    let cols = calloc(max(columns.count, 1), MemoryLayout<LaceColumn>.stride)!
      .bindMemory(to: LaceColumn.self, capacity: max(columns.count, 1))
    for (i, c) in columns.enumerated() {
      c.fill(&cols[i])
    }
    raw.pointee.columns = cols
    self.init(owning: raw)
  }

  deinit {
    lace_schema_free(raw)
  }

  public var primaryKey: [Column] { columns.filter(\.primaryKey) }

  public func column(named name: String) -> Column? {
    columns.first { $0.name == name }
  }

  public func index(ofColumn name: String) -> Int? {
    columns.firstIndex { $0.name == name }
  }

  /// The column as liblace read it (Ddl.alterColumn compares against it)
  func rawColumn(named name: String) -> UnsafePointer<LaceColumn>? {
    let s = raw.pointee
    for i in 0..<Int(s.num_columns) where borrowCString(s.columns[i].name) == name {
      return UnsafePointer(s.columns + i)
    }
    return nil
  }
}

/// What a statement returned: rows (a result with columns: SELECT, but also
/// RETURNING or CALL) or an affected count. The reply's shape decides, not
/// the SQL keyword.
public final class ResultSet: @unchecked Sendable {
  let raw: UnsafeMutablePointer<LaceResult>
  public let columns: [Column]
  public let rowCount: Int
  /// The daemon stopped at its row or size limit
  public let truncated: Bool
  /// A transaction is open on the connection after this statement
  public let inTransaction: Bool
  /// For INSERT/UPDATE/DELETE; meaningless for a result with rows (hasRows)
  public let rowsAffected: Int64

  init(owning raw: UnsafeMutablePointer<LaceResult>) {
    self.raw = raw
    let r = raw.pointee
    columns = (0..<Int(r.num_columns)).map { Column(r.columns[$0]) }
    rowCount = r.rows != nil ? Int(r.num_rows) : 0
    truncated = r.truncated
    inTransaction = r.in_transaction
    rowsAffected = r.rows_affected
  }

  /// A result with no columns: "N rows affected" (a ROLLBACK acknowledged
  /// without being sent answers with 0)
  static func empty(affected: Int64 = 0) -> ResultSet {
    let raw = calloc(1, MemoryLayout<LaceResult>.stride)!
      .bindMemory(to: LaceResult.self, capacity: 1)
    raw.pointee.rows_affected = affected
    return ResultSet(owning: raw)
  }

  /// A result made in memory (tests, synthesized views); every row must
  /// have one value per column
  public convenience init(columns: [Column], rows: [[Value]]) {
    let raw = calloc(1, MemoryLayout<LaceResult>.stride)!
      .bindMemory(to: LaceResult.self, capacity: 1)
    raw.pointee.rows_affected = -1
    raw.pointee.num_columns = columns.count
    let cols = calloc(max(columns.count, 1), MemoryLayout<LaceColumn>.stride)!
      .bindMemory(to: LaceColumn.self, capacity: max(columns.count, 1))
    for (i, c) in columns.enumerated() {
      c.fill(&cols[i])
    }
    raw.pointee.columns = cols
    raw.pointee.num_rows = rows.count
    let crows = calloc(max(rows.count, 1), MemoryLayout<LaceRow>.stride)!
      .bindMemory(to: LaceRow.self, capacity: max(rows.count, 1))
    for (i, values) in rows.enumerated() {
      precondition(values.count == columns.count, "one value per column")
      let cells = calloc(max(values.count, 1), MemoryLayout<LaceValue>.stride)!
        .bindMemory(to: LaceValue.self, capacity: max(values.count, 1))
      for (j, v) in values.enumerated() {
        cells[j] = v.makeLaceValue()
      }
      crows[i].cells = cells
      crows[i].num_cells = values.count
    }
    raw.pointee.rows = crows
    self.init(owning: raw)
  }

  deinit {
    lace_result_free(raw)
  }

  /// True for a result that has columns, even with zero rows
  public var hasRows: Bool { !columns.isEmpty }

  public func value(row: Int, column: Int) -> Value {
    precondition(row >= 0 && row < rowCount, "row out of range")
    let r = raw.pointee.rows[row]
    guard let cells = r.cells, column >= 0, column < Int(r.num_cells) else { return .null }
    return Value(cells[column])
  }

  public func row(_ index: Int) -> [Value] {
    (0..<columns.count).map { value(row: index, column: $0) }
  }

  /// Every row (converted on each call; keep what you need)
  public var rows: [[Value]] { (0..<rowCount).map(row) }

  public func columnIndex(named name: String) -> Int? {
    columns.firstIndex { $0.name == name }
  }

  /// Replace one cell after a verified write, so a page shows the new value
  /// without a reload. The only mutation; row keys and snapshots copy what
  /// they read, so a worker verifying another write never sees it.
  func replaceValue(row: Int, column: Int, with value: Value) {
    precondition(row >= 0 && row < rowCount, "row out of range")
    let r = raw.pointee.rows[row]
    guard let cells = r.cells, column >= 0, column < Int(r.num_cells) else { return }
    lace_value_free(cells + column)
    cells[column] = value.makeLaceValue()
  }
}
