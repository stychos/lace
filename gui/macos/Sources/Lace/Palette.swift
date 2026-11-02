//
// Lace macOS frontend
// The command palette (View ▸ Command Palette…, ⇧⌘P) and the pick list it
// is built on (the database switch uses it too): a panel over the window
// with a search field and one list of sections, filtered as the TUIs'
// palette is (core/fuzzy.c through LaceKit's Fuzzy; sections ordered by
// their best match, rows likewise, rows that do not match and the sections
// left empty hidden). ↑↓ and Page Up/Down move, Return or a click runs, Esc
// or a click outside closes.
//
// The palette's rows: every enabled menu item (its target found and
// validated in the window's responder chain when the palette opens, and
// its action sent there once the panel is gone, so the palette has
// every command the menus have and nothing is written twice), then the
// current connection's tables and objects, its tabs, the other windows,
// the saved workspaces, the saved connections and Recent, and the
// preferences' on/off settings.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

/// One row: something to run, under a section heading
struct PaletteRow {
  let section: String
  let title: String
  var detail: String? = nil
  var keys: String? = nil
  /// Marked (the current database)
  var marked = false
  let run: @MainActor () -> Void
}

@MainActor
final class PaletteController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate,
  NSWindowDelegate
{
  /// The palette or pick list on screen (the script driver)
  private(set) static weak var current: PaletteController?

  private enum Line {
    case header(String)
    case row(Int)
  }

  private let panel: PalettePanel
  private let search = NSSearchField()
  private let table = NSTableView()
  private let titleLabel = NSTextField(labelWithString: "")
  private let rows: [PaletteRow]
  private var lines: [Line] = []
  private weak var parent: NSWindow?
  /// Kept alive while the panel shows
  private var selfRef: PaletteController?
  private var closed = false

  init(title: String?, placeholder: String, rows: [PaletteRow], parent: NSWindow) {
    self.rows = rows
    self.parent = parent
    let width = min(660, max(360, parent.frame.width - 80))
    let height = min(460, max(220, parent.frame.height - 140))
    panel = PalettePanel(
      contentRect: NSRect(x: 0, y: 0, width: width, height: height),
      styleMask: [.titled, .fullSizeContentView], backing: .buffered, defer: false)
    super.init()
    panel.titleVisibility = .hidden
    panel.titlebarAppearsTransparent = true
    panel.isMovable = false
    panel.hidesOnDeactivate = false
    panel.becomesKeyOnlyIfNeeded = false
    for b in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
      panel.standardWindowButton(b)?.isHidden = true
    }
    panel.delegate = self
    panel.onCancel = { [weak self] in self?.close() }
    panel.contentView = makeContent(title: title, placeholder: placeholder)
    filter("")
  }

  private func makeContent(title: String?, placeholder: String) -> NSView {
    let effect = NSVisualEffectView()
    effect.material = .menu
    effect.blendingMode = .behindWindow
    effect.state = .active

    titleLabel.stringValue = title ?? ""
    titleLabel.font = .systemFont(ofSize: NSFont.systemFontSize, weight: .semibold)
    titleLabel.isHidden = title == nil
    search.placeholderString = placeholder
    search.delegate = self
    search.sendsSearchStringImmediately = true
    search.focusRingType = .none
    search.font = .systemFont(ofSize: 15)
    search.controlSize = .large

    let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("row"))
    column.resizingMask = .autoresizingMask
    table.addTableColumn(column)
    table.headerView = nil
    table.style = .plain
    table.rowHeight = 24
    table.intercellSpacing = NSSize(width: 0, height: 2)
    table.backgroundColor = .clear
    table.dataSource = self
    table.delegate = self
    table.target = self
    table.action = #selector(clicked(_:))
    table.refusesFirstResponder = true
    // Headings scroll with the rows: a floating one would cover the first row
    table.floatsGroupRows = false
    table.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
    let scroll = NSScrollView()
    scroll.documentView = table
    scroll.hasVerticalScroller = true
    scroll.autohidesScrollers = true
    scroll.drawsBackground = false
    scroll.borderType = .noBorder

    let stack = NSStackView(views: [titleLabel, search, scroll])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 8
    stack.edgeInsets = NSEdgeInsets(top: 14, left: 12, bottom: 10, right: 12)
    stack.translatesAutoresizingMaskIntoConstraints = false
    for v in [titleLabel, search, scroll] {
      v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -24).isActive = true
    }
    scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
    effect.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: effect.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: effect.trailingAnchor),
      stack.topAnchor.constraint(equalTo: effect.topAnchor),
      stack.bottomAnchor.constraint(equalTo: effect.bottomAnchor),
    ])
    return effect
  }

  /// Over the parent window, centred, near its top; the search field has the keyboard
  func show() {
    guard let parent else { return }
    PaletteController.current?.close()
    PaletteController.current = self
    selfRef = self
    let f = parent.frame
    let size = panel.frame.size
    panel.setFrameOrigin(NSPoint(x: f.midX - size.width / 2, y: f.maxY - 90 - size.height))
    parent.addChildWindow(panel, ordered: .above)
    panel.makeKeyAndOrderFront(nil)
    panel.makeFirstResponder(search)
    // Filtered before the panel had a size: from the top, the first heading shown
    panel.contentView?.layoutSubtreeIfNeeded()
    table.scrollRowToVisible(0)
  }

  func close() {
    guard !closed else { return }
    closed = true
    parent?.removeChildWindow(panel)
    panel.orderOut(nil)
    if PaletteController.current === self {
      PaletteController.current = nil
    }
    DispatchQueue.main.async { [self] in selfRef = nil }
  }

  func windowDidResignKey(_ notification: Notification) {
    // A click outside closes it (not in a scripted run, whose app is never active)
    if !ScriptDriver.isRequested {
      close()
    }
  }

  // MARK: Filtering

  /// The rows matching text: sections by their best score, rows likewise
  /// (both stable); everything in order for an empty text
  func filter(_ text: String) {
    let needle = text.trimmingCharacters(in: .whitespaces)
    var sections: [(name: String, best: Int, rows: [(index: Int, score: Int)])] = []
    for (i, r) in rows.enumerated() {
      let score = needle.isEmpty ? 0 : Fuzzy.score(needle, r.title)
      if sections.last?.name != r.section {
        sections.append((r.section, -1, []))
      }
      guard score >= 0 else { continue }
      sections[sections.count - 1].rows.append((i, score))
      sections[sections.count - 1].best = max(sections[sections.count - 1].best, score)
    }
    sections = sections.filter { !$0.rows.isEmpty }
    if !needle.isEmpty {
      sections = sections.enumerated().sorted { a, b in
        a.element.best != b.element.best ? a.element.best > b.element.best : a.offset < b.offset
      }.map(\.element)
    }
    lines = []
    for s in sections {
      if !s.name.isEmpty {
        lines.append(.header(s.name))
      }
      let ordered =
        needle.isEmpty
        ? s.rows
        : s.rows.enumerated().sorted { a, b in
          a.element.score != b.element.score ? a.element.score > b.element.score : a.offset < b.offset
        }.map(\.element)
      lines += ordered.map { .row($0.index) }
    }
    table.reloadData()
    table.scrollRowToVisible(0)  // the first section's heading too
    select(lines.firstIndex { if case .row = $0 { return true } else { return false } })
  }

  private func select(_ line: Int?) {
    guard let line else {
      table.deselectAll(nil)
      return
    }
    table.selectRowIndexes(IndexSet(integer: line), byExtendingSelection: false)
    table.scrollRowToVisible(line)
  }

  /// Move the selection by delta rows, over the headings
  private func move(_ delta: Int) {
    let rowLines = lines.indices.filter { if case .row = lines[$0] { return true } else { return false } }
    guard !rowLines.isEmpty else { return }
    let cur = rowLines.firstIndex(of: table.selectedRow) ?? (delta > 0 ? -1 : rowLines.count)
    select(rowLines[min(max(cur + delta, 0), rowLines.count - 1)])
  }

  // MARK: Running

  private func run(line: Int) {
    guard line >= 0, line < lines.count, case .row(let i) = lines[line] else { return }
    let row = rows[i]
    close()
    parent?.makeKeyAndOrderFront(nil)
    // A turn later: the window is key again and its responder chain is the one acted on
    DispatchQueue.main.async { row.run() }
  }

  func runSelected() {
    run(line: table.selectedRow)
  }

  @objc private func clicked(_ sender: Any?) {
    run(line: table.clickedRow)
  }

  // MARK: For the script driver

  /// The rows shown, in order (headings left out)
  var visibleTitles: [String] {
    lines.compactMap { if case .row(let i) = $0 { return rows[i].title } else { return nil } }
  }

  /// The panel's content (the script driver's screenshots)
  var contentView: NSView? { panel.contentView }

  /// The selected row's title
  var selectedTitle: String? {
    let line = table.selectedRow
    guard line >= 0, line < lines.count, case .row(let i) = lines[line] else { return nil }
    return rows[i].title
  }

  func setFilter(_ text: String) {
    search.stringValue = text
    filter(text)
  }

  /// Run the first row shown whose title is text, else the first containing it
  @discardableResult
  func run(titled text: String) -> Bool {
    let match =
      lines.firstIndex { if case .row(let i) = $0 { return rows[i].title == text } else { return false } }
      ?? lines.firstIndex { if case .row(let i) = $0 { return rows[i].title.contains(text) } else { return false } }
    guard let match else { return false }
    run(line: match)
    return true
  }

  // MARK: NSSearchFieldDelegate

  func controlTextDidChange(_ obj: Notification) {
    filter(search.stringValue)
  }

  func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
    switch selector {
    case #selector(NSResponder.moveUp(_:)): move(-1)
    case #selector(NSResponder.moveDown(_:)): move(1)
    case #selector(NSResponder.scrollPageUp(_:)), #selector(NSResponder.pageUp(_:)): move(-10)
    case #selector(NSResponder.scrollPageDown(_:)), #selector(NSResponder.pageDown(_:)): move(10)
    case #selector(NSResponder.insertNewline(_:)): runSelected()
    case #selector(NSResponder.cancelOperation(_:)): close()
    default: return false
    }
    return true
  }

  // MARK: NSTableViewDataSource / Delegate

  func numberOfRows(in tableView: NSTableView) -> Int { lines.count }

  func tableView(_ tableView: NSTableView, isGroupRow row: Int) -> Bool {
    if case .header = lines[row] { return true }
    return false
  }

  func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
    if case .row = lines[row] { return true }
    return false
  }

  func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
    PaletteRowView()
  }

  func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat {
    if case .header = lines[row] { return 20 }
    return 24
  }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    switch lines[row] {
    case .header(let name):
      let id = NSUserInterfaceItemIdentifier("header")
      let cell = tableView.makeView(withIdentifier: id, owner: nil) as? TextCellView ?? TextCellView(frame: .zero)
      cell.identifier = id
      cell.label.stringValue = name.uppercased()
      cell.label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
      cell.label.textColor = .secondaryLabelColor
      return cell
    case .row(let i):
      let id = NSUserInterfaceItemIdentifier("item")
      let cell = tableView.makeView(withIdentifier: id, owner: nil) as? PaletteCellView ?? PaletteCellView()
      cell.identifier = id
      cell.configure(rows[i])
      return cell
    }
  }
}

