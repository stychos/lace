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

  deinit {
    lace_row_snapshot_free(&raw)
  }
}
