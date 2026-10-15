//
// Lace macOS frontend
// A workspace: one window whose tabs are database connections
//
// Connecting happens here (a typed connection string or SQLite path, an
// open panel, a password prompt and retry when the server refuses one).
// Closing a connection tab disconnects; with no tabs the window shows the
// welcome page.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class WorkspaceWindowController: NSWindowController, NSWindowDelegate {
  let workspace = WorkspaceViewController()
  private(set) var name: String
  private var forceClose = false

  init(name: String) {
    self.name = name
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
    if forceClose || !workspace.hasOpenTransaction {
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
    if !LaceApp.shared.isTerminating {
      if LaceApp.shared.workspaces.count > 1 {
        Session.save(except: self)
      } else {
        Session.save()
      }
    }
    workspace.closeAllConnections()
    LaceApp.shared.workspaceClosed(self)
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

final class WorkspaceViewController: NSViewController, NSUserInterfaceValidations {
  private let strip = TabStripView(frame: .zero)
  private let container = NSView()
  private let welcome = WelcomeViewController()
  private var connections: [ConnectionViewController] = []
  private var currentID: UUID?

  var current: ConnectionViewController? { connections.first { $0.id == currentID } }
  var hasOpenTransaction: Bool { connections.contains { $0.hasOpenTransaction } }
  /// In tab order (the session file)
  var connectionControllers: [ConnectionViewController] { connections }
  var currentIndex: Int? { connections.firstIndex { $0.id == currentID } }

  func select(index: Int) {
    guard index >= 0, index < connections.count else { return }
    select(connections[index].id)
  }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 1160, height: 740))
    strip.translatesAutoresizingMaskIntoConstraints = false
    strip.isHidden = true
    strip.onSelect = { [weak self] id in self?.select(id) }
    strip.onClose = { [weak self] id in self?.closeConnection(id: id) }
    strip.onCloseOthers = { [weak self] id in
      guard let self else { return }
      for vc in connections where vc.id != id {
        closeConnection(id: vc.id)
      }
    }
    strip.onCloseAll = { [weak self] in
      guard let self else { return }
      for vc in connections {
        closeConnection(id: vc.id)
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
    welcome.onConnect = { [weak self] text in self?.connect(to: text) }
    welcome.onOpenFile = { [weak self] in self?.openSQLiteFile(nil) }
    welcome.onConnectSaved = { [weak self] c in self?.connect(saved: c) }
    welcome.onEdit = { [weak self] c in self?.showConnectSheet(editing: c) }
    welcome.onConnectRecent = { [weak self] url in self?.connect(to: url) }
    welcome.onSaveRecent = { [weak self] url in self?.showConnectSheet(url: url) }
    addChild(welcome)
    view = root
    show(welcome)
  }

  override func viewDidAppear() {
    super.viewDidAppear()
    if current == nil {
      welcome.focus()
    }
  }

  private func show(_ vc: NSViewController) {
    for sub in container.subviews {
      sub.removeFromSuperview()
    }
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

  // MARK: Connecting

  /// A connection string, or the path of a SQLite file
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
    Task { _ = await open(connectionString: connectionString) }
  }

  /// A saved connection (its passwords from the Keychain or the file)
  func connect(saved: SavedConnection) {
    let full = LaceApp.shared.savedConnection(id: saved.id) ?? saved
    Task { _ = await open(connectionString: full.connectionString(), saved: full) }
  }

  /// Connect, asking for a password when the server (or the SSH host)
  /// refuses one and retrying with the string rebuilt around it, so a tunnel
  /// is kept; nil when it failed or was cancelled (the error was shown)
  @discardableResult
  func open(connectionString: String, saved: SavedConnection? = nil) async -> ConnectionViewController? {
    let window = view.window
    let client = await LaceApp.shared.client()
    guard client.isConnected else {
      Alerts.error("No daemon", "The daemon did not answer: \(client.lastError)", window: window)
      return nil
    }
    var cs = connectionString
    var attempts = 0
    while true {
      do {
        let conn = try await Connection.open(client: client, connectionString: cs)
        return add(conn, saved: saved)
      } catch let error as LaceError where attempts < 4 && (error.isAuthError || Self.looksLikeSSHAuth(error.message)) {
        attempts += 1
        let ssh = !error.isAuthError
        guard
          let typed = await Alerts.prompt(
            ssh ? "SSH password" : "Password", error.message, secure: true, action: "Connect", window: window)
        else { return nil }
        guard let model = saved ?? SavedConnection(connectionString: cs) else {
          Alerts.error("Could not connect", "The connection string could not be parsed", window: window)
          return nil
        }
        cs = model.connectionString(password: ssh ? nil : typed, sshPassword: ssh ? typed : nil)
      } catch {
        Alerts.error("Could not connect", "\(error)", window: window)
        return nil
      }
    }
  }

  /// The daemon passes ssh's own line ("Permission denied") for a refused tunnel login
  private static func looksLikeSSHAuth(_ message: String) -> Bool {
    let l = message.lowercased()
    return l.contains("permission denied") || l.contains("ssh")
  }

  /// The connect sheet, on a saved connection, a new one, or a URL (a
  /// recent connection to save) in the form
  func showConnectSheet(editing: SavedConnection? = nil, url: String? = nil) {
    let vc = ConnectViewController()
    vc.onConnect = { [weak self] cs, saved in
      Task { _ = await self?.open(connectionString: cs, saved: saved) }
    }
    presentAsSheet(vc)
    if let editing {
      vc.edit(editing)
    } else if let url {
      vc.fill(url: url)
    }
  }

  private func add(_ connection: Connection, saved: SavedConnection?) -> ConnectionViewController {
    if saved == nil, let url = connection.passwordlessConnectionString {
      LaceApp.shared.recents.add(url)
    }
    let vc = ConnectionViewController(connection: connection, prefs: LaceApp.shared.prefs, saved: saved)
    vc.onStateChanged = { [weak self] in self?.refreshStrip() }
    connections.append(vc)
    addChild(vc)
    refreshStrip()
    select(vc.id)
    return vc
  }

  private func refreshStrip() {
    strip.items = connections.map {
      TabStripView.Item(
        id: $0.id, title: $0.tabTitle, symbol: "cylinder", tooltip: $0.connection.displayName,
        badge: $0.transactionBadge)
    }
    strip.isHidden = connections.isEmpty
  }

  private func select(_ id: UUID?) {
    currentID = id
    strip.selectedID = id
    if let vc = current {
      show(vc)
      view.window?.subtitle = vc.connection.displayName
      vc.focusContent()
    } else {
      show(welcome)
      view.window?.subtitle = ""
      welcome.focus()
    }
  }

  func closeConnection(id: UUID) {
    guard let i = connections.firstIndex(where: { $0.id == id }) else { return }
    let vc = connections[i]
    if vc.hasOpenTransaction {
      Task { [weak self] in
        guard let self else { return }
        let ok = await Alerts.confirm(
          "Close \(vc.connection.shortName)?", "A transaction is open. Disconnecting rolls it back.",
          action: "Disconnect", window: view.window)
        if ok {
          remove(at: i)
        }
      }
      return
    }
    remove(at: i)
  }

  private func remove(at i: Int) {
    guard i < connections.count else { return }
    let vc = connections.remove(at: i)
    vc.tearDown()
    vc.view.removeFromSuperview()
    vc.removeFromParent()
    if currentID == vc.id {
      select(connections.isEmpty ? nil : connections[min(i, connections.count - 1)].id)
    }
    refreshStrip()
  }

  func closeAllConnections() {
    for vc in connections {
      vc.tearDown()
      vc.removeFromParent()
    }
    connections.removeAll()
    currentID = nil
  }

  // MARK: Actions (responder chain)

  @objc func connect(_ sender: Any?) {
    showConnectSheet(editing: nil)
  }

  @objc func openSQLiteFile(_ sender: Any?) {
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    panel.message = "Choose a SQLite database file"
    let handler: (NSApplication.ModalResponse) -> Void = { [weak self] response in
      guard response == .OK, let url = panel.url else { return }
      self?.connect(to: url.path)
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

  @objc func closeTab(_ sender: Any?) {
    if let current {
      if !current.closeCurrentTab() {
        closeConnection(id: current.id)
      }
    } else {
      view.window?.performClose(nil)
    }
  }

  @objc func closeConnection(_ sender: Any?) {
    if let current {
      closeConnection(id: current.id)
    }
  }

  @objc func nextConnection(_ sender: Any?) {
    cycle(by: 1)
  }

  @objc func previousConnection(_ sender: Any?) {
    cycle(by: -1)
  }

  private func cycle(by delta: Int) {
    guard connections.count > 1, let i = connections.firstIndex(where: { $0.id == currentID }) else { return }
    select(connections[(i + delta + connections.count) % connections.count].id)
  }

  func validateUserInterfaceItem(_ item: any NSValidatedUserInterfaceItem) -> Bool {
    switch item.action {
    case #selector(newQuery(_:)), #selector(closeConnection(_:)), #selector(nextConnection(_:)),
      #selector(previousConnection(_:)):
      return current != nil
    default:
      return true
    }
  }
}