/// Becomes key (a titled panel with its title bar hidden); Esc closes
final class PalettePanel: NSPanel {
  var onCancel: (() -> Void)?
  override var canBecomeKey: Bool { true }
  override func cancelOperation(_ sender: Any?) { onCancel?() }
}

/// The selection draws as active (the accent colour): the search field
/// keeps the keyboard, so the table never holds the focus itself
final class PaletteRowView: NSTableRowView {
  override var isEmphasized: Bool {
    get { true }
    set {}
  }
}

/// A row: the title (bold when marked), the detail after it, the keys on the right
final class PaletteCellView: NSTableCellView {
  private let title = NSTextField(labelWithString: "")
  private let detail = NSTextField(labelWithString: "")
  private let keys = NSTextField(labelWithString: "")

  init() {
    super.init(frame: .zero)
    title.lineBreakMode = .byTruncatingTail
    title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    detail.lineBreakMode = .byTruncatingTail
    detail.textColor = .secondaryLabelColor
    detail.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    detail.setContentCompressionResistancePriority(.defaultLow - 1, for: .horizontal)
    keys.textColor = .secondaryLabelColor
    keys.font = .monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
    keys.alignment = .right
    keys.setContentCompressionResistancePriority(.required, for: .horizontal)
    keys.setContentHuggingPriority(.required, for: .horizontal)
    let spacer = NSView()
    spacer.setContentHuggingPriority(.init(1), for: .horizontal)
    let stack = NSStackView(views: [title, detail, spacer, keys])
    stack.orientation = .horizontal
    stack.alignment = .firstBaseline
    stack.spacing = 8
    stack.edgeInsets = NSEdgeInsets(top: 0, left: 8, bottom: 0, right: 8)
    stack.translatesAutoresizingMaskIntoConstraints = false
    addSubview(stack)
    textField = title
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: trailingAnchor),
      stack.centerYAnchor.constraint(equalTo: centerYAnchor),
    ])
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  /// On the selection (the accent colour) every label turns white
  override var backgroundStyle: NSView.BackgroundStyle {
    didSet {
      let on = backgroundStyle == .emphasized
      detail.textColor = on ? .alternateSelectedControlTextColor : .secondaryLabelColor
      keys.textColor = on ? .alternateSelectedControlTextColor : .secondaryLabelColor
    }
  }

  func configure(_ row: PaletteRow) {
    title.stringValue = row.title
    title.font = .systemFont(ofSize: NSFont.systemFontSize, weight: row.marked ? .semibold : .regular)
    detail.stringValue = row.detail ?? ""
    detail.isHidden = row.detail == nil
    keys.stringValue = row.keys ?? ""
    keys.isHidden = row.keys == nil
  }
}

