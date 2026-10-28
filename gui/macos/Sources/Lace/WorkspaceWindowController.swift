//
// Lace macOS frontend
// A workspace: one window whose strip entries are database connections
// and connection forms (the connection tab design). A new window starts
// with a form; ⌘T puts one after the current entry. A connection is shared
// by every entry showing it, in any window (ConnectionRegistry): closing an
// entry lets it go, and only its last holder asks before an open
// transaction is rolled back.
//
// Closing: the user closing a connection's last table or query tab (or ⌘W
// on a connection with none) turns the entry into a form in its place with
// general.keep_connection_tab, else closes it; the strip's × and ⇧⌘W close
// an entry; a window left with no entry closes; the last window's close
// follows general.on_last_workspace_close (quit, or a new window with a form).
//
// Dragging: an entry's tab reorders within the strip, or drops on another
// window's strip, which takes the entry over with its connection hold (its
// callbacks point at the new window); a window left with none closes.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class WorkspaceWindowController: NSWindowController, NSWindowDelegate {
  let workspace = WorkspaceViewController()
  private(set) var name: String
  /// 1, 2, ...: the order windows were made in (never reused)
  let number: Int
  private var forceClose = false

  init(name: String, number: Int) {
    self.name = name
    self.number = number
    let window = NSWindow(
      contentRect: NSRect(x: 0, y: 0, width: 1160, height: 740),
      styleMask: [.titled, .closable, .miniaturizable, .resizable],
      backing: .buffered, defer: false)
    window.title = name
    window.tabbingIdentifier = "dev.hexed.lace.workspace"
    window.minSize = NSSize(width: 700, height: 420)
    window.contentViewController = workspace
    if !window.setFrameUsingName("Workspace") {
      window.center()
    }
    window.setFrameAutosaveName("Workspace")
    super.init(window: window)
    window.delegate = self
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  func rename(_ newName: String) {
    name = newName
    window?.title = newName
  }

  func windowShouldClose(_ sender: NSWindow) -> Bool {
    if forceClose || !workspace.hasLastHeldTransaction {
      return true
    }
    Task { [weak self] in
      guard let self else { return }
      let ok = await Alerts.confirm(
        "Close \(name)?", "A transaction is open. Closing the window rolls it back.", action: "Close",
        window: window)
      if ok {
        forceClose = true
        window?.close()
      }
    }
    return false
  }

  /// A window the user closes leaves the session, except the last one: the
  /// app stays open without windows, and the next launch (or Dock click)
  /// brings that workspace back. Windows closing because the app quits
  /// change nothing: the session was written with all of them.
  func windowWillClose(_ notification: Notification) {
    workspace.isClosed = true
    let app = LaceApp.shared
    if !app.isTerminating {
      if app.workspaces.count > 1 {
        Session.save(except: self)
      } else {
        Session.save()
      }
    }
    workspace.closeAllConnections()
    app.workspaceClosed(self)
    if !app.isTerminating && app.workspaces.isEmpty {
      app.lastWorkspaceClosed()
    }
  }

  @objc func renameWorkspace(_ sender: Any?) {
    Task { [weak self] in
      guard let self else { return }
      guard let text = await Alerts.prompt("Rename Workspace", "", initial: name, action: "Rename", window: window)
      else { return }
      let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
      if !trimmed.isEmpty {
        rename(trimmed)
      }
    }
  }
}

/// A strip entry of a workspace window: a connection or a connection form
@MainActor
protocol WorkspaceEntry: NSViewController {
  var id: UUID { get }
  /// How the strip shows it
  var stripItem: TabStripView.Item { get }
  /// Put the keyboard focus into it (it was selected)
  func focusEntry()
}

final class WorkspaceViewController: NSViewController, NSUserInterfaceValidations {
  private let strip = TabStripView(frame: .zero)
  private let container = NSView()
  private var entries: [any WorkspaceEntry] = []
  private var currentID: UUID?
  /// The window closed: a connection that lands now finds no place here
  var isClosed = false

