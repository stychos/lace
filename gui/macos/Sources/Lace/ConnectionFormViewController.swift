//
// Lace macOS frontend
// ConnectionFormViewController: a connection form as a workspace's strip
// entry (the connection tab design, as the GTK frontend's form page): the
// saved connections and Recent on the left, the URI line over the fields on
// the right, and a fixed bar with the open and manage buttons and the
// message line. The fields are the TUIs' shared model (LaceKit's
// ConnectionFields over core/conn_fields.c).
//
// The URI line goes into the fields when its editing ends or an action
// runs; a line that does not parse stops Open, Save and Test. Highlighting
// a row fills the form, after "Drop the changes in the form?" when it holds
// edits (a typed password included); Keep leaves highlight and fields as
// they were. Open uses the highlighted saved entry (its Keychain passwords
// too) only while the form is clean, so a form never connects to something
// other than what its fields show.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

/// Where Open puts the connection
enum FormDestination: Equatable {
  /// The form's place in its window
  case here
  /// A new entry after the form; the form stays selected
  case background
  /// A new window whose only entry it is
  case newWindow
  /// The end of the window made N-th, which comes to the front
  case toWindow(Int)
}

final class ConnectionFormViewController: NSViewController, NSTextFieldDelegate, NSSearchFieldDelegate,
  NSMenuDelegate, WorkspaceEntry
{
  typealias Field = ConnectionFields.Field

  let id = UUID()
  /// The workspace holding this entry (set when it is added)
  weak var workspace: WorkspaceViewController?
  /// The strip label: the highlighted saved connection's name, or "New connection"
  private(set) var label = "New connection"
  var onLabelChanged: (() -> Void)?
  /// An open or a test is running
  private(set) var isBusy = false

  private let fields = ConnectionFields()
  /// The highlighted saved connection (nil: none, or a recent row)
  private var highlightID: String?
  /// Its saved password did not come back from the Keychain (refused):
  /// Save leaves the item alone instead of removing it
  private var keychainMissing = false
  /// The row that filled the form
  private var picked: SavedListView.Key?
  /// Typed into the URI line and not applied yet
  private var uriEdited = false
  /// "Drop the changes?" is open: further picks wait for its answer
  private var asking: (key: SavedListView.Key, thenOpen: Bool)?

  private let search = NSSearchField()
  private let list = SavedListView(frame: .zero)
  private let uriField = NSTextField()
  private var texts: [Field: NSTextField] = [:]
  private var checks: [Field: NSButton] = [:]
  private let driverPopup = NSPopUpButton()
  private let folderPopup = NSPopUpButton()
  /// The Folder pop-up's folder by the names on its way from the root, kept
  /// apart from the pop-up: the store's folder list changes before it says so
  private var folderNames: [String] = []
  private var rows: [Field: NSGridRow] = [:]
  private let message = NSTextField(wrappingLabelWithString: "")
  private let spinner = NSProgressIndicator()
  private let toWindow = NSPopUpButton(frame: .zero, pullsDown: true)
  private var actionButtons: [NSButton] = []

  // MARK: Workspace entry

  var stripItem: TabStripView.Item {
    TabStripView.Item(
      id: id, title: label, symbol: "plus.circle",
      tooltip: "A connection form: pick a saved connection, or type a URL or a SQLite file")
  }

  func focusEntry() {
    view.window?.makeFirstResponder(uriField)
  }

  // MARK: View

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 1100, height: 680))

    // Left: the saved connections and Recent
    search.placeholderString = "Filter saved connections"
    search.delegate = self
    list.translatesAutoresizingMaskIntoConstraints = false
    list.openIn = { id in LaceApp.shared.windowsShowing(savedID: id) }
    list.onSelect = { [weak self] row in self?.requestPick(row) }
    list.onActivate = { [weak self] row in
      guard let self else { return }
      if row.key == picked {
        perform("open")
      } else {
        requestPick(row, thenOpen: true)
      }
    }
    list.onDuplicate = { [weak self] c in
      guard let self, let copy = SavedActions.duplicate(c, window: view.window) else { return }
      list.reload()
      requestPick(list.row(for: .saved(copy.id)))
    }
    list.onDelete = { [weak self] c in
      Task { [weak self] in
        guard let self, await SavedActions.delete(c, window: view.window) else { return }
        if highlightID == c.id {
          if isDirty {  // the edits stay, an unsaved copy now
            highlightID = nil
            keychainMissing = false
            picked = nil
            setLabel(nil)
          } else {  // nothing of the entry is left to show
            clear()
          }
        }
      }
    }
    list.onNewConnection = { [weak self] folder in
      guard let self else { return }
      clear()
      selectFolder(folder)
      focusEntry()
    }
    list.onNewFolder = { [weak self] parent in
      Task { [weak self] in await SavedActions.newFolder(in: parent, window: self?.view.window) }
    }
    list.onRenameFolder = { [weak self] f in
      Task { [weak self] in await SavedActions.renameFolder(f, window: self?.view.window) }
    }
    list.onDeleteFolder = { [weak self] f in SavedActions.deleteFolder(f, window: self?.view.window) }
    let folderButton = NSButton(
      image: NSImage(systemSymbolName: "folder.badge.plus", accessibilityDescription: "New folder")!, target: self,
      action: #selector(newFolderTapped))
    folderButton.bezelStyle = .rounded
    folderButton.toolTip = "New folder"
    let left = NSStackView(views: [search, list, folderButton])
    left.orientation = .vertical
    left.alignment = .leading
    left.spacing = 8
    left.translatesAutoresizingMaskIntoConstraints = false
    for v in [search, list] {
      v.widthAnchor.constraint(equalTo: left.widthAnchor).isActive = true
    }
    list.setContentHuggingPriority(.defaultLow, for: .vertical)
    left.widthAnchor.constraint(equalToConstant: 290).isActive = true

    // Right: the URI line over the fields
    uriField.placeholderString = "postgres://user@host/db, mysql://…, sqlite:///path, or a file path"
    uriField.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
    uriField.delegate = self
    uriField.target = self
    uriField.action = #selector(openHere)
    uriField.lineBreakMode = .byTruncatingMiddle
    uriField.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let uriLabel = NSTextField(labelWithString: "URI")
    uriLabel.textColor = .secondaryLabelColor
    uriLabel.font = .systemFont(ofSize: 13, weight: .semibold)
    let uriRow = NSStackView(views: [uriLabel, uriField])
    uriRow.orientation = .horizontal
    uriRow.spacing = 10

    let grid = makeGrid()
    let formScroll = NSScrollView()
    let flipped = FlippedView()
    flipped.translatesAutoresizingMaskIntoConstraints = false
    flipped.addSubview(grid)
    NSLayoutConstraint.activate([
      grid.topAnchor.constraint(equalTo: flipped.topAnchor, constant: 2),
      grid.leadingAnchor.constraint(equalTo: flipped.leadingAnchor),
      grid.trailingAnchor.constraint(equalTo: flipped.trailingAnchor, constant: -4),
      grid.bottomAnchor.constraint(equalTo: flipped.bottomAnchor, constant: -2),
    ])
    formScroll.documentView = flipped
    formScroll.hasVerticalScroller = true
    formScroll.drawsBackground = false
    formScroll.translatesAutoresizingMaskIntoConstraints = false
    flipped.widthAnchor.constraint(equalTo: formScroll.contentView.widthAnchor).isActive = true
    formScroll.setContentHuggingPriority(.defaultLow, for: .vertical)

    // The fixed bar: open, manage, the message line
    let openRow = buttonRow([
      button("Open Here", #selector(openHere), key: "\r", modifiers: [], tip: "Connect in this entry's place (Return)"),
      button(
        "Background Tab", #selector(openBackground), key: "\r", modifiers: [.option],
        tip: "Connect in a new entry after this one; the form stays (⌥Return)"),
      button(
        "New Window", #selector(openNewWindow), key: "\r", modifiers: [.command, .shift],
        tip: "Connect in a new window (⇧⌘Return)"),
      toWindowButton(),
    ])
    let manageRow = buttonRow([
      button("Save", #selector(saveTapped), key: "s", modifiers: [.command], tip: "Save the highlighted connection, or a new one (⌘S)"),
      button("Save as New", #selector(saveAsNewTapped), key: "s", modifiers: [.command, .shift], tip: "Save a new connection (⇧⌘S)"),
      button("Test", #selector(testTapped), key: "", modifiers: [], tip: "Connect and disconnect"),
      button("Clear", #selector(clearTapped), key: "", modifiers: [], tip: "Empty the form and drop the highlight"),
    ])
    spinner.style = .spinning
    spinner.controlSize = .small
    spinner.isDisplayedWhenStopped = false
    message.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    message.maximumNumberOfLines = 3
    message.lineBreakMode = .byTruncatingTail
    message.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let messageRow = NSStackView(views: [spinner, message])
    messageRow.orientation = .horizontal
    messageRow.spacing = 6
    let bar = NSStackView(views: [openRow, manageRow, messageRow])
    bar.orientation = .vertical
    bar.alignment = .leading
    bar.spacing = 8

    let right = NSStackView(views: [uriRow, SeparatorView(), formScroll, SeparatorView(), bar])
    right.orientation = .vertical
    right.alignment = .leading
    right.spacing = 10
    right.translatesAutoresizingMaskIntoConstraints = false
    for v in right.views {
      v.widthAnchor.constraint(equalTo: right.widthAnchor).isActive = true
    }
    message.widthAnchor.constraint(lessThanOrEqualTo: right.widthAnchor, constant: -30).isActive = true

    let columns = NSStackView(views: [left, right])
    columns.orientation = .horizontal
    columns.alignment = .top
    columns.distribution = .fill
    right.setContentHuggingPriority(.defaultLow - 1, for: .horizontal)
    left.setContentHuggingPriority(.required, for: .horizontal)
    columns.spacing = 24
    columns.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
    columns.translatesAutoresizingMaskIntoConstraints = false
    left.heightAnchor.constraint(equalTo: columns.heightAnchor, constant: -32).isActive = true
    right.heightAnchor.constraint(equalTo: columns.heightAnchor, constant: -32).isActive = true
    root.addSubview(columns)
    NSLayoutConstraint.activate([
      columns.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      columns.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      columns.topAnchor.constraint(equalTo: root.topAnchor),
      columns.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    view = root

    // Selector observers go with the controller by themselves
    for name in [SavedConnectionStore.didChange, RecentConnections.didChange, LaceApp.entriesDidChange] {
      NotificationCenter.default.addObserver(self, selector: #selector(listsChanged), name: name, object: nil)
    }
    refreshFolders()
    fieldsToWidgets()
  }

  /// The saved connections, Recent or a window's entries changed
  @objc private func listsChanged(_ note: Notification) {
    list.reload()
    refreshFolders()
  }

  private func makeGrid() -> NSGridView {
    for f in Field.allCases where f.isText {
      let t: NSTextField = (f == .password || f == .sshPassword) ? NSSecureTextField() : NSTextField()
      t.delegate = self
      t.target = self
      t.action = #selector(openHere)
      texts[f] = t
    }
    for f in Field.allCases where f.isCheck {
      let title = f == .ssh ? "SSH tunnel" : f == .savePassword ? "Save password" : "Save SSH password"
      let b = NSButton(checkboxWithTitle: title, target: self, action: #selector(checkChanged(_:)))
      b.tag = f.rawValue
      checks[f] = b
    }
    driverPopup.target = self
    driverPopup.action = #selector(driverChanged)
    texts[.file]?.placeholderString = "/path/to/database.sqlite"
    texts[.host]?.placeholderString = "localhost"
    texts[.port]?.placeholderString = "default"
    texts[.sshHost]?.placeholderString = "bastion.example.com"
    texts[.sshPort]?.placeholderString = "22"
    texts[.sshUser]?.placeholderString = "your login on the SSH host"
    texts[.sshKey]?.placeholderString = "ssh's own choice (~/.ssh/config, the agent)"
    texts[.port]?.widthAnchor.constraint(equalToConstant: 80).isActive = true
    texts[.sshPort]?.widthAnchor.constraint(equalToConstant: 80).isActive = true

    func hstack(_ views: [NSView], _ grow: NSView) -> NSStackView {
      let s = NSStackView(views: views)
      s.orientation = .horizontal
      s.spacing = 6
      grow.setContentHuggingPriority(.defaultLow, for: .horizontal)
      return s
    }
    let chooseFile = NSButton(title: "Choose…", target: self, action: #selector(chooseFile))
    chooseFile.bezelStyle = .rounded
    let chooseKey = NSButton(title: "Choose…", target: self, action: #selector(chooseKey))
    chooseKey.bezelStyle = .rounded

    let grid = NSGridView(numberOfColumns: 2, rows: 0)
    grid.rowSpacing = 8
    grid.columnSpacing = 10
    grid.column(at: 0).xPlacement = .trailing
    grid.column(at: 0).width = 110
    grid.rowAlignment = .firstBaseline
    func row(_ field: Field, _ label: String, _ view: NSView) {
      let l = NSTextField(labelWithString: label)
      l.textColor = .secondaryLabelColor
      rows[field] = grid.addRow(with: [l, view])
    }
    row(.name, "Name", texts[.name]!)
    row(.driver, "Driver", driverPopup)
    row(.file, "Database file", hstack([texts[.file]!, chooseFile], texts[.file]!))
    row(.host, "Host", hstack([texts[.host]!, NSTextField(labelWithString: "Port"), texts[.port]!], texts[.host]!))
    row(.database, "Database", texts[.database]!)
    row(.user, "User", texts[.user]!)
    row(.password, "Password", hstack([texts[.password]!, checks[.savePassword]!], texts[.password]!))
    rows[.ssh] = grid.addRow(with: [NSView(), checks[.ssh]!])
    row(
      .sshHost, "SSH host",
      hstack([texts[.sshHost]!, NSTextField(labelWithString: "Port"), texts[.sshPort]!], texts[.sshHost]!))
    row(.sshUser, "SSH user", texts[.sshUser]!)
    row(.sshKey, "SSH key", hstack([texts[.sshKey]!, chooseKey], texts[.sshKey]!))
    row(.sshPassword, "SSH password", hstack([texts[.sshPassword]!, checks[.saveSSHPassword]!], texts[.sshPassword]!))
    folderPopup.target = self
    folderPopup.action = #selector(folderPicked)
    let folderLabel = NSTextField(labelWithString: "Folder")
    folderLabel.textColor = .secondaryLabelColor
    grid.addRow(with: [folderLabel, folderPopup])
    grid.translatesAutoresizingMaskIntoConstraints = false
    return grid
  }

  private func button(_ title: String, _ action: Selector, key: String, modifiers: NSEvent.ModifierFlags, tip: String)
    -> NSButton
  {
    let b = NSButton(title: title, target: self, action: action)
    b.bezelStyle = .rounded
    b.keyEquivalent = key
    b.keyEquivalentModifierMask = modifiers
    b.toolTip = tip
    actionButtons.append(b)
    return b
  }

  private func toWindowButton() -> NSPopUpButton {
    toWindow.bezelStyle = .rounded
    toWindow.addItem(withTitle: "To Window")
    toWindow.menu?.delegate = self
    toWindow.toolTip = "Connect at the end of another window"
    return toWindow
  }

  private func buttonRow(_ views: [NSView]) -> NSStackView {
    let s = NSStackView(views: views)
    s.orientation = .horizontal
    s.spacing = 8
    return s
  }

  override func viewDidAppear() {
    super.viewDidAppear()
    list.reload()
  }

  // MARK: Fields and widgets

  private static func driverTitle(_ name: String) -> String {
    switch name {
    case "sqlite": "SQLite"
    case "postgres": "PostgreSQL"
    case "mysql": "MySQL"
    case "mariadb": "MariaDB"
    default: name
    }
  }

  /// Every widget from the fields, the URI line rebuilt
  private func fieldsToWidgets() {
    let drivers = fields.drivers
    if driverPopup.numberOfItems != drivers.count {
      driverPopup.removeAllItems()
      for d in drivers {
        driverPopup.addItem(withTitle: Self.driverTitle(d))
      }
    }
    driverPopup.selectItem(at: fields.driver)
    for (f, t) in texts {
      t.stringValue = fields[f]
    }
    for (f, b) in checks {
      b.state = fields[check: f] ? .on : .off
    }
    updateVisibility()
    syncURI()
    showMessage()
  }

  private func updateVisibility() {
    for (f, row) in rows {
      row.isHidden = !fields.isVisible(f)
    }
    let server = fields.driver != 0
    texts[.port]?.placeholderString = server ? String(SavedConnection.defaultPort(driver: fields.driverName)) : ""
  }

  private func syncURI() {
    uriField.stringValue = fields.uri
    uriEdited = false
  }

  private func showMessage() {
    let m = fields.message
    message.stringValue = m?.text ?? ""
    message.textColor = m?.isError == true ? .systemRed : .secondaryLabelColor
  }

  private func say(_ text: String?, error: Bool = false) {
    fields.setMessage(text, isError: error)
    showMessage()
  }

  /// What the URI line holds goes into the fields; false (the message says
  /// why, the text stays) when it does not parse
  @discardableResult
  private func commitURI() -> Bool {
    guard uriEdited else { return true }
    let text = uriField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
    if text.isEmpty {  // cleared: the fields stay as they are
      syncURI()
      return true
    }
    guard fields.setURI(text) else {
      showMessage()
      return false
    }
    fieldsToWidgets()
    return true
  }

  /// Edits not saved: in the fields, or typed into the URI line
  var isDirty: Bool { uriEdited || fields.isDirty }

  /// Nothing highlighted, nothing typed: a file opened from the Finder may take its place
  var isPristine: Bool { !isViewLoaded || (!isDirty && picked == nil && !isBusy) }

  private func setLabel(_ name: String?) {
    let l = (name ?? "").isEmpty ? "New connection" : name!
    if l != label {
      label = l
      onLabelChanged?()
    }
  }

  func controlTextDidChange(_ obj: Notification) {
    guard let control = obj.object as? NSControl else { return }
    if control === search {
      list.filter = search.stringValue
    } else if control === uriField {
      uriEdited = true
    } else if let f = texts.first(where: { $0.value === control })?.key {
      fields[f] = (control as? NSTextField)?.stringValue ?? ""
      syncURI()
    }
  }

  func controlTextDidEndEditing(_ obj: Notification) {
    if (obj.object as? NSControl) === uriField {
      commitURI()
    }
  }

  @objc private func checkChanged(_ sender: NSButton) {
    guard let f = Field(rawValue: sender.tag) else { return }
    fields[check: f] = sender.state == .on
    updateVisibility()
    syncURI()
  }

  @objc private func driverChanged() {
    fields.driver = driverPopup.indexOfSelectedItem
    updateVisibility()
    syncURI()
  }

  // MARK: Folders

  /// A folder by the names on its way from the root (paths are indices,
  /// which change when a connection before a folder moves)
  private func names(of path: [Int]) -> [String] {
    let folders = LaceApp.shared.saved.folders
    return (0..<path.count).map { n in folders.first { $0.path == Array(path[...n]) }?.name ?? "" }
  }

  private var selectedFolderPath: [Int] {
    let i = folderPopup.indexOfSelectedItem
    let folders = LaceApp.shared.saved.folders
    return i >= 0 && i < folders.count ? folders[i].path : []
  }

  private func refreshFolders() {
    folderPopup.removeAllItems()
    let folders = LaceApp.shared.saved.folders
    for f in folders {
      folderPopup.addItem(withTitle: f.isRoot ? "Top level" : String(repeating: "   ", count: f.depth - 1) + f.name)
    }
    let i = folders.firstIndex { names(of: $0.path) == folderNames }
    folderPopup.selectItem(at: i ?? 0)
    if i == nil {
      folderNames = []  // that folder is gone
    }
  }

  private func selectFolder(_ path: [Int]) {
    folderPopup.selectItem(at: LaceApp.shared.saved.folders.firstIndex { $0.path == path } ?? 0)
    folderNames = names(of: selectedFolderPath)
  }

  @objc private func folderPicked() {
    folderNames = names(of: selectedFolderPath)
  }

  // MARK: Highlight

  /// The user moved the list's selection: fill the form from that row,
  /// after the question when the form holds edits
  private func requestPick(_ row: SavedListView.Row?, thenOpen: Bool = false) {
    guard let key = row?.key else { return }
    if let pending = asking {
      // the double-click after the click that asked: open once answered
      if pending.key == key && thenOpen {
        asking = (key, true)
      }
      return
    }
    if key == picked {
      if thenOpen { perform("open") }
      return
    }
    guard isDirty else {
      apply(key)
      if thenOpen { perform("open") }
      return
    }
    asking = (key, thenOpen)
    Task { [weak self] in
      guard let self else { return }
      let drop = await Alerts.confirm(
        "Unsaved changes", "Drop the changes in the form?", action: "Drop", cancel: "Keep", window: view.window)
      let open = asking?.thenOpen == true
      asking = nil
      if drop, list.row(for: key) != nil {
        apply(key)
        if open { perform("open") }
      } else {
        list.select(key: picked)  // the highlight goes back
      }
    }
  }

  /// The row named by key becomes the highlight and fills the form
  private func apply(_ key: SavedListView.Key) {
    switch key {
    case .saved(let id):
      guard let c = LaceApp.shared.savedConnection(id: id) else { return }
      fields.fill(c)
      highlightID = id
      keychainMissing =
        LaceApp.shared.prefs.keychainPasswords
        && ((c.savePassword && c.password == nil) || (c.ssh && c.saveSSHPassword && c.sshPassword == nil))
      setLabel(c.name.isEmpty ? c.displayName : c.name)
      selectFolder(LaceApp.shared.saved.entry(id: id)?.folderPath ?? [])
    case .recent(let url):
      fields.clear()
      fields.setURI(url)
      fields.markClean()
      highlightID = nil
      keychainMissing = false
      setLabel(nil)
    }
    picked = key
    list.select(key: key)
    fieldsToWidgets()
  }

  // MARK: Actions

  /// open, background, new-window, to-window:N, save, save-as-new, test, clear
  func perform(_ action: String) {
    switch action {
    case "open": open(.here)
    case "background": open(.background)
    case "new-window": open(.newWindow)
    case "save": save(asNew: false)
    case "save-as-new": save(asNew: true)
    case "test": test()
    case "clear": clear()
    default:
      if action.hasPrefix("to-window:"), let n = Int(action.dropFirst("to-window:".count)) {
        open(.toWindow(n))
      }
    }
  }

  /// What Open would connect to: the highlighted saved entry while the form
  /// is clean (its Keychain passwords included), else the string the fields
  /// build; nil (the message says why) when they do not validate
  func openTarget() -> (connectionString: String, saved: SavedConnection?)? {
    if let id = highlightID, !fields.isDirty, let c = LaceApp.shared.savedConnection(id: id) {
      return (c.connectionString(), c)
    }
    guard let cs = fields.connectionString() else { return nil }
    return (cs, nil)
  }

  private func setBusy(_ on: Bool) {
    isBusy = on
    on ? spinner.startAnimation(nil) : spinner.stopAnimation(nil)
    for b in actionButtons {
      b.isEnabled = !on
    }
    toWindow.isEnabled = !on
  }

  private func open(_ destination: FormDestination) {
    guard !isBusy, commitURI() else { return }
    guard let target = openTarget() else {
      showMessage()
      return
    }
    say("Connecting…")
    setBusy(true)
    let window = view.window
    Task { [weak self] in
      let conn = await LaceApp.shared.connections.open(
        connectionString: target.connectionString, saved: target.saved, window: window,
        onError: { [weak self] why in self?.say(why, error: true) })
      guard let self else {
        if let conn { LaceApp.shared.connections.releaseIfUnheld(conn) }
        return
      }
      setBusy(false)
      guard let conn else {
        // a cancelled password prompt, or another open of it that failed
        if fields.message?.text == "Connecting…" {
          say(nil)
        }
        return
      }
      say(nil)
      place(conn, saved: target.saved, destination)
    }
  }

  private func place(_ conn: Connection, saved: SavedConnection?, _ destination: FormDestination) {
    switch destination {
    case .here, .background:
      guard let workspace else {
        LaceApp.shared.connections.releaseIfUnheld(conn)
        return
      }
      workspace.place(conn, saved: saved, from: self, background: destination == .background)
    case .newWindow:
      let wc = LaceApp.shared.newWorkspace(withForm: false)
      wc.workspace.append(conn, saved: saved, select: true, autoOpen: true)
    case .toWindow(let n):
      guard let wc = LaceApp.shared.workspace(number: n) else {
        say("That window was closed", error: true)
        LaceApp.shared.connections.releaseIfUnheld(conn)
        return
      }
      wc.workspace.append(conn, saved: saved, select: true, autoOpen: true)
      wc.showWindow(nil)
      wc.window?.makeKeyAndOrderFront(nil)
    }
  }

  private func test() {
    guard !isBusy, commitURI() else { return }
    guard let cs = fields.connectionString() else {
      showMessage()
      return
    }
    say("Testing…")
    setBusy(true)
    Task { [weak self] in
      let client = await LaceApp.shared.client()
      var outcome: (String, Bool) = ("Connection works", false)
      if !client.isConnected {
        outcome = ("The daemon did not answer: \(client.lastError)", true)
      } else {
        do {
          let conn = try await Connection.open(client: client, connectionString: cs)
          try? await conn.close()
        } catch {
          outcome = ("\(error)", true)
        }
      }
      guard let self else { return }
      setBusy(false)
      say(outcome.0, error: outcome.1)
    }
  }

  private func clear() {
    fields.clear()
    highlightID = nil
    keychainMissing = false
    picked = nil
    list.select(key: nil)
    selectFolder([])
    setLabel(nil)
    fieldsToWidgets()
  }

  private func save(asNew: Bool) {
    guard commitURI() else { return }
    guard var c = fields.collect(withSecrets: false) else {
      showMessage()
      return
    }
    let existing = asNew ? nil : highlightID.flatMap { LaceApp.shared.savedConnection(id: $0) }
    if let existing {
      c.id = existing.id
    }
    func holds(_ saved: Bool?, _ pw: String?, _ typed: String?) -> Bool { saved == true && pw != nil && pw == typed }
    let newPassword =
      c.savePassword && !(c.password ?? "").isEmpty && !holds(existing?.savePassword, existing?.password, c.password)
    let newSSHPassword =
      c.saveSSHPassword && !(c.sshPassword ?? "").isEmpty
      && !holds(existing?.saveSSHPassword, existing?.sshPassword, c.sshPassword)
    guard newPassword || newSSHPassword else {
      store(c)
      return
    }
    Task { [weak self] in
      guard let self else { return }
      let place = LaceApp.shared.prefs.keychainPasswords ? "in the Keychain" : "in connections.json (readable only by you)"
      let yes = await Alerts.ask(
        "Save password", "Store the password \(place)?", yes: "Store", no: "Don't Store", window: view.window)
      if !yes {  // saved without the passwords
        c.savePassword = false
        c.password = nil
        c.saveSSHPassword = false
        c.sshPassword = nil
        fields[check: .savePassword] = false
        fields[check: .saveSSHPassword] = false
      }
      store(c)
    }
  }

  /// Write c over the highlighted entry, or as a new one in the Folder
  /// pop-up's folder; the entry moves when the folder changed
  private func store(_ c: SavedConnection) {
    do {
      try LaceApp.shared.store(c, inFolder: selectedFolderPath, keepMissing: keychainMissing && c.id == highlightID)
    } catch {
      say("Not saved: \(error.localizedDescription)", error: true)
      return
    }
    // A saved URL is no longer a recent one
    if let cs = fields.connectionString() {
      LaceApp.shared.recents.remove(url: ConnectionKey.passwordless(cs))
    }
    highlightID = c.id
    picked = .saved(c.id)
    if fields[.name].isEmpty {
      fields[.name] = c.name
    }
    fields.markClean()
    setLabel(c.name)
    list.reload()
    list.select(key: picked)
    selectFolder(LaceApp.shared.saved.entry(id: c.id)?.folderPath ?? [])
    fieldsToWidgets()
    say("Saved \(c.name)")
  }

  @objc private func openHere() { perform("open") }
  @objc private func openBackground() { perform("background") }
  @objc private func openNewWindow() { perform("new-window") }
  @objc private func saveTapped() { perform("save") }
  @objc private func saveAsNewTapped() { perform("save-as-new") }
  @objc private func testTapped() { perform("test") }
  @objc private func clearTapped() { perform("clear") }

  @objc private func toWindowPicked(_ sender: NSMenuItem) {
    perform("to-window:\(sender.tag)")
  }

  /// To Window lists the other windows
  func menuNeedsUpdate(_ menu: NSMenu) {
    guard menu === toWindow.menu else { return }
    while menu.items.count > 1 {
      menu.removeItem(at: 1)
    }
    let others = LaceApp.shared.workspaces.filter { $0.workspace !== workspace }
    if others.isEmpty {
      let none = menu.addItem(withTitle: "No other window", action: nil, keyEquivalent: "")
      none.isEnabled = false
    }
    for wc in others {
      let item = menu.addItem(withTitle: wc.name, action: #selector(toWindowPicked(_:)), keyEquivalent: "")
      item.target = self
      item.tag = wc.number
    }
  }

  @objc private func newFolderTapped() {
    Task { [weak self] in await SavedActions.newFolder(in: [], window: self?.view.window) }
  }

  @objc private func chooseFile() {
    guard let window = view.window else { return }
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.message = "Choose a SQLite database file"
    panel.beginSheetModal(for: window) { [weak self] r in
      if r == .OK, let url = panel.url {
        MainActor.assumeIsolated { _ = self?.setField("file", url.path) }
      }
    }
  }

  @objc private func chooseKey() {
    guard let window = view.window else { return }
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.showsHiddenFiles = true
    panel.directoryURL = Prefs.homeDirectory.appendingPathComponent(".ssh")
    panel.message = "Choose a private key for ssh -i"
    panel.beginSheetModal(for: window) { [weak self] r in
      if r == .OK, let url = panel.url {
        MainActor.assumeIsolated { _ = self?.setField("ssh-key", url.path) }
      }
    }
  }

  // MARK: For the window and the script driver

  /// Typed into the URI line, then applied (as leaving it does)
  func setURI(_ text: String) {
    loadViewIfNeeded()
    uriField.stringValue = text
    uriEdited = true
    commitURI()
  }

  /// The URI line as shown
  var uriText: String { uriField.stringValue }

  /// The message line, if any
  var messageText: String? { fields.message?.text }

  /// Highlight the saved connection (or Recent entry) with that name, as a
  /// click does (through the question when the form holds edits); false if none
  func pick(name: String) -> Bool {
    let saved = LaceApp.shared.saved.entries.first { $0.connection.name == name }
    let key: SavedListView.Key
    if let saved {
      key = .saved(saved.connection.id)
    } else if let r = LaceApp.shared.recents.entries.first(where: {
      $0.url.contains(name) || SavedConnection(connectionString: $0.url)?.summary == name
    }) {
      key = .recent(r.url)
    } else {
      return false
    }
    list.reload()
    list.select(key: key)
    requestPick(list.row(for: key))
    return true
  }

  /// Set a field by its key (name, driver, file, host, port, database, user,
  /// password, save-password, ssh, ssh-host, ssh-port, ssh-user, ssh-key,
  /// ssh-password, save-ssh-password) as typing or a click would; false for
  /// an unknown key
  func setField(_ key: String, _ value: String) -> Bool {
    guard let f = Field(key: key) else { return false }
    if f == .driver {
      guard let i = fields.drivers.firstIndex(of: value) else { return false }
      driverPopup.selectItem(at: i)
      driverChanged()
    } else if f.isCheck {
      checks[f]?.state = value == "true" ? .on : .off
      if let b = checks[f] { checkChanged(b) }
    } else {
      texts[f]?.stringValue = value
      fields[f] = value
      syncURI()
    }
    return true
  }

  /// A field's value (a checkbox: "true"/"false"; the driver: its name)
  func field(_ key: String) -> String? {
    guard let f = Field(key: key) else { return nil }
    if f == .driver { return fields.driverName }
    if f.isCheck { return fields[check: f] ? "true" : "false" }
    return fields[f]
  }

  /// The folder pop-up's folder, by name ("" for the top level)
  func setFolder(named name: String) -> Bool {
    guard let f = LaceApp.shared.saved.folders.first(where: { name.isEmpty ? $0.isRoot : $0.name == name }) else {
      return false
    }
    selectFolder(f.path)
    return true
  }
}
