//
// Lace macOS frontend
// The catalog reads behind the sub-views of a table tab: the objects of a
// database (views, routines, triggers), a table's information and counter,
// the tables referencing one, an object's definition, and the rows the
// sub-views list, built by the shared core/catalog_rows.c so every
// frontend shows the same thing.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

/// What a tab shows: a table, or one of the other objects lace_list_objects names
public enum ObjectKind: Int32, Sendable, Hashable {
  case table = 0
  case view = 1
  case routine = 2
  case trigger = 3

  public var word: String { String(cString: catalog_object_word(LaceObjectKind(rawValue: UInt32(rawValue)))) }

  /// The session's name: "table", "view", "routine", "trigger"
  public var key: String {
    switch self {
    case .table: "table"
    case .view: "view"
    case .routine: "routine"
    case .trigger: "trigger"
    }
  }

  public init?(key: String) {
    switch key {
    case "table": self = .table
    case "view": self = .view
    case "routine": self = .routine
    case "trigger": self = .trigger
    default: return nil
    }
  }
}

/// An entry of lace_list_objects: a view, routine or trigger (a table is one
/// too, with kind .table, when a tab is made from a table name)
public struct CatalogObject: Sendable, Hashable {
  public let kind: ObjectKind
  public let name: String
  /// A trigger's table
  public let table: String?
  /// A routine's "function"/"procedure", a materialized view's "materialized"
  public let detail: String?

  public init(kind: ObjectKind, name: String, table: String? = nil, detail: String? = nil) {
    self.kind = kind
    self.name = name
    self.table = table
    self.detail = detail
  }

  init(_ o: LaceObject) {
    kind = ObjectKind(rawValue: Int32(o.kind.rawValue)) ?? .table
    name = borrowCString(o.name) ?? ""
    table = borrowCString(o.table)
    detail = borrowCString(o.detail)
  }

  func withLaceObject<T>(_ body: (UnsafePointer<LaceObject>) throws -> T) rethrows -> T {
    var o = LaceObject()
    o.kind = LaceObjectKind(rawValue: UInt32(kind.rawValue))
    o.name = strdup(name)
    o.table = table.map { strdup($0) }
    o.detail = detail.map { strdup($0) }
    defer {
      free(o.name)
      free(o.table)
      free(o.detail)
    }
    return try withUnsafePointer(to: &o, body)
  }
}

/// A foreign key of a schema
public struct ForeignKey: Sendable, Hashable {
  public let name: String?
  public let columns: [String]
  public let refTable: String?
  public let refColumns: [String]
  public let onDelete: String?
  public let onUpdate: String?
}

/// A table whose foreign key references another (lace_table_dependents)
public struct Dependent: Sendable, Hashable {
  public let table: String
  public let name: String?
  public let columns: [String]
  public let refColumns: [String]
  public let onDelete: String?
}

extension Schema {
  /// The schema's foreign keys (filled by lace_get_schema)
  public var foreignKeys: [ForeignKey] {
    let s = raw.pointee
    return (0..<Int(s.num_foreign_keys)).map { i in
      let fk = s.foreign_keys[i]
      return ForeignKey(
        name: borrowCString(fk.name), columns: strings(fk.columns, fk.num_columns),
        refTable: borrowCString(fk.ref_table), refColumns: strings(fk.ref_columns, fk.num_ref_columns),
        onDelete: borrowCString(fk.on_delete), onUpdate: borrowCString(fk.on_update))
    }
  }

  /// The column is the first column of one of the foreign keys: it can be followed
  public func isForeignKeyColumn(_ name: String) -> Bool {
    catalog_fk_first_column(raw, name)
  }
}

private func strings(_ arr: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?, _ n: Int) -> [String] {
  guard let arr else { return [] }
  return (0..<n).map { borrowCString(arr[$0]) ?? "" }
}

/// lace_table_info and the counter, kept as liblace gave them for the row builder
public final class TableInfo: @unchecked Sendable {
  let raw: UnsafeMutablePointer<LaceTableInfo>
  let autoinc: UnsafeMutablePointer<LaceAutoInc>?

  init(raw: UnsafeMutablePointer<LaceTableInfo>, autoinc: UnsafeMutablePointer<LaceAutoInc>?) {
    self.raw = raw
    self.autoinc = autoinc
  }

  deinit {
    lace_table_info_free(raw)
    lace_autoinc_free(autoinc)
  }

  public var rowEstimate: Int64 { raw.pointee.row_estimate }
}

/// The tables referencing one, kept as liblace gave them
public final class Dependents: @unchecked Sendable {
  let raw: UnsafeMutablePointer<LaceDependent>?
  let count: Int
  public let entries: [Dependent]

  init(raw: UnsafeMutablePointer<LaceDependent>?, count: Int) {
    self.raw = raw
    self.count = count
    entries = (0..<count).compactMap { i -> Dependent? in
      guard let raw else { return nil }
      let d = raw[i]
      return Dependent(
        table: borrowCString(d.table) ?? "", name: borrowCString(d.name), columns: strings(d.columns, d.num_columns),
        refColumns: strings(d.ref_columns, d.num_columns), onDelete: borrowCString(d.on_delete))
    }
  }

  deinit {
    lace_dependents_free(raw, count)
  }
}

