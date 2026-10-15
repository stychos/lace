//
// Lace macOS frontend
// EditController: the one write path for grids (table tabs and query
// results): verified cell updates and row deletes
//
// Value.edit decides whether anything is written (unchanged text and a NULL
// cell left empty write nothing; empty text is ''; only the NULL action
// writes NULL). The row's key is built by column name, a snapshot of the
// columns to verify is taken, and Connection.update/delete re-read the row
// with the same WHERE before writing: a row changed elsewhere is refused.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
final class EditController {
  let connection: Connection
  let model: RowModel
  unowned let grid: GridController
  /// Set by the owner once the schema is known
  var table: String?
  var schema: Schema?
  /// Key columns must appear once in the result (query results)
  var uniqueNames = false
  /// Verify every loaded table column, not only the edited one (query results)
  var snapshotAll = false
  var confirmDelete = true
  var toast: ((String) -> Void)?
  var onRowsDeleted: ((Int) -> Void)?
  /// The SQL liblace sent, for history
  var onWrite: ((String) -> Void)?
  weak var hostView: NSView?

  init(connection: Connection, model: RowModel, grid: GridController) {
    self.connection = connection
    self.model = model
    self.grid = grid
  }

  private var window: NSWindow? { hostView?.window }

  private var readOnlyReason: String {
    grid.readOnlyReason ?? "this result cannot be edited"
  }

  /// Write one cell; text nil is the NULL action
  func editCell(row: Int, column: Int, text: String?) {
    guard grid.isEditable, let schema, let table else {
      toast?("Read only: \(readOnlyReason)")
      return
    }
    guard let (page, index) = model.rowResult(row), column < model.columns.count else { return }
    let modelColumn = model.columns[column]
    let original = page.value(row: index, column: column)
    if original.isOmitted {
      toast?("This value is too large to load and cannot be edited")
      return
    }
    guard let tableColumn = schema.column(named: modelColumn.name) else {
      toast?("\(modelColumn.name) is not a column of \(table)")
      return
    }
    let outcome: Value.EditOutcome
    do {
      outcome = try Value.edit(original: original, columnType: tableColumn.type, text: text)
    } catch {
      Alerts.error("Not updated", "\(error)", window: window)
      return
    }
    guard case .write(let value) = outcome else { return }
    let key: RowKey
    do {
      key = try RowKey.build(for: index, in: page, schema: schema, uniqueNames: uniqueNames)
    } catch {
      Alerts.error("Not updated", "\(error)", window: window)
      return
    }
    let snapshot = RowSnapshot(result: page, row: index, column: snapshotAll ? nil : column, schema: schema)
    let generation = model.generation
    let name = modelColumn.name
    let connection = self.connection
    Task { [weak self] in
      do {
        let sql = try await connection.update(
          table: table, key: key, snapshot: snapshot, column: name, value: value)
        guard let self else { return }
        if generation == model.generation {
          model.setCell(row: row, column: column, value: value)
        }
        toast?("Updated \(name)")
        if let sql {
          onWrite?(sql)
        }
      } catch {
        guard let self else { return }
        Alerts.error("Not updated", "\(error)", window: window)
      }
    }
  }

  /// Delete the grid's selected rows (or the focused row), asking first
  /// when confirmDelete is set
  func deleteSelected() {
    guard grid.isEditable, schema != nil, let table else {
      toast?("Read only: \(readOnlyReason)")
      return
    }
    var rows = grid.selectedRows
    if rows.isEmpty {
      guard let focused = grid.focusedCell else { return }
      rows = [focused.row]
    }
    let rowsToDelete = rows
    Task { [weak self] in
      guard let self else { return }
      if confirmDelete {
        let ok = await Alerts.confirm(
          "Delete rows",
          "Delete \(Format.plural(rowsToDelete.count, "row")) from \(table)? This cannot be undone.",
          action: "Delete", window: window)
        guard ok else { return }
      }
      await deleteNow(rowsToDelete)
    }
  }

  private func deleteNow(_ rows: [Int]) async {
    guard let schema, let table else { return }
    var jobs: [(RowKey, RowSnapshot)] = []
    var firstError: String?
    for row in rows {
      guard let (page, index) = model.rowResult(row) else { continue }
      do {
        let key = try RowKey.build(for: index, in: page, schema: schema, uniqueNames: uniqueNames)
        jobs.append((key, RowSnapshot(result: page, row: index, column: nil, schema: schema)))
      } catch {
        if firstError == nil {
          firstError = "\(error)"
        }
      }
    }
    var deleted = 0
    for (key, snapshot) in jobs {
      do {
        let sql = try await connection.delete(table: table, key: key, snapshot: snapshot)
        deleted += 1
        if let sql {
          onWrite?(sql)
        }
      } catch {
        if firstError == nil {
          firstError = "\(error)"
        }
      }
    }
    if deleted > 0 {
      grid.unselectAll()
      model.rowsRemoved(count: deleted)
      toast?("Deleted \(Format.plural(deleted, "row"))")
      onRowsDeleted?(deleted)
    }
    if let firstError {
      Alerts.error("Not deleted", firstError, window: window)
    }
  }
}
