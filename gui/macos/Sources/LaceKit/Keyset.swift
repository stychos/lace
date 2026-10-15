//
// Lace macOS frontend
// Keyset paging: liblace's ordering for a table (the sort columns, then
// the primary key, so the order is total), the ORDER BY every page uses,
// and the WHERE for the rows after or before an edge row, so a page next
// to a loaded one costs the same anywhere in the table.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class Keyset {
  private let raw: OpaquePointer
  public let schema: Schema

  /// nil when the table has no primary key, a sort column is not in the
  /// schema, or a column cannot be compared exactly (a floating-point column
  /// on PostgreSQL or MySQL)
  public init?(schema: Schema, sorts: [SortSpec], driver: Driver) {
    var cs: [LaceSort] = []
    for s in sorts {
      guard let i = schema.index(ofColumn: s.column) else { return nil }
      cs.append(LaceSort(column: i, dir: s.descending ? LACE_SORT_DESC : LACE_SORT_ASC))
    }
    guard let ks = cs.withUnsafeBufferPointer({ lace_keyset_create(schema.raw, $0.baseAddress, $0.count, driver.c) })
    else { return nil }
    raw = ks
    self.schema = schema
  }

  deinit {
    lace_keyset_free(raw)
  }

  /// Sort columns plus key columns
  public var columnCount: Int { lace_keyset_num_columns(raw) }

  /// ORDER BY body: the table's order, or its exact reverse (then reverse
  /// the rows read)
  public func orderBy(forward: Bool = true) -> String? {
    takeCString(lace_keyset_build_order(raw, forward))
  }

  /// The condition for the rows after (forward) or before `row` of `rows`;
  /// nil when that row cannot be a boundary (a keyset column missing from
  /// the result, a value too large to load, NaN or infinite)
  public func whereClause(rows: ResultSet, row: Int, forward: Bool) -> String? {
    guard row >= 0, row < rows.rowCount else { return nil }
    return takeCString(lace_keyset_build_where(raw, rows.raw, row, forward))
  }
}

extension ResultSet {
  /// The same rows in reverse order (a page read backwards)
  public func reversed() -> ResultSet {
    ResultSet(columns: columns, rows: rows.reversed())
  }
}
