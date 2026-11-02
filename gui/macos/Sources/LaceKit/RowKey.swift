//
// Lace macOS frontend
// Addressing one row for UPDATE / DELETE: liblace's row keys and snapshots
//
// The key is the schema's primary-key columns found BY NAME in the result
// (results order, alias and repeat columns), refused when a part is NULL, NaN,
// infinite or too large to load. A snapshot holds the columns a verification
// compares before a write; Connection.update/delete re-read the row with the
// same WHERE the mutation uses and refuse to write unless exactly one row
// still holds the values on screen.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public enum RowKeyProblem: Error, Sendable, Equatable, CustomStringConvertible {
  /// No usable key: the table has none, or the result lacks a key column
  /// (or, with uniqueNames, has it more than once). Edits stay local.
  case none(reason: String)
  /// A key part is NULL, NaN, infinite or too large to load: this row cannot
  /// be addressed
  case unusable(reason: String)

  public var description: String {
    switch self {
    case .none(let r), .unusable(let r): r
    }
  }
}

public final class RowKey: @unchecked Sendable {
  var raw: LaceRowKey
  public let columnNames: [String]
  public let values: [Value]

  public var count: Int { columnNames.count }

  /// The key of row `row` of result, by schema. With uniqueNames every key
  /// column must appear exactly once in the result (query results, where
  /// `SELECT a.id, b.id` would be ambiguous).
  public static func build(for row: Int, in result: ResultSet, schema: Schema, uniqueNames: Bool)
    throws -> RowKey
  {
    var key = LaceRowKey()
    var why: UnsafePointer<CChar>? = nil
    let status = lace_row_key_build(&key, result.raw, row, schema.raw, uniqueNames, &why)
    let reason = borrowCString(why) ?? ""
    switch status {
    case LACE_ROW_KEY_OK: return RowKey(owning: key)
    case LACE_ROW_KEY_BAD: throw RowKeyProblem.unusable(reason: reason)
    default: throw RowKeyProblem.none(reason: reason)
    }
  }

  init(owning key: LaceRowKey) {
    raw = key
    let n = Int(key.count)
    columnNames = (0..<n).map { key.names[$0].map { String(cString: $0) } ?? "" }
    values = (0..<n).map { Value(key.values[$0]) }
  }

  deinit {
    lace_row_key_free(&raw)
  }

  /// A deep copy (the key outlives the result it was built from already;
  /// this is for keeping one across a rebuild)
  public func copy() -> RowKey {
    var c = LaceRowKey()
    lace_row_key_copy(&c, &raw)
    return RowKey(owning: c)
  }

  /// The key with the part named column holding value (the row's key after
  /// an edit of a key column); a copy as it is when column is not a part
  public func replacing(column: String, with value: Value) -> RowKey {
    var c = LaceRowKey()
    lace_row_key_copy(&c, &raw)
    for i in 0..<Int(c.count) where c.names[i].map({ String(cString: $0) }) == column {
      lace_value_free(&c.values[i])
      c.values[i] = value.makeLaceValue()
    }
    return RowKey(owning: c)
  }
}

/// The columns of a row that a verification compares, as they were loaded
public final class RowSnapshot: @unchecked Sendable {
  var raw = LaceRowSnapshot()

  public var count: Int { Int(raw.count) }

  /// column: that one column of the result (a table-tab cell edit), or nil
  /// for every column of the result that belongs to the table (query
  /// results, deletes); values too large to load are skipped
  public init(result: ResultSet, row: Int, column: Int?, schema: Schema) {
    // SIZE_MAX means "every column" to liblace
    let col = column ?? Int(bitPattern: UInt.max)
    lace_row_snapshot_build(&raw, result.raw, row, col, schema.raw)
  }

  /// Columns and the values a verification expects to find in them (an
  /// undo: the value the edit wrote)
  public init(names: [String], values: [Value]) {
    precondition(names.count == values.count, "one value per column")
    let n = names.count
    // malloc'd: lace_row_snapshot_free() frees them with free()
    let cnames = calloc(max(n, 1), MemoryLayout<UnsafeMutablePointer<CChar>?>.stride)!
      .assumingMemoryBound(to: UnsafeMutablePointer<CChar>?.self)
    let cvalues = calloc(max(n, 1), MemoryLayout<LaceValue>.stride)!.assumingMemoryBound(to: LaceValue.self)
    for i in 0..<n {
      cnames[i] = strdup(names[i])
      cvalues[i] = values[i].makeLaceValue()
    }
    raw.names = cnames
    raw.values = cvalues
    raw.count = n
  }

  deinit {
    lace_row_snapshot_free(&raw)
  }
}
