//
// Lace macOS frontend
// Ddl: the statements liblace builds for a change to a table's shape
// (liblace/include/ddl.h), as Swift calls that return the SQL or throw the
// reason the driver cannot do it. The functions are pure: nothing is sent.
// Identifiers and text are quoted by liblace; defaults, check conditions
// and partial-index predicates are SQL the user typed and pass through.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

/// Why a change cannot be built for the driver (shown before any confirmation)
public struct DdlRefused: Error, CustomStringConvertible, Sendable {
  public let reason: String
  public var description: String { reason }
}

/// A column to add, or the shape a column should have after a change
public struct DdlColumn: Sendable {
  public var name: String
  /// nil keeps the current type (a change)
  public var type: String?
  public var nullable = true
  /// SQL as typed ('x', 0, CURRENT_TIMESTAMP); nil: none
  public var defaultSQL: String?
  public var collation: String?
  /// MySQL and PostgreSQL; nil: none (add) or unchanged (change); "" clears
  public var comment: String?
  public var autoIncrement = false
  /// MySQL: AFTER that column; "" FIRST; nil at the end (add) or in place (change)
  public var after: String?

  public init(
    name: String, type: String? = nil, nullable: Bool = true, defaultSQL: String? = nil, collation: String? = nil,
    comment: String? = nil, autoIncrement: Bool = false, after: String? = nil
  ) {
    self.name = name
    self.type = type
    self.nullable = nullable
    self.defaultSQL = defaultSQL
    self.collation = collation
    self.comment = comment
    self.autoIncrement = autoIncrement
    self.after = after
  }
}

/// A foreign key to add
public struct DdlForeignKey: Sendable {
  public var name: String?
  public var columns: [String]
  public var refTable: String
  public var refColumns: [String]
  public var onDelete: String?
  public var onUpdate: String?
  public var deferrable = false
  public var initiallyDeferred = false

  public init(
    name: String? = nil, columns: [String], refTable: String, refColumns: [String], onDelete: String? = nil,
    onUpdate: String? = nil, deferrable: Bool = false, initiallyDeferred: Bool = false
  ) {
    self.name = name
    self.columns = columns
    self.refTable = refTable
    self.refColumns = refColumns
    self.onDelete = onDelete
    self.onUpdate = onUpdate
    self.deferrable = deferrable
    self.initiallyDeferred = initiallyDeferred
  }
}

public enum DdlTableOption: Sendable {
  case comment, engine, charset
}

public enum DdlMaintenance: Sendable {
  case analyze, vacuum, optimize
}

/// A const C string for a struct field (freed with freeConst)
private func dup(_ s: String?) -> UnsafePointer<CChar>? {
  s.flatMap { UnsafePointer(strdup($0)) }
}

private func freeConst(_ p: UnsafePointer<CChar>?) {
  free(UnsafeMutablePointer(mutating: p))
}

/// A C string array that lives for one call
private final class CStrings {
  private var pointers: [UnsafeMutablePointer<CChar>?]

  init(_ strings: [String]) {
    pointers = strings.map { strdup($0) }
  }

  deinit {
    for p in pointers {
      free(p)
    }
  }

  func with<T>(_ body: (UnsafePointer<UnsafePointer<CChar>?>?, Int) throws -> T) rethrows -> T {
    var consts: [UnsafePointer<CChar>?] = pointers.map { UnsafePointer($0) }
    return try consts.withUnsafeMutableBufferPointer { buf in
      try body(UnsafePointer(buf.baseAddress), buf.count)
    }
  }
}

public enum Ddl {
  /// The statement, or the refusal; frees what liblace allocated
  private static func take(_ sql: UnsafeMutablePointer<CChar>?, _ error: UnsafeMutablePointer<CChar>?) throws
    -> String
  {
    if let sql {
      free(error)
      return takeCString(sql) ?? ""
    }
    throw DdlRefused(reason: takeCString(error) ?? "Not possible here")
  }

  private static func withColumn<T>(_ c: DdlColumn, _ body: (UnsafePointer<LaceDdlColumn>) throws -> T) rethrows -> T {
    var l = LaceDdlColumn()
    l.name = dup(c.name)
    l.type = dup(c.type)
    l.nullable = c.nullable
    l.default_sql = dup(c.defaultSQL)
    l.collation = dup(c.collation)
    l.comment = dup(c.comment)
    l.auto_increment = c.autoIncrement
    l.after = dup(c.after)
    defer {
      freeConst(l.name)
      freeConst(l.type)
      freeConst(l.default_sql)
      freeConst(l.collation)
      freeConst(l.comment)
      freeConst(l.after)
    }
    return try withUnsafePointer(to: &l, body)
  }