// MARK: The command palette's rows

@MainActor
enum CommandPalette {
  /// Open the palette over workspace's window
  static func show(for workspace: WorkspaceViewController) {
    guard let window = workspace.view.window else { return }
    // Built now, while the window is key: the menu items validate against its chain
    let rows = actionRows(in: window) + workspaceRows(workspace) + connectRows(workspace) + preferenceRows(workspace)
    PaletteController(title: nil, placeholder: "Type a command, a table, a connection…", rows: rows, parent: window)
      .show()
  }

  /// Every menu item enabled for window, with its menu path and keys
  private static func actionRows(in window: NSWindow) -> [PaletteRow] {
    guard let main = NSApp.mainMenu else { return [] }
    var rows: [PaletteRow] = []
    let skip: Set<Selector> = [
      #selector(WorkspaceViewController.showPalette(_:)),
      #selector(NSWindow.makeKeyAndOrderFront(_:)),  // the Window menu's list of windows
      #selector(AppDelegate.openSavedWorkspace(_:)),  // the saved workspaces have rows of their own
    ]
    func walk(_ menu: NSMenu, _ path: [String]) {
      for item in menu.items where !item.isSeparatorItem && !item.isHidden {
        if let sub = item.submenu {
          walk(sub, path + [item.title])
          continue
        }
        guard let action = item.action, !skip.contains(action),
          let target = enabledTarget(of: item, action: action, in: window), !item.title.isEmpty
        else { continue }
        rows.append(
          PaletteRow(
            section: "Actions", title: item.title, detail: path.joined(separator: " ▸ "),
            keys: keyString(item)
          ) {
            NSApp.sendAction(action, to: target, from: item)
          })
      }
    }
    for top in main.items {
      if let sub = top.submenu {
        walk(sub, [top.title])
      }
    }
    return rows
  }

