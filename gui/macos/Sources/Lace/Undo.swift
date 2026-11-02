//
// Lace macOS frontend
// The undo stack of a grid (a table tab, a query's results): the cell edits
// and row deletes that succeeded, newest last, each reverted by one ⌘Z. The
// TUIs' rules (core/undo_stack.c, tui/notcurses/src/undo.c): a cell step is
// a verified UPDATE back whose snapshot is the value the edit wrote, so a
// row changed since is refused; a delete step INSERTs the rows as they were
// loaded (table columns only, generated ones left out). Runtime only: it
// lives with the view. EditController records and reverts.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

/// A deleted row as it was loaded: the table's columns the result held
struct UndoRow {
  let names: [String]
  let values: [Value]

  /// Row `row` of result: the columns of schema it holds, generated ones
  /// left out; omitted is set when a value was too large to load (the row
  /// cannot come back whole)
  static func capture(_ result: ResultSet, row: Int, schema: Schema, omitted: inout Bool) -> UndoRow {
    var names: [String] = []
    var values: [Value] = []
    for (i, c) in result.columns.enumerated() {
      guard let sc = schema.column(named: c.name), !sc.generated else { continue }
      let v = result.value(row: row, column: i)
      if v.isOmitted {
        omitted = true
      }
      names.append(c.name)
      values.append(v)
    }
    return UndoRow(names: names, values: values)
  }
}

enum UndoStep {
  /// A cell write: the row's key as it is after the edit, the column, the
  /// value before and the value written (what the verification expects)
  case cell(table: String, key: RowKey, column: String, before: Value, after: Value)
  /// Deleted rows; notRestorable says why they cannot come back, if so
  case delete(table: String, rows: [UndoRow], notRestorable: String?)

  /// "Edit of name", "Delete of 3 Rows": the Undo menu item's title after "Undo "
  var title: String {
    switch self {
    case .cell(_, _, let column, _, _): "Edit of \(column)"
    case .delete(_, let rows, _): rows.count == 1 ? "Delete of 1 Row" : "Delete of \(rows.count) Rows"
    }
  }
}

struct UndoStack {
  /// Steps kept per view; the oldest go first (the TUIs' UNDO_MAX)
  static let limit = 200

  private(set) var steps: [UndoStep] = []

  var top: UndoStep? { steps.last }
  var count: Int { steps.count }

  mutating func push(_ step: UndoStep) {
    if steps.count >= UndoStack.limit {
      steps.removeFirst(steps.count - UndoStack.limit + 1)
    }
    steps.append(step)
  }

  /// The newest step, taken off: it is consumed whether or not its revert lands
  mutating func pop() -> UndoStep? {
    steps.popLast()
  }

  mutating func clear() {
    steps.removeAll()
  }
}
