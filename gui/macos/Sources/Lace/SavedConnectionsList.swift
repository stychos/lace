//
// Lace macOS frontend
// The saved-connections list (the connection form's left panel): folders
// as group rows, connections with their name, what they reach and the
// windows they are open in, then Recent; a filter, a context menu. The
// owner fills its form from the selected row and connects on activation.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
final class SavedListView: NSView, NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate {
  enum Row {
    case folder(SavedConnectionStore.Folder)
    case connection(SavedConnectionStore.Entry)
    /// The "Recent" header
    case recentHeader
    case recent(RecentConnections.Entry)

    /// What names the row across rebuilds
    var key: Key? {
      switch self {
      case .connection(let e): .saved(e.connection.id)
      case .recent(let r): .recent(r.url)
      default: nil
      }
    }
  }

  /// A selectable row: a saved connection by id, a recent one by URL
  enum Key: Equatable {
    case saved(String)
    case recent(String)
  }

  /// The selection moved (keys, a click), not during a rebuild
  var onSelect: ((Row?) -> Void)?
  /// Double-click or Return on a connection or a recent row
  var onActivate: ((Row) -> Void)?
  var onDuplicate: ((SavedConnection) -> Void)?
  var onDelete: ((SavedConnection) -> Void)?
  var onNewConnection: ((_ folder: [Int]) -> Void)?
  var onNewFolder: ((_ parent: [Int]) -> Void)?
  var onRenameFolder: ((SavedConnectionStore.Folder) -> Void)?
  var onDeleteFolder: ((SavedConnectionStore.Folder) -> Void)?
  /// "1, 3": the windows a saved connection is open in ("" for none)
  var openIn: ((String) -> String)?
  /// Recent connections shown after the saved ones
  var showsRecents = true
  /// Case-insensitive, over names and what they reach
  var filter = "" {
    didSet { reload() }
  }

  let tableView = KeyTableView()
  let scrollView = NSScrollView()
  private(set) var rows: [Row] = []
  private let emptyLabel = NSTextField(wrappingLabelWithString: "")
  private let store = LaceApp.shared.saved
  /// Selection changes made here (rebuilds, select(key:)) are not the user's
  private var quiet = false