  /// What the item's action reaches from window (its responder chain from
  /// the first responder, the window's delegate, the application and its
  /// delegate, as AppKit sends it), when that validates the item; nil when
  /// nothing takes it or the item is disabled there. Validated here rather
  /// than by NSMenu.update(), which asks the key window's chain: the window
  /// the palette opened over is the one acted on (also when it is not key,
  /// as in a scripted run). Validation may retitle the item ("Undo Edit of name").
  private static func enabledTarget(of item: NSMenuItem, action: Selector, in window: NSWindow) -> AnyObject? {
    var target: AnyObject?
    if let t = item.target {
      target = t.responds(to: action) ? t : nil
    } else {
      var r: NSResponder? = window.firstResponder
      while let cur = r, target == nil {
        if cur.responds(to: action) {
          target = cur
        }
        r = cur.nextResponder
      }
      if target == nil {
        let rest: [AnyObject?] = [window.delegate, NSApp, NSApp.delegate]
        target = rest.compactMap { $0 }.first { $0.responds(to: action) }
      }
    }
    guard let target else { return nil }
    if let v = target as? NSMenuItemValidation, !v.validateMenuItem(item) {
      return nil
    }
    if !(target is NSMenuItemValidation), let v = target as? NSUserInterfaceValidations,
      !v.validateUserInterfaceItem(item)
    {
      return nil
    }
    return target
  }

