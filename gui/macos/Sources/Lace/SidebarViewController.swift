//
// Lace macOS frontend
// SidebarViewController: the connection's tables, searchable
//
// A single click previews a table (one preview tab, replaced by the next
// click); a double click or Return opens it for keeps. The context menu
// also shows a table's structure and starts a query on it.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class SidebarViewController: NSViewController, NSTableViewDataSource, NSTableViewDelegate,
  NSSearchFieldDelegate, NSMenuDelegate
{
  let connection: Connection
  var onPreview: ((String) -> Void)?
  var onOpen: ((String) -> Void)?
  var onStructure: ((String) -> Void)?
  var onNewQuery: ((String?) -> Void)?

  private let search = NSSearchField()
  private let tableView = KeyTableView()
  private let footer = NSTextField(labelWithString: "")
  private var tables: [String] = []
  private var filtered: [String] = []
  private var loadError: String?

  init(connection: Connection) {
    self.connection = connection
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 600))

    search.placeholderString = "Filter tables"
    search.delegate = self
    search.controlSize = .small
    search.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    search.translatesAutoresizingMaskIntoConstraints = false

    let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("table"))
    column.resizingMask = .autoresizingMask
    tableView.addTableColumn(column)
    tableView.headerView = nil
    tableView.dataSource = self
    tableView.delegate = self
    tableView.style = .sourceList
    tableView.rowHeight = 22
    tableView.allowsEmptySelection = true
    tableView.target = self
    tableView.action = #selector(clicked)
    tableView.doubleAction = #selector(doubleClicked)
    tableView.onReturn = { [weak self] in self?.openSelected() }
    let menu = NSMenu()
    menu.delegate = self
    tableView.menu = menu
    let scroll = NSScrollView()
    scroll.documentView = tableView
    scroll.hasVerticalScroller = true
    scroll.drawsBackground = false
    scroll.borderType = .noBorder
    scroll.translatesAutoresizingMaskIntoConstraints = false
    scroll.setContentHuggingPriority(.defaultLow, for: .vertical)

    footer.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    footer.textColor = .secondaryLabelColor
    footer.lineBreakMode = .byTruncatingTail
    let reload = NSButton(
      image: NSImage(systemSymbolName: "arrow.clockwise", accessibilityDescription: "Reload")!, target: self,
      action: #selector(reloadTapped))
    reload.isBordered = false
    reload.toolTip = "Reload the table list"
    let query = NSButton(
      image: NSImage(systemSymbolName: "terminal", accessibilityDescription: "New query")!, target: self,
      action: #selector(newQueryTapped))
    query.isBordered = false
    query.toolTip = "New SQL query (⌘E)"
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let footerRow = NSStackView(views: [footer, spacer, reload, query])
    footerRow.orientation = .horizontal
    footerRow.alignment = .centerY
    footerRow.spacing = 6
    footerRow.edgeInsets = NSEdgeInsets(top: 4, left: 10, bottom: 6, right: 8)

    let stack = NSStackView(views: [search, scroll, footerRow])
    stack.orientation = .vertical
    stack.alignment = .width
    stack.spacing = 6
    stack.edgeInsets = NSEdgeInsets(top: 8, left: 0, bottom: 0, right: 0)
    stack.translatesAutoresizingMaskIntoConstraints = false
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
      search.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 10),
      search.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -10),
    ])
    view = root
  }

  override func viewDidLoad() {
    super.viewDidLoad()
    reload()
  }

  func focus() {
    view.window?.makeFirstResponder(search)
  }

  func reload() {
    footer.stringValue = "Loading…"
    let connection = self.connection
    Task { [weak self] in
      let outcome = await Result { try await connection.tables() }
      guard let self else { return }
      switch outcome {
      case .success(let names):
        tables = names
        loadError = nil
      case .failure(let error):
        loadError = "\(error)"
      }
      applyFilter()
    }
  }

  private func applyFilter() {
    let q = search.stringValue.trimmingCharacters(in: .whitespaces)
    filtered = q.isEmpty ? tables : tables.filter { $0.localizedCaseInsensitiveContains(q) }
    tableView.reloadData()
    if let loadError {
      footer.stringValue = loadError
      footer.toolTip = loadError
    } else if q.isEmpty {
      footer.stringValue = Format.plural(tables.count, "table")
      footer.toolTip = nil
    } else {
      footer.stringValue = "\(Format.count(filtered.count)) of \(Format.plural(tables.count, "table"))"
      footer.toolTip = nil
    }
  }

  private var selectedTable: String? {
    let row = tableView.selectedRow
    return row >= 0 && row < filtered.count ? filtered[row] : nil
  }

  private var clickedTable: String? {
    let row = tableView.clickedRow
    return row >= 0 && row < filtered.count ? filtered[row] : nil
  }

  private func openSelected() {
    if let t = selectedTable {
      onOpen?(t)
    }
  }

  @objc private func clicked() {
    if let t = clickedTable {
      onPreview?(t)
    }
  }

  @objc private func doubleClicked() {
    if let t = clickedTable {
      onOpen?(t)
    }
  }

  @objc private func reloadTapped() {
    reload()
  }

  @objc private func newQueryTapped() {
    onNewQuery?(nil)
  }

  @objc private func menuOpen() {
    if let t = clickedTable ?? selectedTable {
      onOpen?(t)
    }
  }

  @objc private func menuStructure() {
    if let t = clickedTable ?? selectedTable {
      onStructure?(t)
    }
  }

  @objc private func menuQuery() {
    guard let t = clickedTable ?? selectedTable,
      let quoted = SQL.quoteTable(t, driver: connection.driver)
    else { return }
    onNewQuery?("SELECT * FROM \(quoted)")
  }

  // MARK: Table

  func numberOfRows(in tableView: NSTableView) -> Int { filtered.count }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    IconTextCellView.make(in: tableView, symbol: "tablecells", text: filtered[row])
  }

  func controlTextDidChange(_ obj: Notification) {
    applyFilter()
  }

  func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
    // Return in the search field opens the first match; Down moves to the list
    if commandSelector == #selector(NSResponder.insertNewline(_:)) {
      if let first = filtered.first {
        onOpen?(first)
      }
      return true
    }
    if commandSelector == #selector(NSResponder.moveDown(_:)), !filtered.isEmpty {
      tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
      view.window?.makeFirstResponder(tableView)
      return true
    }
    return false
  }

  func menuNeedsUpdate(_ menu: NSMenu) {
    menu.removeAllItems()
    guard clickedTable != nil else { return }
    menu.addItem(withTitle: "Open", action: #selector(menuOpen), keyEquivalent: "").target = self
    menu.addItem(withTitle: "Structure…", action: #selector(menuStructure), keyEquivalent: "").target = self
    menu.addItem(withTitle: "Query This Table", action: #selector(menuQuery), keyEquivalent: "").target = self
  }
}

/// A table view whose Return key opens the selection
final class KeyTableView: NSTableView {
  var onReturn: (() -> Void)?

  override func keyDown(with event: NSEvent) {
    if event.keyCode == 36 || event.keyCode == 76 {
      onReturn?()
      return
    }
    super.keyDown(with: event)
  }
}