  var currentEntry: (any WorkspaceEntry)? { entries.first { $0.id == currentID } }
  /// The selected entry when it is a connection
  var current: ConnectionViewController? { currentEntry as? ConnectionViewController }
  /// The selected entry when it is a form
  var currentForm: ConnectionFormViewController? { currentEntry as? ConnectionFormViewController }
  /// The connection entries, in strip order
  var connectionControllers: [ConnectionViewController] { entries.compactMap { $0 as? ConnectionViewController } }
  /// Every entry, in strip order (the session file)
  var entryControllers: [any WorkspaceEntry] { entries }
  var currentIndex: Int? { entries.firstIndex { $0.id == currentID } }
  /// The selected entry's place among the connection entries
  var currentConnectionIndex: Int? { connectionControllers.firstIndex { $0.id == currentID } }

  /// A connection with an open transaction that only this window holds:
  /// closing the window closes it (and rolls the transaction back)
  var hasLastHeldTransaction: Bool {
    let mine = connectionControllers
    return mine.contains { vc in
      vc.hasOpenTransaction
        && mine.filter { $0.connection === vc.connection }.count >= LaceApp.shared.connections.holders(vc.connection)
    }
  }

  /// The strip's entries (the script driver)
  var entryCount: Int { entries.count }
  /// What the selected entry is: "connection", "form" or "none"
  var currentKind: String { current != nil ? "connection" : currentForm != nil ? "form" : "none" }
  /// The selected entry's strip title
  var currentTitle: String? { currentEntry?.stripItem.title }
  /// Tabs of the strip that show a transaction badge (what is drawn, so a
  /// badge that did not reach the strip counts as none)
  var badgeCount: Int { strip.items.filter { $0.badge != nil }.count }

  func select(index: Int) {
    guard index >= 0, index < entries.count else { return }
    select(entries[index].id)
  }

