//
// Lace macOS frontend
// ConnectionViewController: one database connection as a tab of a
// workspace: the tables sidebar and the inner tabs of table and query views
//
// A single click in the sidebar opens a table in the one preview tab (eye
// icon, replaced by the next single click); a double click, Return, or
// working in the view (filter, sort, edit: `onTouched`) keeps it. The
// connection's events (transaction state, notices, close) are consumed
// here and reflected in the tab badge and a notice bubble.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class ConnectionViewController: NSSplitViewController {
  let id = UUID()
  let connection: Connection
  let prefs: Prefs
  /// The saved connection this came from, if any (its history, its name)
  let saved: SavedConnection?
  let history: History
  let sidebar: SidebarViewController
  let content: ContentViewController
  private(set) var transactionState: Connection.TxState = .none
  /// The transaction state changed: the workspace redraws the tab
  var onStateChanged: (() -> Void)?
  private var eventsTask: Task<Void, Never>?

  init(connection: Connection, prefs: Prefs, saved: SavedConnection? = nil) {
    self.connection = connection
    self.prefs = prefs
    self.saved = saved
    history = History(connectionID: saved?.id, mode: prefs.historyMode, maxSize: prefs.historyMaxSize)
    sidebar = SidebarViewController(connection: connection)
    content = ContentViewController(connection: connection, prefs: prefs)
    super.init(nibName: nil, bundle: nil)
    content.record = { [weak self] sql in self?.history.record(sql) }
  }

  /// The saved connection's name, else what the connection string says
  var tabTitle: String {
    if let name = saved?.name, !name.isEmpty { return name }
    return connection.shortName
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func viewDidLoad() {
    super.viewDidLoad()
    splitView.autosaveName = "ConnectionSplit"
    let side = NSSplitViewItem(sidebarWithViewController: sidebar)
    side.minimumThickness = 180
    side.maximumThickness = 420
    side.canCollapse = true
    let main = NSSplitViewItem(viewController: content)
    main.minimumThickness = 400
    addSplitViewItem(side)
    addSplitViewItem(main)

    sidebar.onPreview = { [weak self] object in self?.content.openObject(object, preview: true) }
    sidebar.onOpen = { [weak self] object in self?.content.openObject(object, preview: false) }
    sidebar.onStructure = { [weak self] object in
      self?.content.openObject(object, preview: false).setSubView(.structure)
    }
    sidebar.onNewQuery = { [weak self] text in self?.content.newQuery(text: text) }
    content.onTablesChanged = { [weak self] in self?.sidebar.reload() }
    content.objectsProvider = { [weak self] in self?.sidebar.objects ?? [] }
    content.tablesProvider = { [weak self] in self?.sidebar.tables ?? [] }

    let events = connection.events
    eventsTask = Task { [weak self] in
      for await event in events {
        guard let self else { break }
        handle(event)
      }
    }
  }

  private func handle(_ event: Connection.Event) {
    switch event {
    case .transactionChanged(let state):
      transactionState = state
      onStateChanged?()
    case .notice(let message):
      content.showNotice(message)
    case .userBusyChanged:
      content.setLocked(connection.userStatement)  // the lock banner on every tab
    case .closed:
      break
    }
  }

  /// "TX", "TX FAILED", "TX LOST" for the connection tab
  var transactionBadge: String? {
    switch transactionState {
    case .none: nil
    case .open: "TX"
    case .failed: "TX FAILED"
    case .lost: "TX LOST"
    }
  }

  var hasOpenTransaction: Bool { transactionState == .open || transactionState == .failed }

  var isSidebarVisible: Bool {
    get { !(splitViewItems.first?.isCollapsed ?? false) }
    set { splitViewItems.first?.isCollapsed = !newValue }
  }

  func newQuery(text: String?) {
    content.newQuery(text: text)
  }

  /// Close the current inner tab; false when there is none
  func closeCurrentTab() -> Bool {
    content.closeCurrentTab()
  }

  func focusContent() {
    if !content.focusCurrent() {
      sidebar.focus()
    }
  }

  /// Stop everything and disconnect (the daemon rolls an open transaction back)
  func tearDown() {
    eventsTask?.cancel()
    content.tearDown()
    let connection = self.connection
    Task { try? await connection.close() }
  }

  @objc func showHistory(_ sender: Any?) {
    let vc = HistoryViewController(history: history, connectionName: tabTitle)
    vc.onUse = { [weak self] sql in self?.content.useSQL(sql) }
    presentAsSheet(vc)
  }

  @objc func focusSidebar(_ sender: Any?) {
    if let item = splitViewItems.first, item.isCollapsed {
      item.animator().isCollapsed = false
    }
    sidebar.focus()
  }
}