  /// "⇧⌘P" for a menu item's key equivalent (nil when it has none)
  static func keyString(_ item: NSMenuItem) -> String? {
    let key = item.keyEquivalent
    guard !key.isEmpty else { return nil }
    var mods = item.keyEquivalentModifierMask
    var name: String
    switch key {
    case "\r": name = "↩"
    case "\t": name = "⇥"
    case "\u{8}", "\u{7f}": name = "⌫"
    case " ": name = "Space"
    default:
      name = key.uppercased()
      if key != key.lowercased() {
        mods.insert(.shift)  // an upper-case key equivalent is typed with Shift
      }
    }
    var s = ""
    if mods.contains(.control) { s += "⌃" }
    if mods.contains(.option) { s += "⌥" }
    if mods.contains(.shift) { s += "⇧" }
    if mods.contains(.command) { s += "⌘" }
    return s + name
  }

  /// The current connection's tables, objects and tabs; the other windows;
  /// the saved workspaces
  private static func workspaceRows(_ workspace: WorkspaceViewController) -> [PaletteRow] {
    var rows: [PaletteRow] = []
    if let vc = workspace.current {
      for t in vc.sidebar.tables {
        rows.append(PaletteRow(section: "Tables", title: "Open table \(t)") { [weak vc] in vc?.content.openTable(t, preview: false) })
      }
      for o in vc.sidebar.objects where o.kind != .table {
        let word = o.kind.word.lowercased()
        rows.append(
          PaletteRow(section: "Tables", title: "Open \(word) \(o.name)", detail: o.table) { [weak vc] in
            vc?.content.openObject(o, preview: false)
          })
      }
      for (i, tab) in vc.content.stripView.items.enumerated() {
        rows.append(PaletteRow(section: "Tabs", title: tab.title, detail: "tab \(i + 1)") { [weak vc] in vc?.content.select(index: i) })
      }
    }
    for wc in LaceApp.shared.workspaces where wc.workspace !== workspace {
      rows.append(
        PaletteRow(section: "Workspaces", title: "Switch to \(wc.name)") { [weak wc] in
          wc?.window?.makeKeyAndOrderFront(nil)
        })
    }
    for layout in (try? SavedWorkspaces.list()) ?? [] {
      let name = layout.name
      rows.append(
        PaletteRow(section: "Workspaces", title: "Open saved workspace \(name)") {
          SavedWorkspaceActions.open(name: name)
        })
      rows.append(
        PaletteRow(section: "Workspaces", title: "Delete saved workspace \(name)") { [weak workspace] in
          SavedWorkspaceActions.delete(name: name, window: workspace?.view.window)
        })
    }
    return rows
  }

