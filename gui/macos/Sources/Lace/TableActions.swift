//
// Lace macOS frontend
// The table's actions (the Table menu, the strip's actions button, the
// tab's context menu): rename, truncate, drop, new table, new view, new
// function; a view tab's rename, drop and edit in a query view; a routine
// or trigger tab's drop and edit. Each through DdlRunner.perform.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

extension TableViewController {
  /// The items for this tab's object
  func tableActionsMenu() -> NSMenu {
    let menu = NSMenu()
    func add(_ title: String, _ sel: Selector) {
      menu.addItem(withTitle: title, action: sel, keyEquivalent: "").target = self
    }
    switch object.kind {
    case .table:
      add("Rename Table…", #selector(renameTable(_:)))
      add("Truncate Table…", #selector(truncateTable(_:)))
      add("Drop Table…", #selector(dropTable(_:)))
    case .view:
      add("Rename View…", #selector(renameTable(_:)))
      add("Drop View…", #selector(dropTable(_:)))
      add("Edit in a Query View", #selector(editInQuery(_:)))
    case .routine:
      add("Drop Routine…", #selector(dropTable(_:)))
      add("Edit in a Query View", #selector(editInQuery(_:)))
    case .trigger:
      add("Drop Trigger…", #selector(dropTable(_:)))
      add("Edit in a Query View", #selector(editInQuery(_:)))
    }
    menu.addItem(.separator())
    add("New Table…", #selector(newTable(_:)))
    add("New View…", #selector(newView(_:)))
    if connection.driver != .sqlite {
      add("New Function…", #selector(newFunction(_:)))
    }
    return menu
  }

  // MARK: Rename

  @objc func renameTable(_ sender: Any?) {
    let word = object.kind.word.lowercased()
    Task { [weak self] in
      guard let self else { return }
      guard let name = await promptName("Rename \(word) \(table)", "New name", initial: table), name != table else { return }
      do {
        let sql =
          object.kind == .view
          ? try Ddl.renameView(connection.driver, name: table, to: name)
          : try Ddl.renameTable(connection.driver, table: table, to: name)
        await DdlRunner.perform(
          DdlAction(
            title: object.kind == .view ? "Rename view" : "Rename table",
            summary: "Rename the \(word) \(table) to \(name).\nOpen tabs follow; query views keep their text.", sql: sql,
            after: .rename(name)), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  // MARK: Truncate

  @objc func truncateTable(_ sender: Any?) {
    do {
      let sql = try Ddl.truncate(connection.driver, table: table)
      Task {
        await DdlRunner.perform(
          DdlAction(title: "Truncate table", summary: "Delete every row of \(rowsText).", sql: sql, destructive: true), in: self)
      }
    } catch {
      DdlRunner.refused(error, in: self)
    }
  }

  // MARK: Drop

  @objc func dropTable(_ sender: Any?) {
    Task { [weak self] in
      guard let self else { return }
      await dropObject()
    }
  }

  /// "order_items (fk_items_order), invoices (...)"
  private func dependentsText(_ deps: Dependents?) -> String {
    (deps?.entries ?? []).map { d in d.name.map { "\(d.table) (\($0))" } ?? d.table }.joined(separator: ", ")
  }

  func dropObject() async {
    let driver = connection.driver
    let word = object.kind.word.lowercased()
    var deps = dependents
    if object.kind == .table, deps == nil {
      toast?("Checking what references \(table)…")
      deps = try? await connection.dependents(of: table)
      dependents = deps
    }
    let refs = dependentsText(deps)
    do {
      let sql: String
      switch object.kind {
      case .view: sql = try Ddl.dropView(driver, name: table)
      case .routine: sql = try Ddl.dropRoutine(driver, routine: object)
      case .trigger: sql = try Ddl.dropTrigger(driver, name: table, table: object.table)
      case .table: sql = try Ddl.dropTable(driver, table: table, cascade: false)
      }
      if object.kind == .table, driver == .postgres, !refs.isEmpty {
        // PostgreSQL refuses while others reference it: CASCADE, after a second confirmation
        let msg =
          "Drop table \(rowsText)?\nReferenced by \(refs): PostgreSQL drops the table only with CASCADE, which also drops those foreign keys. The next confirmation asks for it."
        guard await DdlRunner.confirm("Drop table", msg, action: "Continue", destructive: true, window: view.window) else { return }
        let cascade = try Ddl.dropTable(driver, table: table, cascade: true)
        await DdlRunner.perform(
          DdlAction(
            title: "Drop table", summary: "Drop table \(table) and the foreign keys of \(refs) with it (CASCADE).", sql: cascade,
            destructive: true, after: .drop), in: self)
        return
      }
      let summary: String
      if object.kind == .table {
        summary =
          refs.isEmpty
          ? "Drop table \(rowsText)."
          : "Drop table \(rowsText).\nReferenced by \(refs): the server may refuse, or drop their rows' keys."
      } else {
        summary = "Drop the \(word) \(table)."
      }
      await DdlRunner.perform(DdlAction(title: "Drop \(word)", summary: summary, sql: sql, destructive: true, after: .drop), in: self)
    } catch {
      DdlRunner.refused(error, in: self)
    }
  }

  // MARK: New table, view, function

  @objc func newTable(_ sender: Any?) {
    Task { [weak self] in
      guard let self else { return }
      guard let name = await promptName("New table", "Name") else { return }
      do {
        let sql = try Ddl.createTableWithID(connection.driver, table: name)
        await DdlRunner.perform(
          DdlAction(
            title: "New table", summary: "Create the table \(name) with one column, id, then open it on Structure.", sql: sql,
            after: .createTable(name)), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  @objc func newView(_ sender: Any?) {
    newFromTemplate(function: false)
  }

  @objc func newFunction(_ sender: Any?) {
    newFromTemplate(function: true)
  }

  private func newFromTemplate(function: Bool) {
    Task { [weak self] in
      guard let self else { return }
      guard let name = await promptName(function ? "New function" : "New view", "Name") else { return }
      do {
        let text =
          function ? try Ddl.routineTemplate(connection.driver, name: name) : try Ddl.viewTemplate(connection.driver, name: name)
        onNewQuery?(text)
        toast?("Edit the statement and run it (⌘↩); the sidebar reloads then")
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  @objc func editInQuery(_ sender: Any?) {
    editDefinitionInQuery()
  }
}