  override init(frame: NSRect) {
    super.init(frame: frame)
    let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("c"))
    col.resizingMask = .autoresizingMask
    tableView.addTableColumn(col)
    tableView.headerView = nil
    tableView.rowHeight = 38
    tableView.style = .inset
    tableView.selectionHighlightStyle = .regular
    tableView.dataSource = self
    tableView.delegate = self
    tableView.doubleAction = #selector(activate)
    tableView.target = self
    tableView.onReturn = { [weak self] in self?.activate() }
    tableView.menu = NSMenu()
    tableView.menu?.delegate = self
    tableView.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
    scrollView.documentView = tableView
    scrollView.hasVerticalScroller = true
    scrollView.drawsBackground = false
    scrollView.translatesAutoresizingMaskIntoConstraints = false
    addSubview(scrollView)
    emptyLabel.font = .systemFont(ofSize: 13)
    emptyLabel.textColor = .tertiaryLabelColor
    emptyLabel.alignment = .center
    emptyLabel.translatesAutoresizingMaskIntoConstraints = false
    addSubview(emptyLabel)
    NSLayoutConstraint.activate([
      scrollView.leadingAnchor.constraint(equalTo: leadingAnchor),
      scrollView.trailingAnchor.constraint(equalTo: trailingAnchor),
      scrollView.topAnchor.constraint(equalTo: topAnchor),
      scrollView.bottomAnchor.constraint(equalTo: bottomAnchor),
      emptyLabel.centerXAnchor.constraint(equalTo: centerXAnchor),
      emptyLabel.centerYAnchor.constraint(equalTo: centerYAnchor),
      emptyLabel.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -40),
    ])
    reload()
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  /// Rebuild from the store and recent.json (after any change); the
  /// selection stays on the row it named
  func reload() {
    let selected = selectedKey
    let needle = filter.trimmingCharacters(in: .whitespaces).lowercased()
    func matches(_ e: SavedConnectionStore.Entry) -> Bool {
      needle.isEmpty || e.connection.name.lowercased().contains(needle)
        || e.connection.summary.lowercased().contains(needle)
    }
    var rows: [Row] = []
    for folder in store.folders {
      let entries = store.entries(inFolder: folder.path).filter(matches)
      if folder.isRoot {
        rows += entries.map(Row.connection)
      } else if !entries.isEmpty || (needle.isEmpty && store.entries(inFolder: folder.path).isEmpty) {
        rows.append(.folder(folder))
        rows += entries.map(Row.connection)
      }
    }
    if showsRecents {
      let recent = LaceApp.shared.recents.entries.filter {
        needle.isEmpty || $0.url.lowercased().contains(needle)
      }
      if !recent.isEmpty {
        rows.append(.recentHeader)
        rows += recent.map(Row.recent)
      }
    }
    self.rows = rows
    quiet = true
    tableView.reloadData()
    select(key: selected)
    quiet = false
    if store.entries.isEmpty && rows.isEmpty {
      emptyLabel.stringValue =
        store.loadError.map { "connections.json could not be read: \($0)" }
        ?? "No saved connections yet. Fill in the form and press Save."
    } else if rows.isEmpty {
      emptyLabel.stringValue = "No saved connection matches"
    } else {
      emptyLabel.stringValue = ""
    }
    emptyLabel.isHidden = emptyLabel.stringValue.isEmpty
  }

  var selectedRow: Row? {
    let i = tableView.selectedRow
    return i >= 0 && i < rows.count ? rows[i] : nil
  }

  var selectedKey: Key? { selectedRow?.key }

  var selectedConnection: SavedConnection? {
    guard case .connection(let e) = selectedRow else { return nil }
    return e.connection
  }

  var selectedRecent: RecentConnections.Entry? {
    guard case .recent(let e) = selectedRow else { return nil }
    return e
  }

  /// The row a key names now, if the list holds it
  func row(for key: Key?) -> Row? {
    guard let key else { return nil }
    return rows.first { $0.key == key }
  }

  /// Select the row a key names (nil, or one not in the list: none),
  /// without telling onSelect
  func select(key: Key?) {
    let was = quiet
    quiet = true
    defer { quiet = was }
    guard let key, let i = rows.firstIndex(where: { $0.key == key }) else {
      tableView.deselectAll(nil)
      return
    }
    tableView.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false)
    tableView.scrollRowToVisible(i)
  }

  func select(id: String) {
    select(key: .saved(id))
  }

  func focus() {
    window?.makeFirstResponder(tableView)
  }

  @objc private func activate() {
    if let row = selectedRow, row.key != nil {
      onActivate?(row)
    }
  }

  // MARK: Table

  func numberOfRows(in tableView: NSTableView) -> Int { rows.count }

  func tableView(_ tableView: NSTableView, isGroupRow row: Int) -> Bool {
    switch rows[row] {
    case .folder, .recentHeader: true
    default: false
    }
  }

  func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat {
    switch rows[row] {
    case .folder, .recentHeader: 24
    default: 38
    }
  }

  func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
    switch rows[row] {
    case .connection, .recent: true
    default: false
    }
  }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    switch rows[row] {
    case .folder(let f):
      let cell = TextCellView.make(in: tableView, text: f.name)
      cell.textField?.font = .systemFont(ofSize: 11, weight: .semibold)
      cell.textField?.textColor = .secondaryLabelColor
      return cell
    case .connection(let e):
      let cell = TwoLineCellView.make(in: tableView)
      let c = e.connection
      var subtitle = c.name.isEmpty ? c.driverKind.displayName : "\(c.driverKind.displayName) · \(c.summary)"
      if let open = openIn?(c.id), !open.isEmpty {
        subtitle = "● open in \(open) · " + subtitle
      }
      cell.configure(
        symbol: c.isSQLite ? "doc.fill" : (c.ssh ? "lock.shield" : "cylinder.fill"), title: c.displayName,
        subtitle: subtitle, indent: CGFloat(e.depth) * 14)
      return cell
    case .recentHeader:
      let cell = TextCellView.make(in: tableView, text: "Recent")
      cell.textField?.font = .systemFont(ofSize: 11, weight: .semibold)
      cell.textField?.textColor = .secondaryLabelColor
      return cell
    case .recent(let r):
      let cell = TwoLineCellView.make(in: tableView)
      let c = SavedConnection(connectionString: r.url)
      cell.configure(
        symbol: "clock", title: c?.summary ?? r.url,
        subtitle: "\(c?.driverKind.displayName ?? "") · \(SavedListView.when(r.used))", indent: 0)
      return cell
    }
  }

  private static let whenFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateStyle = .medium
    f.timeStyle = .short
    f.doesRelativeDateFormatting = true
    return f
  }()

  static func when(_ date: Date) -> String {
    whenFormatter.string(from: date)
  }

  func tableViewSelectionDidChange(_ notification: Notification) {
    if !quiet {
      onSelect?(selectedRow)
    }
  }

  // MARK: Context menu

  func menuNeedsUpdate(_ menu: NSMenu) {
    menu.removeAllItems()
    let row = tableView.clickedRow
    guard row >= 0, row < rows.count else {
      add(menu, "New Connection", #selector(menuNew))
      add(menu, "New Folder…", #selector(menuNewFolder))
      return
    }
    switch rows[row] {
    case .connection:
      tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
      add(menu, "Open Here", #selector(activate))
      add(menu, "Duplicate", #selector(menuDuplicate))
      menu.addItem(.separator())
      add(menu, "Delete…", #selector(menuDelete))
    case .folder:
      add(menu, "New Connection Here", #selector(menuNewInFolder))
      add(menu, "New Folder Here…", #selector(menuNewFolderInFolder))
      add(menu, "Rename Folder…", #selector(menuRenameFolder))
      add(menu, "Delete Folder", #selector(menuDeleteFolder))
    case .recent:
      tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
      add(menu, "Open Here", #selector(activate))
      menu.addItem(.separator())
      add(menu, "Forget", #selector(menuForgetRecent))
    case .recentHeader:
      add(menu, "Forget All Recent", #selector(menuForgetAllRecent))
    }
  }

  @objc private func menuForgetRecent() {
    if let r = selectedRecent {
      LaceApp.shared.recents.remove(url: r.url)
    }
  }
  @objc private func menuForgetAllRecent() {
    for r in LaceApp.shared.recents.entries {
      LaceApp.shared.recents.remove(url: r.url)
    }
  }

  private func add(_ menu: NSMenu, _ title: String, _ action: Selector) {
    let item = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
    item.target = self
  }

  private var clickedFolder: SavedConnectionStore.Folder? {
    let row = tableView.clickedRow
    guard row >= 0, row < rows.count, case .folder(let f) = rows[row] else { return nil }
    return f
  }

  @objc private func menuNew() { onNewConnection?([]) }
  @objc private func menuNewFolder() { onNewFolder?([]) }
  @objc private func menuNewInFolder() {
    if let f = clickedFolder { onNewConnection?(f.path) }
  }
  @objc private func menuNewFolderInFolder() {
    if let f = clickedFolder { onNewFolder?(f.path) }
  }
  @objc private func menuDuplicate() {
    if let c = selectedConnection { onDuplicate?(c) }
  }
  @objc private func menuDelete() {
    if let c = selectedConnection { onDelete?(c) }
  }
  @objc private func menuRenameFolder() {
    if let f = clickedFolder { onRenameFolder?(f) }
  }
  @objc private func menuDeleteFolder() {
    if let f = clickedFolder { onDeleteFolder?(f) }
  }
}

/// A cell with a symbol, a title and a smaller subtitle
final class TwoLineCellView: NSTableCellView {
  private let icon = NSImageView()
  private let title = NSTextField(labelWithString: "")
  private let subtitle = NSTextField(labelWithString: "")
  private var leading: NSLayoutConstraint!

  override init(frame: NSRect) {
    super.init(frame: frame)
    icon.translatesAutoresizingMaskIntoConstraints = false
    icon.contentTintColor = .secondaryLabelColor
    title.font = .systemFont(ofSize: 13)
    title.lineBreakMode = .byTruncatingTail
    subtitle.font = .systemFont(ofSize: 11)
    subtitle.textColor = .secondaryLabelColor
    subtitle.lineBreakMode = .byTruncatingMiddle
    let text = NSStackView(views: [title, subtitle])
    text.orientation = .vertical
    text.alignment = .leading
    text.spacing = 1
    text.translatesAutoresizingMaskIntoConstraints = false
    addSubview(icon)
    addSubview(text)
    leading = icon.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4)
    NSLayoutConstraint.activate([
      leading,
      icon.centerYAnchor.constraint(equalTo: centerYAnchor),
      icon.widthAnchor.constraint(equalToConstant: 18),
      text.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 8),
      text.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
      text.centerYAnchor.constraint(equalTo: centerYAnchor),
    ])
    textField = title
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  func configure(symbol: String, title: String, subtitle: String, indent: CGFloat) {
    icon.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
    self.title.stringValue = title
    self.subtitle.stringValue = subtitle
    leading.constant = 4 + indent
  }

  static func make(in tableView: NSTableView) -> TwoLineCellView {
    let id = NSUserInterfaceItemIdentifier("TwoLineCellView")
    if let v = tableView.makeView(withIdentifier: id, owner: nil) as? TwoLineCellView {
      return v
    }
    let v = TwoLineCellView(frame: .zero)
    v.identifier = id
    return v
  }
}

/// What every list owner does with a saved connection
@MainActor
enum SavedActions {
  static func delete(_ c: SavedConnection, window: NSWindow?) async -> Bool {
    let ok = await Alerts.confirm(
      "Delete \(c.displayName)?", "The saved connection and its query history file are removed.",
      action: "Delete", window: window)
    guard ok else { return false }
    do {
      try LaceApp.shared.removeSaved(id: c.id)
      if let path = History(connectionID: c.id, mode: .persistent).filePath {
        try? FileManager.default.removeItem(atPath: path)
      }
      return true
    } catch {
      Alerts.error("Not deleted", "\(error)", window: window)
      return false
    }
  }

  static func duplicate(_ c: SavedConnection, window: NSWindow?) -> SavedConnection? {
    var copy = LaceApp.shared.savedConnection(id: c.id) ?? c  // its saved passwords too
    copy.id = SavedConnection.newID()
    copy.name = c.name.isEmpty ? "" : "\(c.name) copy"
    do {
      let folder = LaceApp.shared.saved.entry(id: c.id)?.folderPath ?? []
      try LaceApp.shared.store(copy, inFolder: folder)
      return copy
    } catch {
      Alerts.error("Not duplicated", "\(error)", window: window)
      return nil
    }
  }

  static func renameFolder(_ f: SavedConnectionStore.Folder, window: NSWindow?) async {
    guard let name = await Alerts.prompt("Rename Folder", "", initial: f.name, action: "Rename", window: window),
      !name.trimmingCharacters(in: .whitespaces).isEmpty
    else { return }
    do {
      try LaceApp.shared.saved.renameFolder(at: f.path, to: name.trimmingCharacters(in: .whitespaces))
    } catch {
      Alerts.error("Not renamed", "\(error)", window: window)
    }
  }

  static func deleteFolder(_ f: SavedConnectionStore.Folder, window: NSWindow?) {
    do {
      try LaceApp.shared.saved.removeFolder(at: f.path)
    } catch {
      Alerts.error("Not deleted", "\(error)", window: window)
    }
  }

  static func newFolder(in parent: [Int], window: NSWindow?) async {
    guard let name = await Alerts.prompt("New Folder", "A folder groups saved connections", action: "Create", window: window),
      !name.trimmingCharacters(in: .whitespaces).isEmpty
    else { return }
    do {
      try LaceApp.shared.saved.addFolder(named: name.trimmingCharacters(in: .whitespaces), inFolder: parent)
    } catch {
      Alerts.error("No folder", "\(error)", window: window)
    }
  }
}
