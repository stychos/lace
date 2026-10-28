//
// Lace macOS frontend
// SidebarViewController: the connection's tables, then its views, functions
// and triggers in sections that fold, searchable
//
// A single click previews an entry (one preview tab, replaced by the next
// click); a double click or Return opens it for keeps; Return or a click on
// a section header folds it. The context menu also opens an object's
// structure and starts a query on a table.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class SidebarViewController: NSViewController, NSTableViewDataSource, NSTableViewDelegate,
  NSSearchFieldDelegate, NSMenuDelegate
{
  let connection: Connection
  var onPreview: ((CatalogObject) -> Void)?
  var onOpen: ((CatalogObject) -> Void)?
  var onStructure: ((CatalogObject) -> Void)?
  var onNewQuery: ((String?) -> Void)?
  /// The table list arrived (every reload)
  var onLoaded: (() -> Void)?

  /// A row of the list: a section header (folds), or an object
  enum Row: Equatable {
    case header(ObjectKind)
    case object(CatalogObject)
  }

  private let search = NSSearchField()
  private let tableView = KeyTableView()
  private let footer = NSTextField(labelWithString: "")
  private(set) var tables: [String] = []
  /// The views, routines and triggers (lace_list_objects)
  private(set) var objects: [CatalogObject] = []
  private var filtered: [Row] = []
  private var collapsed: Set<ObjectKind> = []
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
      // the other sections come with the tables; a catalog an older server
      // cannot answer leaves them empty
      let objs = (try? await connection.objects()) ?? []
      guard let self else { return }
      switch outcome {
      case .success(let names):
        tables = names
        objects = objs
        loadError = nil
      case .failure(let error):
        loadError = "\(error)"
      }
      applyFilter()
      onLoaded?()
    }
  }

  private func applyFilter() {
    let q = search.stringValue.trimmingCharacters(in: .whitespaces)
    func matches(_ name: String) -> Bool { q.isEmpty || name.localizedCaseInsensitiveContains(q) }
    var rows: [Row] = tables.filter(matches).map { .object(CatalogObject(kind: .table, name: $0)) }
    for kind in [ObjectKind.view, .routine, .trigger] {
      let entries = objects.filter { $0.kind == kind && matches($0.name) }
      guard !entries.isEmpty else { continue }
      rows.append(.header(kind))
      if !collapsed.contains(kind) {
        rows.append(contentsOf: entries.map { .object($0) })
      }
    }
    filtered = rows
    tableView.reloadData()
    let shown = tables.filter(matches).count
    if let loadError {
      footer.stringValue = loadError
      footer.toolTip = loadError
    } else if q.isEmpty {
      footer.stringValue = Format.plural(tables.count, "table") + (objects.isEmpty ? "" : ", \(Format.plural(objects.count, "object"))")
      footer.toolTip = nil
    } else {
      footer.stringValue = "\(Format.count(shown)) of \(Format.plural(tables.count, "table"))"
      footer.toolTip = nil
    }
  }

  /// The rows' text, top to bottom (the script driver)
  var rowTexts: [String] {
    filtered.map {
      switch $0 {
      case .header(let kind): "\(collapsed.contains(kind) ? "▸" : "▾") \(Self.title(of: kind))"
      case .object(let o): o.name
      }
    }
  }

  private static func title(of kind: ObjectKind) -> String {
    switch kind {
    case .table: "TABLES"
    case .view: "VIEWS"
    case .routine: "FUNCTIONS"
    case .trigger: "TRIGGERS"
    }
  }

  private func row(at index: Int) -> Row? { index >= 0 && index < filtered.count ? filtered[index] : nil }

  private var selectedObject: CatalogObject? {
    if case .object(let o) = row(at: tableView.selectedRow) { return o }
    return nil
  }

  private var clickedObject: CatalogObject? {
    if case .object(let o) = row(at: tableView.clickedRow) { return o }
    return nil
  }

  /// Fold or unfold a section (its header row)
  func toggle(_ kind: ObjectKind) {
    if collapsed.contains(kind) { collapsed.remove(kind) } else { collapsed.insert(kind) }
    applyFilter()
  }

  /// The entry with that name and kind, for scripts: open it
  func open(kind: ObjectKind, name: String) -> Bool {
    if kind == .table, tables.contains(name) {
      onOpen?(CatalogObject(kind: .table, name: name))
      return true
    }
    guard let o = objects.first(where: { $0.kind == kind && $0.name == name }) else { return false }
    onOpen?(o)
    return true
  }

  private func openSelected() {
    switch row(at: tableView.selectedRow) {
    case .header(let kind): toggle(kind)
    case .object(let o): onOpen?(o)
    case nil: break
    }
  }

  @objc private func clicked() {
    switch row(at: tableView.clickedRow) {
    case .header(let kind): toggle(kind)
    case .object(let o): onPreview?(o)
    case nil: break
    }
  }

  @objc private func doubleClicked() {
    if let o = clickedObject {
      onOpen?(o)
    }
  }

  @objc private func reloadTapped() {
    reload()
  }

  @objc private func newQueryTapped() {
    onNewQuery?(nil)
  }

  @objc private func menuOpen() {
    if let o = clickedObject ?? selectedObject {
      onOpen?(o)
    }
  }

  @objc private func menuStructure() {
    if let o = clickedObject ?? selectedObject {
      onStructure?(o)
    }
  }

  @objc private func menuQuery() {
    guard let o = clickedObject ?? selectedObject, o.kind == .table || o.kind == .view,
      let quoted = SQL.quoteTable(o.name, driver: connection.driver)
    else { return }
    onNewQuery?("SELECT * FROM \(quoted)")
  }

  // MARK: Table

  func numberOfRows(in tableView: NSTableView) -> Int { filtered.count }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    switch filtered[row] {
    case .header(let kind):
      let cell = IconTextCellView.make(
        in: tableView, symbol: collapsed.contains(kind) ? "chevron.right" : "chevron.down", text: Self.title(of: kind))
      cell.label.font = .boldSystemFont(ofSize: NSFont.smallSystemFontSize)
      cell.label.textColor = .secondaryLabelColor
      return cell
    case .object(let o):
      let symbol: String =
        switch o.kind {
        case .table: "tablecells"
        case .view: "rectangle.on.rectangle"
        case .routine: "function"
        case .trigger: "bolt"
        }
      let cell = IconTextCellView.make(in: tableView, symbol: symbol, text: o.name)
      cell.label.font = .systemFont(ofSize: NSFont.systemFontSize)
      cell.label.textColor = .labelColor
      return cell
    }
  }

  func controlTextDidChange(_ obj: Notification) {
    applyFilter()
  }

  func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
    // Return in the search field opens the first match; Down moves to the list
    if commandSelector == #selector(NSResponder.insertNewline(_:)) {
      if case .object(let first)? = filtered.first {
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
    guard let o = clickedObject else { return }
    menu.addItem(withTitle: "Open", action: #selector(menuOpen), keyEquivalent: "").target = self
    menu.addItem(
      withTitle: o.kind == .table ? "Structure" : "Definition", action: #selector(menuStructure), keyEquivalent: ""
    ).target = self
    if o.kind == .table || o.kind == .view {
      menu.addItem(withTitle: "Query This Table", action: #selector(menuQuery), keyEquivalent: "").target = self
    }
  }
}

/// A table view whose Return key opens the selection, Delete acts on it,
/// and whose context menu comes from the owner (the clicked row selected first)
final class KeyTableView: NSTableView {
  var onReturn: (() -> Void)?
  var onDelete: (() -> Void)?
  var menuProvider: ((Int) -> NSMenu?)?

  override func keyDown(with event: NSEvent) {
    if event.keyCode == 36 || event.keyCode == 76 {
      onReturn?()
      return
    }
    if event.keyCode == 51 || event.keyCode == 117, let onDelete {
      onDelete()
      return
    }
    super.keyDown(with: event)
  }

  override func menu(for event: NSEvent) -> NSMenu? {
    guard let menuProvider else { return super.menu(for: event) }
    let point = convert(event.locationInWindow, from: nil)
    return menuProvider(row(at: point))
  }
}
