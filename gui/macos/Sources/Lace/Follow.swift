//
// Lace macOS frontend
// Following foreign keys from a grid: the referenced table filtered by the
// focused row's key (follow), and a table whose key points at the row
// (referenced by, a menu of the dependents). Shared by table tabs and
// query results that are one table's rows; the owner opens the target.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
enum Follow {
  /// The focused row's values of the named columns as edit text; nil with
  /// the reason when one is missing, NULL or not loaded
  static func values(of columns: [String], grid: GridController) -> Result<[String], FollowProblem> {
    guard let (row, _) = grid.focusedCell else { return .failure(FollowProblem("No row to follow")) }
    let model = grid.model
    var out: [String] = []
    for name in columns {
      guard let c = model.columns.firstIndex(where: { $0.name == name }) else {
        return .failure(FollowProblem("\(name) is not in the results"))
      }
      guard let v = model.value(row: row, column: c) else { return .failure(FollowProblem("\(name) is not loaded yet")) }
      if v.isNull { return .failure(FollowProblem("\(name) is NULL: nothing to follow")) }
      if v.isOmitted { return .failure(FollowProblem("\(name) is too large to load")) }
      out.append(v.editText)
    }
    return .success(out)
  }

  /// The keys the focused column starts
  static func keys(of grid: GridController, schema: Schema?) -> [ForeignKey] {
    guard let schema, let (_, c) = grid.focusedCell, c < grid.model.columns.count else { return [] }
    let name = grid.model.columns[c].name
    return schema.foreignKeys.filter { $0.columns.first == name }
  }

  /// Follow one key: the filters for the referenced table
  static func filters(for key: ForeignKey, grid: GridController) -> Result<(table: String, filters: [Filter]), FollowProblem> {
    guard let table = key.refTable, key.refColumns.count == key.columns.count else {
      return .failure(FollowProblem("The foreign key \(key.name ?? "") is incomplete"))
    }
    switch values(of: key.columns, grid: grid) {
    case .failure(let why): return .failure(why)
    case .success(let vals):
      return .success((table, zip(key.refColumns, vals).map { Filter(column: $0, op: .eq, value: $1) }))
    }
  }

  /// A dependent's filters: its key columns equal the focused row's referenced values
  static func filters(for dep: Dependent, grid: GridController) -> Result<(table: String, filters: [Filter]), FollowProblem> {
    switch values(of: dep.refColumns, grid: grid) {
    case .failure(let why): return .failure(why)
    case .success(let vals):
      return .success((dep.table, zip(dep.columns, vals).map { Filter(column: $0, op: .eq, value: $1) }))
    }
  }

  /// Follow from the focused cell: one key goes at once, several ask with a
  /// menu at the cell; open gets the table and its filters
  static func follow(
    grid: GridController, schema: Schema?, toast: ((String) -> Void)?,
    open: @escaping (String, [Filter]) -> Void
  ) {
    guard schema != nil else {
      toast?("The table's structure is not loaded")
      return
    }
    let keys = keys(of: grid, schema: schema)
    guard !keys.isEmpty else {
      let name = grid.focusedCell.map { grid.model.columns[$0.column].name } ?? "This column"
      toast?("\(name) is not a foreign key column")
      return
    }
    func go(_ key: ForeignKey) {
      switch filters(for: key, grid: grid) {
      case .failure(let why): toast?(why.text)
      case .success(let target): open(target.table, target.filters)
      }
    }
    if keys.count == 1 {
      go(keys[0])
      return
    }
    let menu = NSMenu()
    for key in keys {
      let title = "\(key.refTable ?? "?") (\(key.refColumns.joined(separator: ", ")))" + (key.name.map { "  \($0)" } ?? "")
      let item = menu.addItem(withTitle: title, action: #selector(MenuTarget.pick(_:)), keyEquivalent: "")
      let target = MenuTarget { go(key) }
      item.target = target
      item.representedObject = target
    }
    popUp(menu, grid: grid)
  }

  /// The tables referencing the row's table: a menu of them at the cell
  static func referencedBy(
    grid: GridController, dependents: Dependents, table: String, toast: ((String) -> Void)?,
    open: @escaping (String, [Filter]) -> Void
  ) {
    guard !dependents.entries.isEmpty else {
      toast?("No table references \(table)")
      return
    }
    let menu = NSMenu()
    for dep in dependents.entries {
      var title = "\(dep.table) (\(dep.columns.joined(separator: ", ")))"
      if let od = dep.onDelete { title += "  ON DELETE \(od)" }
      let item = menu.addItem(withTitle: title, action: #selector(MenuTarget.pick(_:)), keyEquivalent: "")
      let target = MenuTarget {
        switch filters(for: dep, grid: grid) {
        case .failure(let why): toast?(why.text)
        case .success(let t): open(t.table, t.filters)
        }
      }
      item.target = target
      item.representedObject = target
    }
    popUp(menu, grid: grid)
  }

  private static func popUp(_ menu: NSMenu, grid: GridController) {
    let view = grid.tableView
    var at = NSPoint(x: 20, y: 20)
    if let (row, _) = grid.focusedCell {
      let rect = view.rect(ofRow: row)
      at = NSPoint(x: rect.minX + 40, y: rect.maxY)
    }
    menu.popUp(positioning: nil, at: at, in: view)
  }
}

/// Why a key cannot be followed, said to the user
struct FollowProblem: Error {
  let text: String
  init(_ text: String) { self.text = text }
}

/// A menu item's action as a closure (the item keeps it alive)
final class MenuTarget: NSObject {
  private let body: () -> Void
  init(_ body: @escaping () -> Void) { self.body = body }
  @objc func pick(_ sender: Any?) { body() }
}
