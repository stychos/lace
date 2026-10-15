//
// Lace macOS frontend
// Preferences (⌘,): macos.json's settings in one window; every change is
// written at once. Daemon settings apply to the next launch; the rest to
// tabs opened from now on.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class PreferencesWindowController: NSWindowController, NSTextFieldDelegate {
  private let spawnPopup = NSPopUpButton()
  private let idleField = NSTextField()
  private let confirmDelete = NSButton(checkboxWithTitle: "Ask before deleting rows", target: nil, action: nil)
  private let approximate = NSButton(
    checkboxWithTitle: "Estimate the count of big server tables (shown as ≈)", target: nil, action: nil)
  private let pageSizeField = NSTextField()
  private let rowNumbersPopup = NSPopUpButton()
  private let historyPopup = NSPopUpButton()
  private let historySizeField = NSTextField()
  private let restoreSession = NSButton(checkboxWithTitle: "Reopen the last session at launch", target: nil, action: nil)
  private let keychain = NSButton(
    checkboxWithTitle: "Keep saved passwords in the Keychain, not in connections.json", target: nil, action: nil)
  private let note = NSTextField(labelWithString: "")

  init() {
    let window = NSWindow(
      contentRect: NSRect(x: 0, y: 0, width: 560, height: 420),
      styleMask: [.titled, .closable], backing: .buffered, defer: false)
    window.title = "Preferences"
    window.center()
    super.init(window: window)
    window.contentView = makeContent()
    load()
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  private func makeContent() -> NSView {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 560, height: 420))
    for title in ["Shared daemon (Unix socket)", "Private daemon (stdio)", "Shared daemon (TCP 7433)", "Detect: a running daemon, else private"] {
      spawnPopup.addItem(withTitle: title)
    }
    spawnPopup.target = self
    spawnPopup.action = #selector(changed)
    idleField.placeholderString = "0 = keep running"
    idleField.delegate = self
    idleField.widthAnchor.constraint(equalToConstant: 110).isActive = true
    let idleRow = NSStackView(views: [idleField, NSTextField(labelWithString: "seconds without clients before a shared daemon exits")])
    idleRow.orientation = .horizontal
    idleRow.spacing = 8
    (idleRow.views[1] as? NSTextField)?.textColor = .secondaryLabelColor
    (idleRow.views[1] as? NSTextField)?.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    for b in [confirmDelete, approximate, restoreSession, keychain] {
      b.target = self
      b.action = #selector(changed)
    }
    pageSizeField.delegate = self
    pageSizeField.widthAnchor.constraint(equalToConstant: 110).isActive = true
    let pageRow = NSStackView(views: [pageSizeField, NSTextField(labelWithString: "rows per page (100 to 100 000)")])
    pageRow.orientation = .horizontal
    pageRow.spacing = 8
    (pageRow.views[1] as? NSTextField)?.textColor = .secondaryLabelColor
    (pageRow.views[1] as? NSTextField)?.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    for title in ["Never", "Unless the key numbers the rows", "Always"] {
      rowNumbersPopup.addItem(withTitle: title)
    }
    rowNumbersPopup.target = self
    rowNumbersPopup.action = #selector(changed)
    for title in ["Off", "This session only", "Kept per saved connection"] {
      historyPopup.addItem(withTitle: title)
    }
    historyPopup.target = self
    historyPopup.action = #selector(changed)
    historySizeField.delegate = self
    historySizeField.widthAnchor.constraint(equalToConstant: 110).isActive = true
    let historyRow = NSStackView(views: [historySizeField, NSTextField(labelWithString: "statements kept (\(History.sizeRange.lowerBound) to \(History.sizeRange.upperBound))")])
    historyRow.orientation = .horizontal
    historyRow.spacing = 8
    (historyRow.views[1] as? NSTextField)?.textColor = .secondaryLabelColor
    (historyRow.views[1] as? NSTextField)?.font = .systemFont(ofSize: NSFont.smallSystemFontSize)

    let grid = NSGridView(numberOfColumns: 2, rows: 0)
    grid.rowSpacing = 10
    grid.columnSpacing = 12
    grid.column(at: 0).xPlacement = .trailing
    grid.column(at: 0).width = 120
    grid.rowAlignment = .firstBaseline
    func row(_ label: String, _ v: NSView) {
      let l = NSTextField(labelWithString: label)
      l.textColor = .secondaryLabelColor
      grid.addRow(with: [l, v])
    }
    func section(_ title: String) {
      let l = NSTextField(labelWithString: title)
      l.font = .systemFont(ofSize: 13, weight: .semibold)
      let r = grid.addRow(with: [NSView(), l])
      r.topPadding = 8
    }
    section("Daemon (next launch)")
    row("Start", spawnPopup)
    row("Idle timeout", idleRow)
    section("Tables")
    row("Page size", pageRow)
    row("Row numbers", rowNumbersPopup)
    row("Counts", approximate)
    row("Deleting", confirmDelete)
    section("History")
    row("Query history", historyPopup)
    row("Size", historyRow)
    section("Startup and passwords")
    row("Session", restoreSession)
    row("Passwords", keychain)
    grid.translatesAutoresizingMaskIntoConstraints = false

    note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    note.textColor = .secondaryLabelColor
    note.stringValue = "Written to \(Prefs.path.path). Table and history settings apply to tabs opened from now on."
    note.lineBreakMode = .byTruncatingMiddle

    let stack = NSStackView(views: [grid, note])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 16
    stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
    stack.translatesAutoresizingMaskIntoConstraints = false
    note.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40).isActive = true
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    return root
  }

  private func load() {
    let p = LaceApp.shared.prefs
    spawnPopup.selectItem(at: Int(p.daemonSpawnMode.rawValue))
    idleField.stringValue = String(p.daemonIdleTimeout)
    confirmDelete.state = p.confirmDelete ? .on : .off
    approximate.state = p.approximateCounts ? .on : .off
    pageSizeField.stringValue = String(p.pageSize)
    rowNumbersPopup.selectItem(at: p.rowNumbers.rawValue)
    historyPopup.selectItem(at: p.historyMode.rawValue)
    historySizeField.stringValue = String(p.historyMaxSize)
    restoreSession.state = p.restoreSession ? .on : .off
    keychain.state = p.keychainPasswords ? .on : .off
  }

  @objc private func changed() {
    save()
  }

  func controlTextDidEndEditing(_ obj: Notification) {
    save()
    load()
  }

  private func save() {
    var p = LaceApp.shared.prefs
    if let mode = LaceClient.SpawnMode(rawValue: Int32(spawnPopup.indexOfSelectedItem)) {
      p.daemonSpawnMode = mode
    }
    if let n = Int32(idleField.stringValue.trimmingCharacters(in: .whitespaces)), (0...86400).contains(n) {
      p.daemonIdleTimeout = n
    }
    p.confirmDelete = confirmDelete.state == .on
    p.approximateCounts = approximate.state == .on
    if let n = Int(pageSizeField.stringValue.trimmingCharacters(in: .whitespaces)), (100...100_000).contains(n) {
      p.pageSize = n
    }
    if let r = Prefs.RowNumbers(rawValue: rowNumbersPopup.indexOfSelectedItem) {
      p.rowNumbers = r
    }
    if let m = History.Mode(rawValue: historyPopup.indexOfSelectedItem) {
      p.historyMode = m
    }
    if let n = Int(historySizeField.stringValue.trimmingCharacters(in: .whitespaces)), History.sizeRange.contains(n) {
      p.historyMaxSize = n
    }
    p.restoreSession = restoreSession.state == .on
    p.keychainPasswords = keychain.state == .on
    LaceApp.shared.prefs = p
    do {
      try p.save()
    } catch {
      Alerts.error("Preferences not saved", "\(error)", window: window)
    }
  }
}