/// One row of a sub-view's list
public struct CatalogRow: Sendable, Hashable {
  public let cells: [String]
  /// The first cell spans the line
  public let span: Bool
  /// A title or a header line, never selected
  public let heading: Bool
  /// What Enter means (CatalogTable.Tag)
  public let tag: Int
}

/// A sub-view's list: an optional header and its rows, from core/catalog_rows.c
public struct CatalogTable: Sendable {
  public let head: [String]?
  public let rows: [CatalogRow]

  /// The row tags (CATALOG_TAG_*)
  public enum Tag {
    public static let none = 0
    public static let rows = 1
    public static let foreignKey = 100
    public static let dependent = 1000
    public static let trigger = 2000
    public static let column = 3000
  }

  init(_ t: inout CatalogRows) {
    head = t.head.map { h in (0..<Int(t.ncols)).map { borrowCString(h[$0]) ?? "" } }
    rows = (0..<Int(t.nrows)).map { r in
      CatalogRow(
        cells: (0..<Int(t.ncols)).map { borrowCString(t.cells[r]![$0]) ?? "" }, span: t.span[r],
        heading: t.heading[r], tag: Int(t.tag[r]))
    }
  }

  private static func build(_ fill: (inout CatalogRows) -> Bool) -> CatalogTable? {
    var t = CatalogRows()
    catalog_rows_init(&t, 0, nil)
    defer { catalog_rows_free(&t) }
    guard fill(&t) else { return nil }
    return CatalogTable(&t)
  }

  public static func structure(_ schema: Schema) -> CatalogTable {
    build { t in
      catalog_rows_structure(&t, schema.raw)
      return true
    }!
  }

  /// nil when the table has no indexes or constraints
  public static func indexes(_ schema: Schema) -> CatalogTable? {
    build { t in catalog_rows_indexes(&t, schema.raw) }
  }

  public static func foreignKeys(
    _ schema: Schema, table: String, dependents: Dependents?, loaded: Bool, error: String?
  ) -> CatalogTable {
    build { t in
      catalog_rows_fks(&t, schema.raw, table, dependents?.raw, dependents?.count ?? 0, loaded, error)
      return true
    }!
  }

  public static func definition(_ sql: String, width: Int, columns: Schema?) -> CatalogTable {
    build { t in
      catalog_rows_definition(&t, sql, Int32(width), columns?.raw)
      return true
    }!
  }

  public static func info(
    _ object: CatalogObject, info: TableInfo?, error: String?, exact: Int64, counting: Bool,
    objects: [CatalogObject]
  ) -> CatalogTable {
    // The trigger rows name the objects by index: give the builder the same list
    var raw = objects.map { o -> LaceObject in
      var l = LaceObject()
      l.kind = LaceObjectKind(rawValue: UInt32(o.kind.rawValue))
      l.name = strdup(o.name)
      l.table = o.table.map { strdup($0) }
      l.detail = o.detail.map { strdup($0) }
      return l
    }
    defer {
      for i in raw.indices {
        free(raw[i].name)
        free(raw[i].table)
        free(raw[i].detail)
      }
    }
    return raw.withUnsafeBufferPointer { objs in
      build { t in
        catalog_rows_info(
          &t, LaceObjectKind(rawValue: UInt32(object.kind.rawValue)), object.name, object.table, object.detail,
          info?.raw, info?.autoinc, error, exact, counting, objs.baseAddress, objects.count)
        return true
      }!
    }
  }
}

extension Connection {
  /// The database's views, routines and triggers (lace_list_objects)
  public func objects() async throws -> [CatalogObject] {
    try await submitRead { [client] id in
      var objs: UnsafeMutablePointer<LaceObject>? = nil
      var count = 0
      try client.check(lace_list_objects(client.raw, id, &objs, &count))
      defer { lace_objects_free(objs, count) }
      return (0..<count).map { CatalogObject(objs![$0]) }
    }
  }

  /// lace_table_info and lace_autoincrement_get (the counter is a detail: nil when it fails)
  public func tableInfo(of table: String) async throws -> TableInfo {
    try await submitRead { [client] id in
      var info: UnsafeMutablePointer<LaceTableInfo>? = nil
      try client.check(lace_table_info(client.raw, id, table, &info))
      guard let info else { throw LaceError(code: LACE_ERR_INTERNAL_ERROR, message: "No information for \(table)") }
      var autoinc: UnsafeMutablePointer<LaceAutoInc>? = nil
      if lace_autoincrement_get(client.raw, id, table, &autoinc) != LACE_OK {
        autoinc = nil
      }
      return TableInfo(raw: info, autoinc: autoinc)
    }
  }

  /// The tables whose foreign keys reference one
  public func dependents(of table: String) async throws -> Dependents {
    try await submitRead { [client] id in
      var deps: UnsafeMutablePointer<LaceDependent>? = nil
      var count = 0
      try client.check(lace_table_dependents(client.raw, id, table, &deps, &count))
      return Dependents(raw: deps, count: count)
    }
  }

  /// The CREATE text of a view, routine or trigger
  public func definition(of object: CatalogObject) async throws -> String {
    try await submitRead { [client] id in
      var sql: UnsafeMutablePointer<CChar>? = nil
      try object.withLaceObject { o in
        try client.check(lace_object_definition(client.raw, id, o, &sql))
      }
      return takeCString(sql) ?? ""
    }
  }
}