/// The inner tabs of a connection: table views and query views
final class ContentViewController: NSViewController {
  private struct Tab {
    let id: UUID
    var controller: NSViewController
    var isPreview: Bool
    /// A table tab's object; nil for a query
    var object: CatalogObject?
    var table: String? { object?.name }
  }

  let connection: Connection
  let prefs: Prefs
  /// A query wrote something (DDL, DML): the sidebar reloads its tables
  var onTablesChanged: (() -> Void)?
  /// A statement to record in the connection's history
  var record: ((String) -> Void)?
  /// The connection's objects (the sidebar's list), for the Information view
  var objectsProvider: (() -> [CatalogObject])?
  /// The connection's tables (the foreign key editor)
  var tablesProvider: (() -> [String])?
  private let strip = TabStripView(frame: .zero)
  private let container = NSView()
  private let emptyTitle = NSTextField(labelWithString: "Pick a table")
  private let emptyBody = NSTextField(
    wrappingLabelWithString: "Choose a table in the sidebar, or press ⌘E for a SQL editor.")
  private let notice = NoticeView(frame: .zero)
  private var tabs: [Tab] = []
  private var currentID: UUID?
  private var queryCounter = 0

  init(connection: Connection, prefs: Prefs) {
    self.connection = connection
    self.prefs = prefs
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 600))
    strip.translatesAutoresizingMaskIntoConstraints = false
    strip.onSelect = { [weak self] id in self?.select(id) }
    strip.onClose = { [weak self] id in self?.closeTab(id: id) }
    strip.onCloseOthers = { [weak self] id in self?.closeOtherTabs(keeping: id) }
    strip.onCloseAll = { [weak self] in self?.closeAllTabs() }
    strip.menuItemsProvider = { [weak self] id in
      // the tab's object actions, after the close items
      guard let self, let tab = tabs.first(where: { $0.id == id }), let vc = tab.controller as? TableViewController
      else { return [] }
      return [NSMenuItem.separator()] + vc.tableActionsMenu().items.map { $0.copy() as! NSMenuItem }
    }
    container.translatesAutoresizingMaskIntoConstraints = false
    emptyTitle.font = .systemFont(ofSize: 18, weight: .medium)
    emptyTitle.textColor = .secondaryLabelColor
    emptyTitle.alignment = .center
    emptyBody.font = .systemFont(ofSize: 13)
    emptyBody.textColor = .tertiaryLabelColor
    emptyBody.alignment = .center
    emptyBody.preferredMaxLayoutWidth = 360
    let empty = NSStackView(views: [emptyTitle, emptyBody])
    empty.orientation = .vertical
    empty.alignment = .centerX
    empty.spacing = 8
    empty.translatesAutoresizingMaskIntoConstraints = false
    notice.translatesAutoresizingMaskIntoConstraints = false
    root.addSubview(strip)
    root.addSubview(container)
    container.addSubview(empty)
    root.addSubview(notice)
    NSLayoutConstraint.activate([
      strip.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      strip.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      strip.topAnchor.constraint(equalTo: root.topAnchor),
      container.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      container.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      container.topAnchor.constraint(equalTo: strip.bottomAnchor),
      container.bottomAnchor.constraint(equalTo: root.bottomAnchor),
      empty.centerXAnchor.constraint(equalTo: container.centerXAnchor),
      empty.centerYAnchor.constraint(equalTo: container.centerYAnchor, constant: -20),
      notice.centerXAnchor.constraint(equalTo: root.centerXAnchor),
      notice.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -40),
      notice.widthAnchor.constraint(lessThanOrEqualTo: root.widthAnchor, constant: -40),
    ])
    view = root
  }

  private var current: Tab? { tabs.first { $0.id == currentID } }

  /// The view in the selected tab (a TableViewController or a QueryViewController)
  var currentController: NSViewController? { current?.controller }
  /// Every tab's view, in order (the session file)
  var tabControllers: [NSViewController] { tabs.map(\.controller) }
  var currentIndex: Int? { tabs.firstIndex { $0.id == currentID } }

  func select(index: Int) {
    guard index >= 0, index < tabs.count else { return }
    select(tabs[index].id)
  }

  func showNotice(_ text: String) {
    notice.show(text)
  }

  /// Put SQL from the history into the current query tab, or a new one
  func useSQL(_ sql: String) {
    if let query = current?.controller as? QueryViewController {
      query.insertText(sql)
    } else {
      newQuery(text: sql)
    }
  }

  // MARK: Tabs

  @discardableResult
  func openTable(_ name: String, preview: Bool) -> TableViewController {
    openObject(CatalogObject(kind: .table, name: name), preview: preview)
  }

  /// Tables and database views share a namespace; routines and triggers have their own
  private func sameObject(_ a: CatalogObject, _ b: CatalogObject) -> Bool {
    let relation = { (k: ObjectKind) in k == .table || k == .view }
    return a.name == b.name && (relation(a.kind) ? relation(b.kind) : a.kind == b.kind)
  }

  /// Open a table filtered by a followed key: an open tab of it gets the
  /// filters, else a new one; the tab is kept and shown
  func openFiltered(_ table: String, filters: [Filter]) {
    let vc = openObject(CatalogObject(kind: .table, name: table), preview: false)
    vc.showFiltered(filters)
    showNotice("\(table) where " + filters.map { "\($0.column ?? "") = \($0.value)" }.joined(separator: ", "))
  }

  @discardableResult
  func openObject(_ object: CatalogObject, preview: Bool) -> TableViewController {
    let name = object.name
    if let i = tabs.firstIndex(where: { $0.object.map { sameObject($0, object) } ?? false }),
      let existing = tabs[i].controller as? TableViewController
    {
      if !preview && tabs[i].isPreview {
        tabs[i].isPreview = false
      }
      select(tabs[i].id)
      refreshStrip()
      if !preview {
        existing.focusGrid()
      }
      return existing
    }
    let vc = TableViewController(connection: connection, object: object, prefs: prefs)
    vc.toast = { [weak self] in self?.showNotice($0) }
    vc.record = { [weak self] in self?.record?($0) }
    vc.onOpenFiltered = { [weak self] table, filters in self?.openFiltered(table, filters: filters) }
    vc.onOpenObject = { [weak self] object, sv in self?.openObject(object, preview: false).setSubView(sv) }
    vc.objectsProvider = { [weak self] in self?.objectsProvider?() ?? [] }
    vc.tablesProvider = { [weak self] in self?.tablesProvider?() ?? [] }
    vc.onNewQuery = { [weak self] text in self?.newQuery(text: text) }
    vc.onDdlApplied = { [weak self, weak vc] after in
      guard let self, let vc else { return }
      ddlApplied(to: vc.object, after: after)
    }
    vc.setLocked(connection.userStatement)
    vc.onTouched = { [weak self, weak vc] in
      guard let self, let vc, let i = tabs.firstIndex(where: { $0.controller === vc }), tabs[i].isPreview
      else { return }
      tabs[i].isPreview = false
      refreshStrip()
    }
    if preview, let i = tabs.firstIndex(where: \.isPreview) {
      // The preview tab stays where it is; only what it shows changes
      let old = tabs[i]
      detach(old.controller)
      tabs[i] = Tab(id: old.id, controller: vc, isPreview: true, object: object)
      addChild(vc)
      select(old.id)
    } else {
      let tab = Tab(id: UUID(), controller: vc, isPreview: preview, object: object)
      tabs.append(tab)
      addChild(vc)
      select(tab.id)
    }
    refreshStrip()
    if !preview {
      vc.focusGrid()
    }
    return vc
  }

  @discardableResult
  func newQuery(text: String?) -> QueryViewController {
    queryCounter += 1
    let vc = QueryViewController(connection: connection, prefs: prefs)
    vc.number = queryCounter
    vc.toast = { [weak self] in self?.showNotice($0) }
    vc.record = { [weak self] in self?.record?($0) }
    vc.onTitleChanged = { [weak self] in self?.refreshStrip() }
    vc.onTablesChanged = { [weak self] in self?.onTablesChanged?() }
    vc.onOpenFiltered = { [weak self] table, filters in self?.openFiltered(table, filters: filters) }
    let tab = Tab(id: UUID(), controller: vc, isPreview: false, object: nil)
    tabs.append(tab)
    addChild(vc)
    select(tab.id)
    vc.setLocked(connection.userStatement)
    if let text, !text.isEmpty {
      vc.text = text
    }
    refreshStrip()
    vc.focusEditor()
    return vc
  }

  /// A structure change or table action ran on object: every tab of it
  /// reloads, follows a rename or closes; the sidebar reloads after a
  /// rename, create or drop; a new table opens on Structure
  func ddlApplied(to object: CatalogObject, after: DdlAfter) {
    var sidebar = false
    for tab in tabs where tab.object.map({ sameObject($0, object) }) ?? false {
      guard let vc = tab.controller as? TableViewController else { continue }
      switch after {
      case .drop:
        closeTab(id: tab.id)
        sidebar = true
      case .rename(let name):
        vc.rename(to: name)
        if let i = tabs.firstIndex(where: { $0.id == tab.id }) {
          tabs[i].object = vc.object
        }
        vc.reloadAfterChange()
        sidebar = true
      case .tables, .createTable:
        vc.reloadAfterChange()
        sidebar = true
      case .schema:
        vc.reloadAfterChange()
      }
    }
    if case .createTable(let name) = after {
      sidebar = true
      openObject(CatalogObject(kind: .table, name: name), preview: false).setSubView(.structure)
    }
    refreshStrip()
    if sidebar {
      onTablesChanged?()
    }
  }

  /// The lock banner on every tab (the connection's user statement, or nil)
  func setLocked(_ statement: Connection.UserStatement?) {
    for tab in tabs {
      (tab.controller as? TableViewController)?.setLocked(statement)
      (tab.controller as? QueryViewController)?.setLocked(statement)
    }
  }

  private func select(_ id: UUID?) {
    currentID = id
    for sub in container.subviews where sub is NSStackView == false {
      sub.removeFromSuperview()
    }
    if let tab = current {
      let v = tab.controller.view
      v.translatesAutoresizingMaskIntoConstraints = false
      container.addSubview(v)
      NSLayoutConstraint.activate([
        v.leadingAnchor.constraint(equalTo: container.leadingAnchor),
        v.trailingAnchor.constraint(equalTo: container.trailingAnchor),
        v.topAnchor.constraint(equalTo: container.topAnchor),
        v.bottomAnchor.constraint(equalTo: container.bottomAnchor),
      ])
    }
    strip.selectedID = id
    strip.isHidden = tabs.isEmpty
    for sub in container.subviews where sub is NSStackView {
      sub.isHidden = !tabs.isEmpty
    }
  }

  private func detach(_ vc: NSViewController) {
    (vc as? TableViewController)?.tearDown()
    (vc as? QueryViewController)?.tearDown()
    vc.view.removeFromSuperview()
    vc.removeFromParent()
  }

  private func refreshStrip() {
    strip.items = tabs.map { tab in
      let query = tab.controller as? QueryViewController
      let title = query?.tabTitle ?? tab.table ?? "Query"
      let kindSymbol: String =
        switch tab.object?.kind {
        case .view: "rectangle.on.rectangle"
        case .routine: "function"
        case .trigger: "bolt"
        default: "tablecells"
        }
      let symbol = tab.isPreview ? "eye" : (query != nil ? "terminal" : kindSymbol)
      let tip = tab.isPreview ? "\(title) (preview: double-click the table or edit to keep it)" : title
      return TabStripView.Item(id: tab.id, title: title, symbol: symbol, tooltip: tip, dimmed: tab.isPreview)
    }
    strip.isHidden = tabs.isEmpty
  }

  func closeTab(id: UUID) {
    guard let i = tabs.firstIndex(where: { $0.id == id }) else { return }
    let tab = tabs.remove(at: i)
    detach(tab.controller)
    if currentID == id {
      select(tabs.isEmpty ? nil : tabs[min(i, tabs.count - 1)].id)
    }
    refreshStrip()
    _ = focusCurrent()
  }

  func closeCurrentTab() -> Bool {
    guard let currentID else { return false }
    closeTab(id: currentID)
    return true
  }

  private func closeOtherTabs(keeping id: UUID) {
    for tab in tabs where tab.id != id {
      closeTab(id: tab.id)
    }
  }

  private func closeAllTabs() {
    for tab in tabs {
      closeTab(id: tab.id)
    }
  }

  @discardableResult
  func focusCurrent() -> Bool {
    guard let tab = current else { return false }
    if let table = tab.controller as? TableViewController {
      table.focusGrid()
    } else if let query = tab.controller as? QueryViewController {
      query.focusEditor()
    }
    return true
  }

  func tearDown() {
    for tab in tabs {
      detach(tab.controller)
    }
    tabs.removeAll()
  }

  // MARK: Actions (responder chain)

  @objc func selectNextTab(_ sender: Any?) {
    cycle(by: 1)
  }

  @objc func selectPreviousTab(_ sender: Any?) {
    cycle(by: -1)
  }

  private func cycle(by delta: Int) {
    guard tabs.count > 1, let i = tabs.firstIndex(where: { $0.id == currentID }) else { return }
    select(tabs[(i + delta + tabs.count) % tabs.count].id)
    focusCurrent()
  }
}
