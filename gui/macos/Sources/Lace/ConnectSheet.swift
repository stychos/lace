//
// Lace macOS frontend
// Connect: a sheet with the saved connections on the left and a form on
// the right (the GTK connect dialog): a URL to connect to or to fill the
// form with, a tree of saved connections to connect to or edit, New, Test,
// Save, Duplicate, Delete. Connecting itself is the workspace's job
// (onConnect): the sheet closes first.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class ConnectViewController: NSViewController, NSTextFieldDelegate, NSSearchFieldDelegate {
  /// Connect with this string; the saved connection it came from, if any
  var onConnect: ((String, SavedConnection?) -> Void)?

  private static let drivers: [(name: String, driver: String)] = [
    ("SQLite", "sqlite"), ("PostgreSQL", "postgres"), ("MySQL", "mysql"), ("MariaDB", "mariadb"),
  ]

  private let urlField = NSTextField()
  private let search = NSSearchField()
  private let list = SavedListView(frame: .zero)
  private let formTitle = NSTextField(labelWithString: "New connection")
  private let nameField = NSTextField()
  private let driverPopup = NSPopUpButton()
  private let folderPopup = NSPopUpButton()
  private let fileField = NSTextField()
  private let fileRow = NSStackView()
  private let hostField = NSTextField()
  private let portField = NSTextField()
  private let dbField = NSTextField()
  private let userField = NSTextField()
  private let passwordField = NSSecureTextField()
  private let savePassword = NSButton(checkboxWithTitle: "Save password", target: nil, action: nil)
  private let sshCheck = NSButton(checkboxWithTitle: "SSH tunnel", target: nil, action: nil)
  private let sshHostField = NSTextField()
  private let sshPortField = NSTextField()
  private let sshUserField = NSTextField()
  private let sshKeyField = NSTextField()
  private let sshPasswordField = NSSecureTextField()
  private let saveSSHPassword = NSButton(checkboxWithTitle: "Save SSH password", target: nil, action: nil)
  private var serverRows: [NSView] = []
  private var sshRows: [NSView] = []
  private let banner = BannerView(frame: .zero)
  private let status = NSTextField(labelWithString: "")
  private let deleteButton = NSButton()
  private let duplicateButton = NSButton()
  private let testButton = NSButton()
  private let saveButton = NSButton()
  private let connectButton = NSButton()
  private let spinner = NSProgressIndicator()
  /// The saved connection in the form (nil: a new one)
  private var editing: SavedConnection?
  private var busy = false

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 920, height: 620))

    // A URL: connect at once, or put it into the form
    urlField.placeholderString = "postgres://user@host/db, mysql://…, sqlite:///path, or a file path"
    urlField.font = .systemFont(ofSize: 13)
    urlField.target = self
    urlField.action = #selector(connectURL)
    let fill = NSButton(title: "Fill Form", target: self, action: #selector(fillForm))
    fill.bezelStyle = .rounded
    fill.toolTip = "Put the URL into the form to edit or save it"
    let go = NSButton(title: "Connect", target: self, action: #selector(connectURL))
    go.bezelStyle = .rounded
    let urlRow = NSStackView(views: [urlField, fill, go])
    urlRow.orientation = .horizontal
    urlRow.spacing = 8
    urlField.setContentHuggingPriority(.defaultLow, for: .horizontal)

    // Saved connections
    search.placeholderString = "Search saved connections"
    search.delegate = self
    list.translatesAutoresizingMaskIntoConstraints = false
    list.onActivate = { [weak self] c in self?.connectSaved(c) }
    list.onSelect = { [weak self] c in
      if let c { self?.load(c) }
    }
    list.onEdit = { [weak self] c in self?.load(c) }
    list.onDuplicate = { [weak self] c in
      guard let self, let copy = SavedActions.duplicate(c, window: view.window) else { return }
      list.reload()
      list.select(id: copy.id)
      load(copy)
    }
    list.onDelete = { [weak self] c in self?.delete(c) }
    list.onNewConnection = { [weak self] folder in self?.newConnection(inFolder: folder) }
    list.onConnectRecent = { [weak self] url in
      self?.dismiss(nil)
      self?.onConnect?(url, nil)
    }
    list.onSaveRecent = { [weak self] url in self?.fill(url: url) }
    list.onRenameFolder = { [weak self] f in
      Task { [weak self] in
        await SavedActions.renameFolder(f, window: self?.view.window)
        self?.refreshFolders()
      }
    }
    list.onDeleteFolder = { [weak self] f in
      SavedActions.deleteFolder(f, window: self?.view.window)
      self?.refreshFolders()
    }
    let newButton = NSButton(title: "New", target: self, action: #selector(newTapped))
    newButton.bezelStyle = .rounded
    let folderButton = NSButton(
      image: NSImage(systemSymbolName: "folder.badge.plus", accessibilityDescription: "New folder")!, target: self,
      action: #selector(newFolderTapped))
    folderButton.bezelStyle = .rounded
    folderButton.toolTip = "New folder"
    let leftButtons = NSStackView(views: [newButton, folderButton])
    leftButtons.orientation = .horizontal
    leftButtons.spacing = 6
    let left = NSStackView(views: [search, list, leftButtons])
    left.orientation = .vertical
    left.alignment = .leading
    left.spacing = 8
    left.translatesAutoresizingMaskIntoConstraints = false
    for v in [search, list] {
      v.widthAnchor.constraint(equalTo: left.widthAnchor).isActive = true
    }
    list.setContentHuggingPriority(.defaultLow, for: .vertical)
    left.widthAnchor.constraint(equalToConstant: 300).isActive = true

    // The form
    formTitle.font = .systemFont(ofSize: 15, weight: .semibold)
    formTitle.lineBreakMode = .byTruncatingTail
    for (name, _) in ConnectViewController.drivers {
      driverPopup.addItem(withTitle: name)
    }
    driverPopup.target = self
    driverPopup.action = #selector(driverChanged)
    refreshFolders()
    let chooseFile = NSButton(title: "Choose…", target: self, action: #selector(chooseFile))
    chooseFile.bezelStyle = .rounded
    fileRow.orientation = .horizontal
    fileRow.spacing = 6
    fileRow.addArrangedSubview(fileField)
    fileRow.addArrangedSubview(chooseFile)
    fileField.setContentHuggingPriority(.defaultLow, for: .horizontal)
    fileField.placeholderString = "/path/to/database.sqlite"
    hostField.placeholderString = "localhost"
    portField.placeholderString = "default"
    portField.widthAnchor.constraint(equalToConstant: 80).isActive = true
    let hostRow = NSStackView(views: [hostField, NSTextField(labelWithString: "Port"), portField])
    hostRow.orientation = .horizontal
    hostRow.spacing = 6
    hostField.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let passwordRow = NSStackView(views: [passwordField, savePassword])
    passwordRow.orientation = .horizontal
    passwordRow.spacing = 8
    passwordField.setContentHuggingPriority(.defaultLow, for: .horizontal)
    sshCheck.target = self
    sshCheck.action = #selector(sshChanged)
    sshHostField.placeholderString = "bastion.example.com"
    sshPortField.placeholderString = "22"
    sshPortField.widthAnchor.constraint(equalToConstant: 80).isActive = true
    let sshHostRow = NSStackView(views: [sshHostField, NSTextField(labelWithString: "Port"), sshPortField])
    sshHostRow.orientation = .horizontal
    sshHostRow.spacing = 6
    sshHostField.setContentHuggingPriority(.defaultLow, for: .horizontal)
    sshUserField.placeholderString = "your login on the SSH host"
    sshKeyField.placeholderString = "ssh's own choice (~/.ssh/config, the agent)"
    let chooseKey = NSButton(title: "Choose…", target: self, action: #selector(chooseKey))
    chooseKey.bezelStyle = .rounded
    let sshKeyRow = NSStackView(views: [sshKeyField, chooseKey])
    sshKeyRow.orientation = .horizontal
    sshKeyRow.spacing = 6
    sshKeyField.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let sshPasswordRow = NSStackView(views: [sshPasswordField, saveSSHPassword])
    sshPasswordRow.orientation = .horizontal
    sshPasswordRow.spacing = 8
    sshPasswordField.setContentHuggingPriority(.defaultLow, for: .horizontal)

    let grid = NSGridView(numberOfColumns: 2, rows: 0)
    grid.rowSpacing = 8
    grid.columnSpacing = 10
    grid.column(at: 0).xPlacement = .trailing
    grid.column(at: 0).width = 110
    grid.rowAlignment = .firstBaseline
    func row(_ label: String, _ view: NSView) -> NSGridRow {
      let l = NSTextField(labelWithString: label)
      l.textColor = .secondaryLabelColor
      return grid.addRow(with: [l, view])
    }
    row("Name", nameField)
    row("Database", driverPopup)
    row("Folder", folderPopup)
    let fileGridRow = row("Database file", fileRow)
    let hostGridRow = row("Host", hostRow)
    let dbGridRow = row("Database name", dbField)
    let userGridRow = row("User", userField)
    let passwordGridRow = row("Password", passwordRow)
    let sshGridRow = grid.addRow(with: [NSView(), sshCheck])
    let sshHostGridRow = row("SSH host", sshHostRow)
    let sshUserGridRow = row("SSH user", sshUserField)
    let sshKeyGridRow = row("SSH key", sshKeyRow)
    let sshPasswordGridRow = row("SSH password", sshPasswordRow)
    _ = sshGridRow
    serverRows = [hostGridRow, dbGridRow, userGridRow, passwordGridRow].map { $0.cell(at: 1).contentView! }
    sshRows = [sshHostGridRow, sshUserGridRow, sshKeyGridRow, sshPasswordGridRow].map { $0.cell(at: 1).contentView! }
    gridRows = [
      fileGridRow, hostGridRow, dbGridRow, userGridRow, passwordGridRow, sshGridRow, sshHostGridRow, sshUserGridRow,
      sshKeyGridRow, sshPasswordGridRow,
    ]
    for f in [nameField, fileField, hostField, portField, dbField, userField, sshHostField, sshPortField, sshUserField, sshKeyField]
    {
      f.delegate = self
    }
    grid.translatesAutoresizingMaskIntoConstraints = false
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

    status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    status.textColor = .secondaryLabelColor
    status.lineBreakMode = .byTruncatingTail
    for (button, title, action) in [
      (deleteButton, "Delete…", #selector(deleteTapped)), (duplicateButton, "Duplicate", #selector(duplicateTapped)),
      (testButton, "Test", #selector(testTapped)), (saveButton, "Save", #selector(saveTapped)),
      (connectButton, "Connect", #selector(connectTapped)),
    ] {
      button.title = title
      button.bezelStyle = .rounded
      button.target = self
      button.action = action
    }
    connectButton.keyEquivalent = "\r"
    spinner.style = .spinning
    spinner.controlSize = .small
    spinner.isDisplayedWhenStopped = false
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let formButtons = NSStackView(views: [
      deleteButton, duplicateButton, spinner, status, spacer, testButton, saveButton, connectButton,
    ])
    formButtons.orientation = .horizontal
    formButtons.spacing = 8
    let right = NSStackView(views: [formTitle, banner, formScroll, formButtons])
    right.orientation = .vertical
    right.alignment = .leading
    right.spacing = 10
    right.translatesAutoresizingMaskIntoConstraints = false
    for v in [formTitle, banner, formScroll, formButtons] {
      v.widthAnchor.constraint(equalTo: right.widthAnchor).isActive = true
    }

    let columns = NSStackView(views: [left, right])
    columns.orientation = .horizontal
    columns.alignment = .top
    columns.spacing = 24
    columns.translatesAutoresizingMaskIntoConstraints = false
    left.heightAnchor.constraint(equalTo: columns.heightAnchor).isActive = true
    right.heightAnchor.constraint(equalTo: columns.heightAnchor).isActive = true
    columns.setContentHuggingPriority(.defaultLow, for: .vertical)

    let title = NSTextField(labelWithString: "Connect")
    title.font = .systemFont(ofSize: 15, weight: .semibold)
    let close = NSButton(title: "Close", target: self, action: #selector(closeTapped))
    close.bezelStyle = .rounded
    close.keyEquivalent = "\u{1b}"
    let titleSpacer = NSView()
    titleSpacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let titleRow = NSStackView(views: [title, titleSpacer, close])
    titleRow.orientation = .horizontal

    let stack = NSStackView(views: [titleRow, urlRow, SeparatorView(), columns])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 12
    stack.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
    stack.translatesAutoresizingMaskIntoConstraints = false
    for v in [titleRow, urlRow, columns] {
      v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40).isActive = true
    }
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    view = root
    LaceApp.shared.saved.onChanged = { [weak self] in
      self?.list.reload()
      self?.refreshFolders()
    }
    clearForm()
  }

  private var gridRows: [NSGridRow] = []

  override func viewDidAppear() {
    super.viewDidAppear()
    view.window?.makeFirstResponder(urlField)
  }

  override func viewWillDisappear() {
    super.viewWillDisappear()
    LaceApp.shared.saved.onChanged = nil
  }

  /// Open with a saved connection in the form
  func edit(_ c: SavedConnection) {
    loadViewIfNeeded()
    list.select(id: c.id)
    load(c)
  }

  /// A URL (a recent connection) in the form, to save or edit
  func fill(url: String) {
    loadViewIfNeeded()
    urlField.stringValue = url
    fillForm()
  }

  /// Open with an empty form for a folder
  func newConnection(inFolder folder: [Int]) {
    loadViewIfNeeded()
    clearForm()
    selectFolder(folder)
    view.window?.makeFirstResponder(nameField)
  }

  // MARK: Form state

  private var driver: String {
    ConnectViewController.drivers[max(0, driverPopup.indexOfSelectedItem)].driver
  }

  private func refreshFolders() {
    let selectedPath = selectedFolderPath
    folderPopup.removeAllItems()
    for f in LaceApp.shared.saved.folders {
      let title = f.isRoot ? "Top level" : String(repeating: "   ", count: f.depth - 1) + f.name
      folderPopup.addItem(withTitle: title)
    }
    selectFolder(selectedPath)
  }

  private var selectedFolderPath: [Int] {
    let i = folderPopup.indexOfSelectedItem
    let folders = LaceApp.shared.saved.folders
    return i >= 0 && i < folders.count ? folders[i].path : []
  }

  private func selectFolder(_ path: [Int]) {
    let folders = LaceApp.shared.saved.folders
    folderPopup.selectItem(at: folders.firstIndex { $0.path == path } ?? 0)
  }

  private func clearForm() {
    editing = nil
    formTitle.stringValue = "New connection"
    nameField.stringValue = ""
    driverPopup.selectItem(at: 0)
    selectFolder([])
    fileField.stringValue = ""
    hostField.stringValue = ""
    portField.stringValue = ""
    dbField.stringValue = ""
    userField.stringValue = ""
    passwordField.stringValue = ""
    savePassword.state = .off
    sshCheck.state = .off
    sshHostField.stringValue = ""
    sshPortField.stringValue = ""
    sshUserField.stringValue = ""
    sshKeyField.stringValue = ""
    sshPasswordField.stringValue = ""
    saveSSHPassword.state = .off
    banner.hide()
    status.stringValue = ""
    updateVisibility()
  }

  private func load(_ c: SavedConnection) {
    let full = LaceApp.shared.savedConnection(id: c.id) ?? c
    editing = full
    formTitle.stringValue = full.name.isEmpty ? "Connection" : full.name
    nameField.stringValue = full.name
    driverPopup.selectItem(at: ConnectViewController.drivers.firstIndex { $0.driver == full.driver } ?? 0)
    selectFolder(LaceApp.shared.saved.entry(id: full.id)?.folderPath ?? [])
    fileField.stringValue = full.isSQLite ? full.database : ""
    hostField.stringValue = full.host
    portField.stringValue = full.port > 0 ? String(full.port) : ""
    dbField.stringValue = full.isSQLite ? "" : full.database
    userField.stringValue = full.user
    passwordField.stringValue = full.password ?? ""
    savePassword.state = full.savePassword ? .on : .off
    sshCheck.state = full.ssh ? .on : .off
    sshHostField.stringValue = full.sshHost
    sshPortField.stringValue = full.sshPort > 0 ? String(full.sshPort) : ""
    sshUserField.stringValue = full.sshUser
    sshKeyField.stringValue = full.sshKey
    sshPasswordField.stringValue = full.sshPassword ?? ""
    saveSSHPassword.state = full.saveSSHPassword ? .on : .off
    banner.hide()
    status.stringValue = ""
    updateVisibility()
  }

  private func updateVisibility() {
    let sqlite = driver == "sqlite"
    let ssh = !sqlite && sshCheck.state == .on
    for (i, r) in gridRows.enumerated() {
      switch i {
      case 0: r.isHidden = !sqlite
      case 1...4: r.isHidden = sqlite
      case 5: r.isHidden = sqlite
      default: r.isHidden = !ssh
      }
    }
    portField.placeholderString = sqlite ? "" : String(SavedConnection.defaultPort(driver: driver))
    deleteButton.isEnabled = editing != nil && LaceApp.shared.saved.connection(id: editing?.id ?? "") != nil
    duplicateButton.isEnabled = deleteButton.isEnabled
  }

  private struct FormError: Error {
    let message: String
  }

  /// The form as a saved connection (the id of the one being edited)
  private func readForm() throws -> SavedConnection {
    var c = editing ?? SavedConnection()
    c.name = nameField.stringValue.trimmingCharacters(in: .whitespaces)
    c.driver = driver
    if c.isSQLite {
      let file = fileField.stringValue.trimmingCharacters(in: .whitespaces)
      guard !file.isEmpty else { throw FormError(message: "Choose a database file") }
      c.database = (file as NSString).expandingTildeInPath
      c.host = ""
      c.port = 0
      c.user = ""
      c.password = nil
      c.savePassword = false
      c.ssh = false
    } else {
      c.host = hostField.stringValue.trimmingCharacters(in: .whitespaces)
      if c.host.contains(where: { "/?#@ ".contains($0) }) {
        throw FormError(message: "The host may not hold / ? # @ or spaces")
      }
      c.port = try port(portField, "Port")
      c.database = dbField.stringValue.trimmingCharacters(in: .whitespaces)
      c.user = userField.stringValue.trimmingCharacters(in: .whitespaces)
      c.password = passwordField.stringValue.isEmpty ? nil : passwordField.stringValue
      c.savePassword = savePassword.state == .on
      c.ssh = sshCheck.state == .on
      if c.ssh {
        c.sshHost = sshHostField.stringValue.trimmingCharacters(in: .whitespaces)
        guard !c.sshHost.isEmpty else { throw FormError(message: "The SSH host is required for a tunnel") }
        c.sshPort = try port(sshPortField, "SSH port")
        c.sshUser = sshUserField.stringValue.trimmingCharacters(in: .whitespaces)
        c.sshKey = (sshKeyField.stringValue.trimmingCharacters(in: .whitespaces) as NSString).expandingTildeInPath
        c.sshPassword = sshPasswordField.stringValue.isEmpty ? nil : sshPasswordField.stringValue
        c.saveSSHPassword = saveSSHPassword.state == .on
      }
    }
    return c
  }

  private func port(_ field: NSTextField, _ what: String) throws -> Int {
    let text = field.stringValue.trimmingCharacters(in: .whitespaces)
    if text.isEmpty { return 0 }
    guard let n = Int(text), (1...65535).contains(n) else { throw FormError(message: "\(what): 1 to 65535") }
    return n
  }

  /// The string to connect with: the form's passwords, saved or not
  private func formConnectionString() throws -> (String, SavedConnection) {
    let c = try readForm()
    let cs = c.connectionString(password: c.password, sshPassword: c.sshPassword)
    return (cs, c)
  }

  private func setBusy(_ on: Bool) {
    busy = on
    on ? spinner.startAnimation(nil) : spinner.stopAnimation(nil)
    for b in [testButton, saveButton, connectButton] {
      b.isEnabled = !on
    }
  }

  // MARK: Actions

  @objc private func driverChanged() {
    updateVisibility()
  }

  @objc private func sshChanged() {
    updateVisibility()
  }

  func controlTextDidChange(_ obj: Notification) {
    if (obj.object as? NSSearchField) === search {
      list.filter = search.stringValue
    }
  }

  @objc private func chooseFile() {
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.canChooseDirectories = false
    panel.message = "Choose a SQLite database file"
    panel.beginSheetModal(for: view.window!) { [weak self] r in
      if r == .OK, let url = panel.url {
        self?.fileField.stringValue = url.path
      }
    }
  }

  @objc private func chooseKey() {
    let panel = NSOpenPanel()
    panel.canChooseFiles = true
    panel.showsHiddenFiles = true
    panel.directoryURL = Prefs.homeDirectory.appendingPathComponent(".ssh")
    panel.message = "Choose a private key for ssh -i"
    panel.beginSheetModal(for: view.window!) { [weak self] r in
      if r == .OK, let url = panel.url {
        self?.sshKeyField.stringValue = url.path
      }
    }
  }

  @objc private func fillForm() {
    let text = urlField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty else { return }
    var cs = text
    if !text.contains("://") {
      cs = "sqlite://\((text as NSString).expandingTildeInPath)"
    }
    guard let c = SavedConnection(connectionString: cs) else {
      banner.show("The URL could not be parsed")
      return
    }
    clearForm()
    var fresh = c
    fresh.id = SavedConnection.newID()
    fresh.savePassword = false
    fresh.saveSSHPassword = false
    editing = nil
    load(fresh)
    editing = nil
    formTitle.stringValue = "New connection"
    updateVisibility()
    view.window?.makeFirstResponder(nameField)
  }

  @objc private func connectURL() {
    let text = urlField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty else {
      connectTapped()
      return
    }
    var cs = text
    if !text.contains("://") {
      switch Driver.sqliteConnectionString(path: (text as NSString).expandingTildeInPath) {
      case .success(let built): cs = built
      case .failure(let error):
        banner.show(error.message)
        return
      }
    }
    dismiss(nil)
    onConnect?(cs, nil)
  }

  private func connectSaved(_ c: SavedConnection) {
    let full = LaceApp.shared.savedConnection(id: c.id) ?? c
    dismiss(nil)
    onConnect?(full.connectionString(), full)
  }

  @objc private func connectTapped() {
    do {
      let (cs, c) = try formConnectionString()
      // A saved connection keeps its history; a form never saved has none
      let saved = LaceApp.shared.saved.connection(id: c.id) != nil ? c : nil
      dismiss(nil)
      onConnect?(cs, saved)
    } catch let e as FormError {
      banner.show(e.message)
    } catch {
      banner.show("\(error)")
    }
  }

  @objc private func testTapped() {
    guard !busy else { return }
    let cs: String
    do {
      (cs, _) = try formConnectionString()
    } catch let e as FormError {
      banner.show(e.message)
      return
    } catch {
      banner.show("\(error)")
      return
    }
    banner.hide()
    status.stringValue = "Connecting…"
    setBusy(true)
    Task { [weak self] in
      let client = await LaceApp.shared.client()
      var message = "Connection OK"
      var ok = true
      if !client.isConnected {
        message = "The daemon did not answer: \(client.lastError)"
        ok = false
      } else {
        do {
          let conn = try await Connection.open(client: client, connectionString: cs)
          try? await conn.close()
        } catch {
          message = "\(error)"
          ok = false
        }
      }
      guard let self else { return }
      setBusy(false)
      status.stringValue = ok ? message : ""
      if !ok {
        banner.show(message)
      }
    }
  }

  @objc private func saveTapped() {
    do {
      var c = try readForm()
      if c.name.isEmpty {
        c.name = c.summary
      }
      try LaceApp.shared.store(c, inFolder: selectedFolderPath)
      list.reload()
      list.select(id: c.id)
      editing = c
      formTitle.stringValue = c.name
      banner.hide()
      status.stringValue = "Saved"
      updateVisibility()
    } catch let e as FormError {
      banner.show(e.message)
    } catch {
      banner.show("\(error)")
    }
  }

  @objc private func deleteTapped() {
    if let editing {
      delete(editing)
    }
  }

  private func delete(_ c: SavedConnection) {
    Task { [weak self] in
      guard let self else { return }
      if await SavedActions.delete(c, window: view.window) {
        list.reload()
        if editing?.id == c.id {
          clearForm()
        }
      }
    }
  }

  @objc private func duplicateTapped() {
    guard let editing, let copy = SavedActions.duplicate(editing, window: view.window) else { return }
    list.reload()
    list.select(id: copy.id)
    load(copy)
  }

  @objc private func newTapped() {
    newConnection(inFolder: selectedFolderPath)
  }

  @objc private func newFolderTapped() {
    Task { [weak self] in
      await SavedActions.newFolder(in: [], window: self?.view.window)
      self?.refreshFolders()
    }
  }

  @objc private func closeTapped() {
    dismiss(nil)
  }
}