  // MARK: Columns

  public static func addColumn(_ driver: Driver, table: String, column: DdlColumn) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = withColumn(column) { lace_ddl_add_column(driver.c, table, $0, &err) }
    return try take(sql, err)
  }

  public static func dropColumn(_ driver: Driver, table: String, column: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_column(driver.c, table, column, &err), err)
  }

  public static func renameColumn(_ driver: Driver, table: String, column: String, to newName: String) throws
    -> String
  {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_rename_column(driver.c, table, column, newName, &err), err)
  }

  /// The statements that take a column of the schema (by name, as
  /// lace_get_schema read it) to wanted; throws "No change" when nothing differs
  public static func alterColumn(_ driver: Driver, table: String, schema: Schema, column: String, wanted: DdlColumn)
    throws -> String
  {
    guard let cur = schema.rawColumn(named: column) else {
      throw DdlRefused(reason: "\(column) is not a column of \(table)")
    }
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = withColumn(wanted) { lace_ddl_alter_column(driver.c, table, cur, $0, &err) }
    return try take(sql, err)
  }

  /// The primary key becomes these columns (none drops it); existing names the constraint to drop first
  public static func setPrimaryKey(_ driver: Driver, table: String, columns: [String], existing: String?) throws
    -> String
  {
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = CStrings(columns).with { cols, n in lace_ddl_set_primary_key(driver.c, table, cols, n, existing, &err) }
    return try take(sql, err)
  }

  // MARK: Indexes and constraints

  public static func createIndex(
    _ driver: Driver, table: String, name: String, unique: Bool, columns: [String], method: String?, where: String?
  ) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = CStrings(columns).with { cols, n in
      lace_ddl_create_index(driver.c, table, name, unique, cols, n, method, `where`, &err)
    }
    return try take(sql, err)
  }

  /// constraint: the one the index backs (IndexInfo.constraint), dropped instead
  public static func dropIndex(_ driver: Driver, table: String, name: String, constraint: String?) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_index(driver.c, table, name, constraint, &err), err)
  }

  /// PostgreSQL and MySQL rename; SQLite and MariaDB drop and create from what was read
  public static func renameIndex(_ driver: Driver, table: String, index: IndexInfo, to newName: String) throws
    -> String
  {
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = index.withLaceIndex { lace_ddl_rename_index(driver.c, table, $0, newName, &err) }
    return try take(sql, err)
  }

  public static func addCheck(_ driver: Driver, table: String, name: String?, condition: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_add_check(driver.c, table, name, condition, &err), err)
  }

  public static func dropConstraint(_ driver: Driver, table: String, name: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_constraint(driver.c, table, name, &err), err)
  }

  /// PostgreSQL only; the others drop and add the constraint again
  public static func renameConstraint(_ driver: Driver, table: String, name: String, to newName: String) throws
    -> String
  {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_rename_constraint(driver.c, table, name, newName, &err), err)
  }

  public static func addForeignKey(_ driver: Driver, table: String, key: DdlForeignKey) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = CStrings(key.columns).with { cols, n in
      CStrings(key.refColumns).with { rcols, rn in
        var fk = LaceDdlForeignKey()
        fk.name = dup(key.name)
        fk.columns = cols
        fk.num_columns = n
        fk.ref_table = dup(key.refTable)
        fk.ref_columns = rcols
        fk.num_ref_columns = rn
        fk.on_delete = dup(key.onDelete)
        fk.on_update = dup(key.onUpdate)
        fk.deferrable = key.deferrable
        fk.initially_deferred = key.initiallyDeferred
        defer {
          freeConst(fk.name)
          freeConst(fk.ref_table)
          freeConst(fk.on_delete)
          freeConst(fk.on_update)
        }
        return withUnsafePointer(to: &fk) { lace_ddl_add_foreign_key(driver.c, table, $0, &err) }
      }
    }
    return try take(sql, err)
  }

  public static func dropForeignKey(_ driver: Driver, table: String, name: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_foreign_key(driver.c, table, name, &err), err)
  }

  // MARK: The table

  public static func renameTable(_ driver: Driver, table: String, to newName: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_rename_table(driver.c, table, newName, &err), err)
  }

  public static func truncate(_ driver: Driver, table: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_truncate(driver.c, table, &err), err)
  }

  public static func dropTable(_ driver: Driver, table: String, cascade: Bool) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_table(driver.c, table, cascade, &err), err)
  }

  /// A table with one column, id (the driver's auto-increment key)
  public static func createTableWithID(_ driver: Driver, table: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_create_table_with_id(driver.c, table, &err), err)
  }

  public static func dropView(_ driver: Driver, name: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_view(driver.c, name, &err), err)
  }

  /// PostgreSQL ALTER VIEW RENAME, MySQL RENAME TABLE; SQLite refused
  public static func renameView(_ driver: Driver, name: String, to newName: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_rename_view(driver.c, name, newName, &err), err)
  }

  /// routine: an entry of the object list (PostgreSQL names carry their argument types)
  public static func dropRoutine(_ driver: Driver, routine: CatalogObject) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    let sql = routine.withLaceObject { lace_ddl_drop_routine(driver.c, $0, &err) }
    return try take(sql, err)
  }

  public static func dropTrigger(_ driver: Driver, name: String, table: String?) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_drop_trigger(driver.c, name, table, &err), err)
  }

  /// The counter assigns value next
  public static func setAutoincrement(_ driver: Driver, table: String, counter: AutoIncrement, value: Int64) throws
    -> String
  {
    var err: UnsafeMutablePointer<CChar>? = nil
    var a = LaceAutoInc()
    a.column = strdup(counter.column)
    a.sequence = counter.sequence.map { strdup($0) }
    a.has_counter = counter.hasCounter
    a.next = counter.next
    a.max = counter.max
    defer {
      free(a.column)
      free(a.sequence)
    }
    let sql = withUnsafePointer(to: &a) { lace_ddl_set_autoincrement(driver.c, table, $0, value, &err) }
    return try take(sql, err)
  }

  /// comment: value is the text ("" clears); engine: the engine; charset: the
  /// charset and (value2) the collation
  public static func setTableOption(
    _ driver: Driver, table: String, option: DdlTableOption, value: String?, value2: String? = nil
  ) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    let o: LaceDdlTableOption =
      switch option {
      case .comment: LACE_DDL_OPT_COMMENT
      case .engine: LACE_DDL_OPT_ENGINE
      case .charset: LACE_DDL_OPT_CHARSET
      }
    return try take(lace_ddl_set_table_option(driver.c, table, o, value, value2, &err), err)
  }

  public static func maintenance(_ driver: Driver, table: String, kind: DdlMaintenance) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    let k: LaceDdlMaintenance =
      switch kind {
      case .analyze: LACE_DDL_ANALYZE
      case .vacuum: LACE_DDL_VACUUM
      case .optimize: LACE_DDL_OPTIMIZE
      }
    return try take(lace_ddl_maintenance(driver.c, table, k, &err), err)
  }

  // MARK: Templates

  public static func viewTemplate(_ driver: Driver, name: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_view_template(driver.c, name, &err), err)
  }

  public static func routineTemplate(_ driver: Driver, name: String) throws -> String {
    var err: UnsafeMutablePointer<CChar>? = nil
    return try take(lace_ddl_routine_template(driver.c, name, &err), err)
  }
}

extension Connection {
  /// The pickers' names: the driver's types (plus PostgreSQL's user types)
  public func listTypes() async throws -> [String] {
    try await submitRead { [client] id in
      var names: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>? = nil
      var count = 0
      try client.check(lace_list_types(client.raw, id, &names, &count))
      defer { lace_tables_free(names, count) }
      return (0..<count).compactMap { names?[$0].map { String(cString: $0) } }
    }
  }

  /// The collations (MySQL: those of charset when given)
  public func listCollations(charset: String? = nil) async throws -> [String] {
    try await submitRead { [client] id in
      var names: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>? = nil
      var count = 0
      try client.check(lace_list_collations(client.raw, id, charset, &names, &count))
      defer { lace_tables_free(names, count) }
      return (0..<count).compactMap { names?[$0].map { String(cString: $0) } }
    }
  }

  /// MySQL's engines, the default first; empty elsewhere
  public func listEngines() async throws -> [String] {
    try await submitRead { [client] id in
      var names: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>? = nil
      var count = 0
      try client.check(lace_list_engines(client.raw, id, &names, &count))
      defer { lace_tables_free(names, count) }
      return (0..<count).compactMap { names?[$0].map { String(cString: $0) } }
    }
  }
}