  /// "Connect to" the saved connections (their folder as the detail) and Recent
  private static func connectRows(_ workspace: WorkspaceViewController) -> [PaletteRow] {
    var rows: [PaletteRow] = []
    let store = LaceApp.shared.saved
    for entry in store.entries {
      let folders = store.folderNames(of: entry.folderPath)
      rows.append(
        PaletteRow(
          section: "Connect", title: "Connect to \(entry.connection.name)",
          detail: folders.isEmpty ? nil : folders.joined(separator: "/")
        ) { [weak workspace] in
          workspace?.connect(savedID: entry.connection.id)
        })
    }
    for recent in LaceApp.shared.recents.entries {
      rows.append(
        PaletteRow(section: "Connect", title: "Connect to \(recent.url)", detail: "recent") { [weak workspace] in
          workspace?.connect(to: recent.url)
        })
    }
    return rows
  }

  /// The preferences' on/off settings ("name: on"), toggled and saved, and the colour scheme
  private static func preferenceRows(_ workspace: WorkspaceViewController) -> [PaletteRow] {
    let p = LaceApp.shared.prefs
    let toggles: [(String, WritableKeyPath<Prefs, Bool>)] = [
      ("Ask before deleting rows", \.confirmDelete),
      ("Estimate the count of big server tables", \.approximateCounts),
      ("Reopen the last session at launch", \.restoreSession),
      ("Restore the cursor position", \.restoreCursor),
      ("Ask before quitting", \.confirmQuit),
      ("Keep a connection form when the last tab closes", \.keepConnectionTab),
      ("Open the first table on connect", \.autoOpenFirstTable),
      ("Keep saved passwords in the Keychain", \.keychainPasswords),
    ]
    var rows = toggles.map { name, key in
      PaletteRow(section: "Preferences", title: "\(name): \(p[keyPath: key] ? "on" : "off")") { [weak workspace] in
        var q = LaceApp.shared.prefs
        q[keyPath: key].toggle()
        save(q, "\(name): \(q[keyPath: key] ? "on" : "off")", workspace)
      }
    }
    let names: [SharedConfig.ColorScheme: String] = [.system: "System", .light: "Light", .dark: "Dark"]
    let next = SharedConfig.ColorScheme(rawValue: (p.colorScheme.rawValue + 1) % 3) ?? .system
    rows.append(
      PaletteRow(
        section: "Preferences", title: "Colour scheme: \(names[p.colorScheme] ?? "")",
        detail: "next: \(names[next] ?? "")"
      ) { [weak workspace] in
        var q = LaceApp.shared.prefs
        q.colorScheme = next
        save(q, "Colour scheme: \(names[next] ?? "")", workspace)
      })
    return rows
  }

  private static func save(_ p: Prefs, _ said: String, _ workspace: WorkspaceViewController?) {
    do {
      try LaceApp.shared.savePrefs(p)
      LaceApp.shared.preferencesChanged()
      workspace?.current?.content.showNotice(said)
    } catch {
      Alerts.error("Preferences not saved", "\(error)", window: workspace?.view.window)
    }
  }
}

extension SavedConnectionStore {
  /// The names of the folders down to path ([] for the top level)
  func folderNames(of path: [Int]) -> [String] {
    (0..<path.count).compactMap { n in folders.first { $0.path == Array(path.prefix(n + 1)) }?.name }
  }
}
