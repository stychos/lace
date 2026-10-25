//
// Lace macOS frontend
// The sub-views of a table tab: which there are for an object, and the list
// that shows one of them (Structure, Indexes, Foreign keys, Information, or
// a Definition: the rows come from LaceKit's CatalogTable, the shared
// builders of core/catalog_rows.c). Return or a double click on a row hands
// its tag to the owner.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

enum SubView: Int, CaseIterable {
  case data = 0, structure, indexes, foreignKeys, information

  /// The session's name; "definition" for an object whose Structure slot is its SQL
  func key(for kind: ObjectKind) -> String {
    switch self {
    case .data: "data"
    case .structure: kind == .table ? "structure" : "definition"
    case .indexes: "indexes"
    case .foreignKeys: "foreign_keys"
    case .information: "information"
    }
  }

  init?(key: String) {
    switch key {
    case "data": self = .data
    case "structure", "definition": self = .structure
    case "indexes": self = .indexes
    case "foreign_keys": self = .foreignKeys
    case "information": self = .information
    default: return nil
    }
  }

  func label(for kind: ObjectKind) -> String {
    switch self {
    case .data: "Data"
    case .structure: kind == .table ? "Structure" : "Definition"
    case .indexes: "Indexes"
    case .foreignKeys: "Foreign Keys"
    case .information: "Information"
    }
  }

  /// The sub-views an object has: a table all five, a database view Data,
  /// Definition and Information, a routine or trigger Definition and Information
  static func available(for kind: ObjectKind) -> [SubView] {
    switch kind {
    case .table: SubView.allCases
    case .view: [.data, .structure, .information]
    case .routine, .trigger: [.structure, .information]
    }
  }
}

/// One sub-view's list
@MainActor
final class CatalogListController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
  let scrollView = NSScrollView()
  let tableView = KeyTableView()
  /// Return or a double click on a row with a tag
  var onActivate: ((Int) -> Void)?
  private(set) var table: CatalogTable?
  private var message: String?
  private let messageLabel = NSTextField(labelWithString: "")
  private let monospace: Bool

  init(monospace: Bool = false) {
    self.monospace = monospace
    super.init()
    tableView.dataSource = self
    tableView.delegate = self
    tableView.usesAlternatingRowBackgroundColors = true
    tableView.allowsColumnReordering = false
    tableView.allowsEmptySelection = true
    tableView.allowsMultipleSelection = false
    tableView.rowHeight = 20
    tableView.style = .plain
    tableView.columnAutoresizingStyle = .noColumnAutoresizing
    tableView.target = self
    tableView.doubleAction = #selector(doubleClicked(_:))
    tableView.onReturn = { [weak self] in self?.activateSelected() }
    scrollView.documentView = tableView
    scrollView.hasVerticalScroller = true
    scrollView.hasHorizontalScroller = true
    scrollView.autohidesScrollers = true
    scrollView.borderType = .noBorder
    messageLabel.textColor = .secondaryLabelColor
    messageLabel.alignment = .center
    messageLabel.translatesAutoresizingMaskIntoConstraints = false
    scrollView.addSubview(messageLabel)
    NSLayoutConstraint.activate([
      messageLabel.centerXAnchor.constraint(equalTo: scrollView.centerXAnchor),
      messageLabel.centerYAnchor.constraint(equalTo: scrollView.centerYAnchor),
      messageLabel.widthAnchor.constraint(lessThanOrEqualTo: scrollView.widthAnchor, constant: -40),
    ])
  }

  /// Show the rows (nil with a message: loading, an error, nothing)
  func show(_ table: CatalogTable?, message: String? = nil) {
    self.table = table
    self.message = message
    for c in tableView.tableColumns {
      tableView.removeTableColumn(c)
    }
    let ncols = max(table?.head?.count ?? table?.rows.first?.cells.count ?? 1, 1)
    for i in 0..<ncols {
      let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("c\(i)"))
      c.title = table?.head?[i] ?? ""
      c.minWidth = 40
      c.maxWidth = 4000
      c.width = width(ofColumn: i, in: table)
      tableView.addTableColumn(c)
    }
    tableView.headerView = table?.head == nil ? nil : NSTableHeaderView()
    tableView.reloadData()
    messageLabel.stringValue = message ?? ""
    messageLabel.isHidden = table != nil
  }

  /// The rows' widest text, measured in the font used, within bounds
  private func width(ofColumn i: Int, in table: CatalogTable?) -> CGFloat {
    guard let table else { return 200 }
    let font = cellFont
    var w: CGFloat = table.head.map { h in (h[i] as NSString).size(withAttributes: [.font: NSFont.boldSystemFont(ofSize: NSFont.smallSystemFontSize)]).width } ?? 0
    let last = i == (table.head?.count ?? table.rows.first?.cells.count ?? 1) - 1
    for r in table.rows where !r.span && i < r.cells.count {
      w = max(w, (r.cells[i] as NSString).size(withAttributes: [.font: font]).width)
    }
    for r in table.rows where r.span && i == 0 {
      w = max(w, (r.cells[0] as NSString).size(withAttributes: [.font: font]).width)
    }
    return min(max(w + 16, 60), last ? 1200 : 480)
  }

  private var cellFont: NSFont {
    monospace
      ? .monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
      : .systemFont(ofSize: NSFont.smallSystemFontSize)
  }

  /// A row's text, for tests and the clipboard
  func rowText(_ row: Int) -> String? {
    guard let table, row < table.rows.count else { return nil }
    let r = table.rows[row]
    return r.span ? r.cells.first : r.cells.joined(separator: "  ")
  }

  var rowCount: Int { table?.rows.count ?? 0 }

  func selectRow(_ row: Int) {
    guard let table, row < table.rows.count, !table.rows[row].heading else { return }
    tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
    tableView.scrollRowToVisible(row)
  }

  func focus() {
    tableView.window?.makeFirstResponder(tableView)
  }

  private func activateSelected() {
    let row = tableView.selectedRow
    guard let table, row >= 0, row < table.rows.count else { return }
    onActivate?(table.rows[row].tag)
  }

  @objc private func doubleClicked(_ sender: Any?) {
    let row = tableView.clickedRow
    guard let table, row >= 0, row < table.rows.count, !table.rows[row].heading else { return }
    onActivate?(table.rows[row].tag)
  }

  // MARK: Table

  func numberOfRows(in tableView: NSTableView) -> Int { table?.rows.count ?? 0 }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    guard let table, let tableColumn, row < table.rows.count,
      let index = tableView.tableColumns.firstIndex(of: tableColumn)
    else { return nil }
    let r = table.rows[row]
    let text = r.span ? (index == 0 ? r.cells.first ?? "" : "") : (index < r.cells.count ? r.cells[index] : "")
    let cell = TextCellView.make(in: tableView, text: text)
    if r.heading {
      cell.label.font = r.span ? .boldSystemFont(ofSize: NSFont.systemFontSize) : .boldSystemFont(ofSize: NSFont.smallSystemFontSize)
      cell.label.textColor = r.span ? .controlAccentColor : .secondaryLabelColor
    } else {
      cell.label.font = cellFont
    }
    return cell
  }

  func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
    guard let table, row < table.rows.count else { return false }
    return !table.rows[row].heading
  }

  func tableView(_ tableView: NSTableView, isGroupRow row: Int) -> Bool {
    false
  }
}
