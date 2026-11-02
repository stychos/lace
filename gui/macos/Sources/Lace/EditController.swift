//
// Lace macOS frontend
// EditController: the one write path for grids (table tabs and query
// results): verified cell updates and row deletes, and their undo
//
// Value.edit decides whether anything is written (unchanged text and a NULL
// cell left empty write nothing; empty text is ''; only the NULL action
// writes NULL). The row's key is built by column name, a snapshot of the
// columns to verify is taken, and Connection.update/delete re-read the row
// with the same WHERE before writing: a row changed elsewhere is refused.
// Every write that succeeded goes on the undo stack (Undo.swift); ⌘Z in the
// grid reverts the newest one through the same verified path.
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
  /// An undo wrote something: the owner reloads what it shows
  var onUndone: (() -> Void)?
  weak var hostView: NSView?
  /// The writes that can be reverted, newest last
  private(set) var undo = UndoStack()

  init(connection: Connection, model: RowModel, grid: GridController) {
    self.connection = connection
    self.model = model
    self.grid = grid
    grid.undoTitle = { [weak self] in self?.undo.top?.title }
    grid.onUndo = { [weak self] in self?.undoLast() }
  }

  /// The view's table changed shape (a structure change): its steps go
  func clearUndo() {
    undo.clear()
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
        // The row's key as it is now (an edited key column holds the new value)
        undo.push(
          .cell(table: table, key: key.replacing(column: name, with: value), column: name, before: original, after: value))
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
          "Delete \(Format.plural(rowsToDelete.count, "row")) from \(table)? Undo (⌘Z) inserts them again, but not rows the database removes with them.",
          action: "Delete", window: window)
        guard ok else { return }
      }
      await deleteNow(rowsToDelete)
    }
  }

  private func deleteNow(_ rows: [Int]) async {
    guard let schema, let table else { return }
    // Each row as it is on screen, for the undo step, taken before the DELETE
    var jobs: [(RowKey, RowSnapshot, UndoRow)] = []
    var omitted = false
    var firstError: String?
    for row in rows {
      guard let (page, index) = model.rowResult(row) else { continue }
      do {
        let key = try RowKey.build(for: index, in: page, schema: schema, uniqueNames: uniqueNames)
        jobs.append(
          (
            key, RowSnapshot(result: page, row: index, column: nil, schema: schema),
            UndoRow.capture(page, row: index, schema: schema, omitted: &omitted)
          ))
      } catch {
        if firstError == nil {
          firstError = "\(error)"
        }
      }
    }
    var deleted = 0
    var gone: [UndoRow] = []
    for (key, snapshot, captured) in jobs {
      do {
        let sql = try await connection.delete(table: table, key: key, snapshot: snapshot)
        deleted += 1
        gone.append(captured)
        if let sql {
          onWrite?(sql)
        }
      } catch {
        if firstError == nil {
          firstError = "\(error)"
        }
      }
    }
    if !gone.isEmpty {
      // The rows that went make one step, which brings them all back
      undo.push(
        .delete(
          table: table, rows: gone, notRestorable: omitted ? "a value of the rows was too large to load" : nil))
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

  // MARK: Undo

  /// Revert the newest write: a cell back to its old value (verified: the
  /// row must still hold what the edit wrote), or the deleted rows INSERTed
  /// again; then the owner reloads. The step is consumed either way.
  func undoLast() {
    guard undo.top != nil else {
      toast?("Nothing to undo")
      return
    }
    // As the TUIs: a view that cannot be edited now (query results re-run
    // as a join) refuses, and the step stays for when it can
    guard grid.isEditable else {
      toast?("Not undone: \(readOnlyReason)")
      return
    }
    guard let step = undo.pop() else { return }
    let connection = self.connection
    switch step {
    case .cell(let table, let key, let column, let before, let after):
      let snapshot = RowSnapshot(names: [column], values: [after])
      Task { [weak self] in
        do {
          let sql = try await connection.update(table: table, key: key, snapshot: snapshot, column: column, value: before)
          guard let self else { return }
          if let sql {
            onWrite?(sql)
          }
          toast?("Undone: \(column) back to \(before.isNull ? "NULL" : before.displayText(maxChars: 40))")
          onUndone?()
        } catch {
          Alerts.error("Not undone", "\(error)", window: self?.window)
        }
      }
    case .delete(let table, let rows, let notRestorable):
      if let notRestorable {
        // Dropped: it can never succeed
        Alerts.error(
          rows.count == 1 ? "Cannot restore the deleted row" : "Cannot restore the deleted rows", notRestorable,
          window: window)
        return
      }
      Task { [weak self] in
        var restored = 0
        var firstError: String?
        for r in rows {
          do {
            let sql = try await connection.insert(into: table, columns: r.names, values: r.values)
            restored += 1
            if let sql {
              self?.onWrite?(sql)
            }
          } catch {
            if firstError == nil {
              firstError = "\(error)"
            }
          }
        }
        guard let self else { return }
        if let firstError {
          Alerts.error("Not undone", firstError, window: window)
        } else {
          toast?("Restored \(Format.plural(restored, "row"))")
        }
        if restored > 0 {
          onUndone?()
          await warnCascade(table: table, restored: restored)
        }
      }
    }
  }

  /// What the delete took with it stays gone: name the referencing tables
  /// whose rows the database removed or changed (CASCADE, SET NULL, SET DEFAULT)
  private func warnCascade(table: String, restored: Int) async {
    guard let deps = try? await connection.dependents(of: table) else { return }
    var names: [String] = []
    for d in deps.entries where ["CASCADE", "SET NULL", "SET DEFAULT"].contains(d.onDelete ?? "") {
      if !names.contains(d.table) {
        names.append(d.table)
      }
    }
    guard !names.isEmpty else { return }
    Alerts.error(
      "Rows not restored",
      "Restored \(Format.plural(restored, "row")); rows of \(names.joined(separator: ", ")) the database removed or changed with them are not restored.",
      window: window)
  }
}