  /// Select the index-th connection entry
  func selectConnection(index: Int) {
    let conns = connectionControllers
    guard index >= 0, index < conns.count else { return }
    select(conns[index].id)
  }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 1160, height: 740))
    strip.translatesAutoresizingMaskIntoConstraints = false
    strip.dragGroup = "workspace"
    strip.onMove = { [weak self] id, index in self?.moveEntry(id: id, to: index) }
    strip.onAccept = { [weak self] id, source, index in self?.takeEntry(id: id, fromStrip: source, at: index) }
    strip.onSelect = { [weak self] id in self?.select(id) }
    strip.onClose = { [weak self] id in self?.closeEntry(id: id) }
    strip.onCloseOthers = { [weak self] id in
      guard let self else { return }
      for e in entries where e.id != id {
        closeEntry(id: e.id)
      }
    }
    strip.onCloseAll = { [weak self] in
      guard let self else { return }
      for e in entries {
        closeEntry(id: e.id)
      }
    }
    container.translatesAutoresizingMaskIntoConstraints = false
    root.addSubview(strip)
    root.addSubview(container)
    NSLayoutConstraint.activate([
      strip.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      strip.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      strip.topAnchor.constraint(equalTo: root.topAnchor),
      container.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      container.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      container.topAnchor.constraint(equalTo: strip.bottomAnchor),
      container.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    view = root
  }

  override func viewDidAppear() {
    super.viewDidAppear()
    currentEntry?.focusEntry()
  }

  private func show(_ vc: NSViewController?) {
    for sub in container.subviews {
      sub.removeFromSuperview()
    }
    guard let vc else { return }
    let v = vc.view
    v.translatesAutoresizingMaskIntoConstraints = false
    container.addSubview(v)
    NSLayoutConstraint.activate([
      v.leadingAnchor.constraint(equalTo: container.leadingAnchor),
      v.trailingAnchor.constraint(equalTo: container.trailingAnchor),
      v.topAnchor.constraint(equalTo: container.topAnchor),
      v.bottomAnchor.constraint(equalTo: container.bottomAnchor),
    ])
  }

  // MARK: Entries

  /// A connection form after the entry with id `after` (nil: at the end), selected
  @discardableResult
  func addForm(after: UUID? = nil) -> ConnectionFormViewController {
    let i = after.flatMap { id in entries.firstIndex { $0.id == id } }.map { $0 + 1 } ?? entries.count
    return addForm(at: i)
  }

  @discardableResult
  func addForm(at index: Int) -> ConnectionFormViewController {
    let form = ConnectionFormViewController()
    wire(form)
    insert(form, at: index, select: true)
    return form
  }

  /// Point an entry's callbacks at this workspace (a new one, or one taken
  /// over from another window: its old window's handlers must not stay)
  private func wire(_ entry: any WorkspaceEntry) {
    if let form = entry as? ConnectionFormViewController {
      form.workspace = self
      form.onLabelChanged = { [weak self] in self?.refreshStrip() }
    } else if let vc = entry as? ConnectionViewController {
      vc.onStateChanged = { [weak self] in self?.refreshStrip() }
      vc.onLastTabClosed = { [weak self, weak vc] in
        guard let self, let vc else { return }
        lastViewClosed(vc)
      }
    }
  }

  private func insert(_ entry: any WorkspaceEntry, at index: Int, select doSelect: Bool) {
    entries.insert(entry, at: max(0, min(index, entries.count)))
    addChild(entry)
    refreshStrip()
    if doSelect || currentID == nil {
      select(entry.id)
    }
  }

  /// A new entry holding a connection the registry opened
  private func makeEntry(_ connection: Connection, saved: SavedConnection?) -> ConnectionViewController {
    LaceApp.shared.connections.hold(connection)
    let vc = ConnectionViewController(connection: connection, prefs: LaceApp.shared.prefs, saved: saved)
    wire(vc)
    return vc
  }

  /// A connection entry is at its last view (its last tab closed, or ⌘W on
  /// one with none): a form in its place with keep_connection_tab, else it closes
  private func lastViewClosed(_ vc: ConnectionViewController) {
    if LaceApp.shared.prefs.keepConnectionTab {
      replaceWithForm(id: vc.id)
    } else {
      closeEntry(id: vc.id)
    }
  }

  /// The connection entry with this id becomes a form at its index; the
  /// connection is let go, after the question when this entry is the last
  /// holder of an open transaction (Cancel leaves it connected)
  func replaceWithForm(id: UUID) {
    guard let vc = entries.first(where: { $0.id == id }) as? ConnectionViewController else { return }
    guard vc.hasOpenTransaction, vc.isLastHolder else {
      swapForForm(vc)
      return
    }
    Task { [weak self] in
      guard let self else { return }
      let ok = await Alerts.confirm(
        "Close \(vc.connection.shortName)?", "A transaction is open. Disconnecting rolls it back.",
        action: "Disconnect", window: view.window)
      if ok {
        swapForForm(vc)
      }
    }
  }

  private func swapForForm(_ vc: ConnectionViewController) {
    guard let i = entries.firstIndex(where: { $0 === vc }) else { return }
    let wasCurrent = currentID == vc.id
    vc.tearDown()
    entries.remove(at: i)
    detach(vc)
    let form = ConnectionFormViewController()
    wire(form)
    entries.insert(form, at: i)
    addChild(form)
    refreshStrip()
    if wasCurrent || currentID == nil {
      select(form.id)
    }
  }

  /// The connection as an entry at the end; autoOpen: its first table opens
  /// when the preference says so
  @discardableResult
  func append(_ connection: Connection, saved: SavedConnection?, select: Bool, autoOpen: Bool) -> ConnectionViewController? {
    guard !isClosed else {
      LaceApp.shared.connections.releaseIfUnheld(connection)
      return nil
    }
    let vc = makeEntry(connection, saved: saved)
    insert(vc, at: entries.count, select: select)
    if autoOpen && LaceApp.shared.prefs.autoOpenFirstTable {
      vc.openFirstTableWhenLoaded()
    }
    return vc
  }

  /// A form's Open here (the connection takes the form's place) or
  /// Background tab (a new entry after it; the form stays selected)
  func place(_ connection: Connection, saved: SavedConnection?, from form: ConnectionFormViewController, background: Bool) {
    guard !isClosed else {
      LaceApp.shared.connections.releaseIfUnheld(connection)
      return
    }
    guard let i = entries.firstIndex(where: { $0 === form }) else {
      append(connection, saved: saved, select: !background, autoOpen: !background)
      return
    }
    let vc = makeEntry(connection, saved: saved)
    if background {
      insert(vc, at: i + 1, select: false)
      return
    }
    let wasCurrent = currentID == form.id
    entries[i] = vc
    detach(form)
    addChild(vc)
    refreshStrip()
    if wasCurrent || currentID == nil || !entries.contains(where: { $0.id == currentID }) {
      select(vc.id)
    }
    if LaceApp.shared.prefs.autoOpenFirstTable {
      vc.openFirstTableWhenLoaded()
    }
  }

  private func detach(_ vc: NSViewController) {
    vc.view.removeFromSuperview()
    vc.removeFromParent()
  }

  private func refreshStrip() {
    strip.items = entries.map(\.stripItem)
    NotificationCenter.default.post(name: LaceApp.entriesDidChange, object: self)
  }

  private func select(_ id: UUID?) {
    currentID = id
    strip.selectedID = id
    let entry = currentEntry
    show(entry)
    if let vc = entry as? ConnectionViewController {
      view.window?.subtitle = vc.connection.displayName
    } else {
      view.window?.subtitle = ""
    }
    entry?.focusEntry()
  }

  // MARK: Connecting

  /// A connection string, or the path of a SQLite file (a file opened from
  /// the Finder, the open panel): into an untouched form's place, else at the end
  func connect(to text: String) {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !trimmed.isEmpty else { return }
    let connectionString: String
    if trimmed.contains("://") {
      connectionString = trimmed
    } else {
      switch Driver.sqliteConnectionString(path: (trimmed as NSString).expandingTildeInPath) {
      case .success(let cs): connectionString = cs
      case .failure(let error):
        Alerts.error("Could not open", error.message, window: view.window)
        return
      }
    }
    Task {
      guard let conn = await LaceApp.shared.connections.open(connectionString: connectionString, saved: nil, window: view.window)
      else { return }
      if let form = currentForm, form.isPristine {
        place(conn, saved: nil, from: form, background: false)
      } else {
        append(conn, saved: nil, select: true, autoOpen: true)
      }
    }
  }

  /// The connection for this target (one already open anywhere in the app,
  /// else a new one, with password prompts) as an entry at the end; nil
  /// when it failed or was cancelled (the error was shown). Session restore
  /// and the script driver; no first table is opened.
  @discardableResult
  func open(connectionString: String, saved: SavedConnection? = nil) async -> ConnectionViewController? {
    guard !isClosed,
      let conn = await LaceApp.shared.connections.open(connectionString: connectionString, saved: saved, window: view.window)
    else { return nil }
    return append(conn, saved: saved, select: true, autoOpen: false)  // nil when the window closed meanwhile
  }

  // MARK: Closing

  /// Close an entry: a form goes; a connection's entry lets the connection
  /// go, asking first when it is the last holder of an open transaction
  func closeEntry(id: UUID) {
    guard let entry = entries.first(where: { $0.id == id }) else { return }
    guard let vc = entry as? ConnectionViewController, vc.hasOpenTransaction, vc.isLastHolder else {
      remove(id: id)
      return
    }
    Task { [weak self] in
      guard let self else { return }
      let ok = await Alerts.confirm(
        "Close \(vc.connection.shortName)?", "A transaction is open. Disconnecting rolls it back.",
        action: "Disconnect", window: view.window)
      if ok {
        remove(id: id)
      }
    }
  }

  func closeConnection(id: UUID) {
    closeEntry(id: id)
  }

  private func remove(id: UUID) {
    guard let i = entries.firstIndex(where: { $0.id == id }) else { return }
    let entry = entries.remove(at: i)
    (entry as? ConnectionViewController)?.tearDown()
    detach(entry)
    if currentID == id {
      select(entries.isEmpty ? nil : entries[min(i, entries.count - 1)].id)
    }
    refreshStrip()
    closeIfEmpty()
  }

  /// A window left with nothing closes (after the change that emptied it)
  private func closeIfEmpty() {
    guard entries.isEmpty else { return }
    DispatchQueue.main.async { [weak self] in
      guard let self, entries.isEmpty, let window = view.window, window.isVisible else { return }
      window.close()
    }
  }

  // MARK: Dragging entries

  /// The strip (the script driver drops on it as a drag does)
  var stripView: TabStripView { strip }

  /// An entry's tab dropped elsewhere in this strip: it moves to index
  func moveEntry(id: UUID, to index: Int) {
    guard let from = entries.firstIndex(where: { $0.id == id }) else { return }
    let entry = entries.remove(at: from)
    entries.insert(entry, at: max(0, min(index, entries.count)))
    refreshStrip()
  }

  /// An entry of another window dropped on this strip before index: it
  /// moves here with its connection hold, selected, and this window comes
  /// to the front; the window it left closes when that was its last entry
  func takeEntry(id: UUID, fromStrip source: TabStripView, at index: Int) {
    guard !isClosed, let from = LaceApp.shared.workspaces.first(where: { $0.workspace.strip === source })?.workspace,
      from !== self, let entry = from.giveEntry(id: id)
    else { return }
    wire(entry)
    insert(entry, at: index, select: true)
    view.window?.makeKeyAndOrderFront(nil)
  }

  /// The entry leaves this window as it is (nothing let go): another one takes it
  private func giveEntry(id: UUID) -> (any WorkspaceEntry)? {
    guard let i = entries.firstIndex(where: { $0.id == id }) else { return nil }
    let entry = entries.remove(at: i)
    detach(entry)
    if currentID == id {
      select(entries.isEmpty ? nil : entries[min(i, entries.count - 1)].id)
    }
    refreshStrip()
    closeIfEmpty()
    return entry
  }

  /// The untouched form a new window starts with goes, now that other
  /// entries were restored (never the last entry)
  func dropPristineForm(_ form: ConnectionFormViewController) {
    guard entries.count > 1, form.isPristine, let i = entries.firstIndex(where: { $0 === form }) else { return }
    entries.remove(at: i)
    detach(form)
    if currentID == form.id {
      select(entries[min(i, entries.count - 1)].id)
    }
    refreshStrip()
  }

  /// The window goes: every entry lets its connection go
  func closeAllConnections() {
    for e in entries {
      (e as? ConnectionViewController)?.tearDown()
      e.removeFromParent()
    }
    entries.removeAll()
    currentID = nil
  }

  // MARK: Actions (responder chain)

  /// ⌘T: a connection form after the current entry
  @objc func newConnectionTab(_ sender: Any?) {
    addForm(after: currentID)
  }

  /// A SQLite file: into the current form's URI line, else connected at the end
  @objc func openSQLiteFile(_ sender: Any?) {
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    panel.message = "Choose a SQLite database file"
    let handler: (NSApplication.ModalResponse) -> Void = { [weak self] response in
      guard response == .OK, let url = panel.url, let self else { return }
      if let form = currentForm {
        form.setURI(url.path)
      } else {
        connect(to: url.path)
      }
    }
    if let window = view.window {
      panel.beginSheetModal(for: window, completionHandler: handler)
    } else {
      handler(panel.runModal())
    }
  }

  @objc func newQuery(_ sender: Any?) {
    if let current {
      current.newQuery(text: nil)
    } else {
      NSSound.beep()
    }
  }

  /// ⌘W: the current inner tab of a connection (its last one follows
  /// keep_connection_tab), on a connection with none the same rule, else the entry
  @objc func closeTab(_ sender: Any?) {
    if let current {
      if !current.closeCurrentTab() {
        lastViewClosed(current)
      }
    } else if let entry = currentEntry {
      closeEntry(id: entry.id)
    } else {
      view.window?.performClose(nil)
    }
  }

  /// ⇧⌘W: the current entry
  @objc func closeConnection(_ sender: Any?) {
    if let entry = currentEntry {
      closeEntry(id: entry.id)
    }
  }

  @objc func nextConnection(_ sender: Any?) {
    cycle(by: 1)
  }

  @objc func previousConnection(_ sender: Any?) {
    cycle(by: -1)
  }

  private func cycle(by delta: Int) {
    guard entries.count > 1, let i = entries.firstIndex(where: { $0.id == currentID }) else { return }
    select(entries[(i + delta + entries.count) % entries.count].id)
  }

  func validateUserInterfaceItem(_ item: any NSValidatedUserInterfaceItem) -> Bool {
    switch item.action {
    case #selector(newQuery(_:)):
      return current != nil
    case #selector(closeConnection(_:)):
      return currentEntry != nil
    case #selector(nextConnection(_:)), #selector(previousConnection(_:)):
      return entries.count > 1
    default:
      return true
    }
  }
}
