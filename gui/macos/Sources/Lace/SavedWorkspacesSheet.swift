//
// Lace macOS frontend
// Saved workspaces: named window layouts in workspaces.json, shared with
// the TUIs (LaceKit's SavedWorkspaces over their saved_workspaces.c). File ▸
// Save Workspace As… stores the window's layout (Session.layoutJSON: its
// saved connections' tabs, nothing else); File ▸ Saved Workspaces opens one
// (Session.open(layout:)); Manage… is a sheet to open, rename and delete
// them. The palette lists them too.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
enum SavedWorkspaceActions {
  /// Ask for a name (the window's own first) and store its layout,
  /// replacing one of that name after a question
  static func save(_ wc: WorkspaceWindowController) {
    let window = wc.window
    guard let (layout, leftOut) = Session.layoutJSON(wc) else {
      Alerts.error(
        "Nothing to save", "Only saved connections can be opened again, and this window has none.", window: window)
      return
    }
    Task {
      var typed = ScriptDriver.takePromptAnswer()
      if typed == nil {
        typed = await Alerts.prompt("Save Workspace As", "Name", initial: wc.name, action: "Save", window: window)
      }
      guard let name = typed?.trimmingCharacters(in: .whitespacesAndNewlines), !name.isEmpty else { return }
      if ((try? SavedWorkspaces.list()) ?? []).contains(where: { $0.name == name }) {
        guard
          await Alerts.confirm(
            "Replace \(name)?", "A saved workspace of that name exists already.", action: "Replace", window: window)
        else { return }
      }
      do {
        try SavedWorkspaces.put(name: name, workspace: layout)
        let note =
          leftOut > 0
          ? "Saved as \(name) (\(Format.plural(leftOut, "connection")) without a saved entry left out)"
          : "Saved as \(name)"
        wc.workspace.current?.content.showNotice(note)
        Alerts.logger?("Saved workspace", note)
      } catch {
        Alerts.error("Workspace not saved", "\(error)", window: window)
      }
    }
  }

  static func open(name: String) {
    guard let layout = ((try? SavedWorkspaces.list()) ?? []).first(where: { $0.name == name }) else {
      Alerts.error("No saved workspace", "There is no saved workspace named \(name).", window: NSApp.keyWindow)
      return
    }
    Task { await Session.open(layout: layout) }
  }

  /// After a question
  static func delete(name: String, window: NSWindow?, done: (() -> Void)? = nil) {
    Task {
      guard
        await Alerts.confirm(
          "Delete \(name)?", "The saved workspace goes from workspaces.json.", action: "Delete", window: window)
      else { return }
      do {
        try SavedWorkspaces.remove(name: name)
      } catch {
        Alerts.error("Not deleted", "\(error)", window: window)
      }
      done?()
    }
  }

  static func rename(name: String, window: NSWindow?, done: (() -> Void)? = nil) {
    Task {
      var typed = ScriptDriver.takePromptAnswer()
      if typed == nil {
        typed = await Alerts.prompt("Rename Saved Workspace", "", initial: name, action: "Rename", window: window)
      }
      guard let new = typed?.trimmingCharacters(in: .whitespacesAndNewlines), !new.isEmpty, new != name else { return }
      do {
        try SavedWorkspaces.rename(name, to: new)
      } catch {
        Alerts.error("Not renamed", "\(error)", window: window)
      }
      done?()
    }
  }
}

/// Manage… : the saved layouts with when they were saved and their
/// connections; Open (or a double click), Rename…, Delete…, Done
final class SavedWorkspacesViewController: NSViewController, NSTableViewDataSource, NSTableViewDelegate {
  private let tableView = NSTableView()
  private let emptyLabel = NSTextField(labelWithString: "No saved workspaces")
  private let message = NSTextField(wrappingLabelWithString: "")
  private let openButton = NSButton()
  private let renameButton = NSButton()
  private let deleteButton = NSButton()
  private var layouts: [SavedWorkspaces.Layout] = []
  private let dateFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateStyle = .medium
    f.timeStyle = .short
    f.doesRelativeDateFormatting = true
    return f
  }()

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 620, height: 380))
    let title = NSTextField(labelWithString: "Saved workspaces")
    title.font = .systemFont(ofSize: 15, weight: .semibold)
    let note = NSTextField(
      wrappingLabelWithString: "Kept in workspaces.json, shared with the other Lace frontends. A layout opens in a new window.")
    note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    note.textColor = .secondaryLabelColor
    for (id, name, width) in [("name", "Name", 200), ("saved", "Saved", 150), ("connections", "Connections", 230)] {
      let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
      col.title = name
      col.width = CGFloat(width)
      col.resizingMask = id == "connections" ? .autoresizingMask : .userResizingMask
      tableView.addTableColumn(col)
    }
    tableView.dataSource = self
    tableView.delegate = self
    tableView.usesAlternatingRowBackgroundColors = true
    tableView.target = self
    tableView.doubleAction = #selector(openSelected(_:))
    let scroll = NSScrollView()
    scroll.documentView = tableView
    scroll.hasVerticalScroller = true
    scroll.borderType = .bezelBorder
    scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
    emptyLabel.textColor = .tertiaryLabelColor
    emptyLabel.translatesAutoresizingMaskIntoConstraints = false
    scroll.addSubview(emptyLabel)
    message.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    message.textColor = .systemRed
    message.isHidden = true

    func button(_ b: NSButton, _ title: String, _ action: Selector) {
      b.title = title
      b.bezelStyle = .rounded
      b.target = self
      b.action = action
    }
    button(openButton, "Open", #selector(openSelected(_:)))
    button(renameButton, "Rename…", #selector(renameSelected(_:)))
    button(deleteButton, "Delete…", #selector(deleteSelected(_:)))
    let done = NSButton(title: "Done", target: self, action: #selector(done(_:)))
    done.keyEquivalent = "\u{1b}"
    openButton.keyEquivalent = "\r"
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let buttons = NSStackView(views: [renameButton, deleteButton, spacer, done, openButton])
    buttons.orientation = .horizontal

    let stack = NSStackView(views: [title, note, scroll, message, buttons])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 10
    stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
    stack.translatesAutoresizingMaskIntoConstraints = false
    for v in [title, note, scroll, message, buttons] {
      v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40).isActive = true
    }
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
      emptyLabel.centerXAnchor.constraint(equalTo: scroll.centerXAnchor),
      emptyLabel.centerYAnchor.constraint(equalTo: scroll.centerYAnchor),
    ])
    view = root
    reload()
  }

  /// workspaces.json read again (another frontend may have changed it)
  func reload() {
    let selected = selectedName
    do {
      layouts = try SavedWorkspaces.list()
      message.isHidden = true
    } catch {
      layouts = []
      message.stringValue = "\(error)"
      message.isHidden = false
    }
    tableView.reloadData()
    if let selected, let i = layouts.firstIndex(where: { $0.name == selected }) {
      tableView.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false)
    } else if !layouts.isEmpty {
      tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
    }
    emptyLabel.isHidden = !layouts.isEmpty
    updateButtons()
  }

  /// The names listed (the script driver)
  var names: [String] { layouts.map(\.name) }

  private var selectedName: String? {
    let i = tableView.selectedRow
    return i >= 0 && i < layouts.count ? layouts[i].name : nil
  }

  private func updateButtons() {
    let on = selectedName != nil
    openButton.isEnabled = on
    renameButton.isEnabled = on
    deleteButton.isEnabled = on
  }

  /// Select the layout named name (the script driver); false if none
  func select(name: String) -> Bool {
    guard let i = layouts.firstIndex(where: { $0.name == name }) else { return false }
    tableView.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false)
    return true
  }

  @objc func openSelected(_ sender: Any?) {
    guard let name = selectedName else { return }
    dismiss(nil)
    SavedWorkspaceActions.open(name: name)
  }

  @objc func renameSelected(_ sender: Any?) {
    guard let name = selectedName else { return }
    SavedWorkspaceActions.rename(name: name, window: view.window) { [weak self] in self?.reload() }
  }

  @objc func deleteSelected(_ sender: Any?) {
    guard let name = selectedName else { return }
    SavedWorkspaceActions.delete(name: name, window: view.window) { [weak self] in self?.reload() }
  }

  @objc private func done(_ sender: Any?) {
    dismiss(nil)
  }

  func numberOfRows(in tableView: NSTableView) -> Int { layouts.count }

  func tableViewSelectionDidChange(_ notification: Notification) {
    updateButtons()
  }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    let id = NSUserInterfaceItemIdentifier("cell")
    let cell = tableView.makeView(withIdentifier: id, owner: nil) as? TextCellView ?? TextCellView(frame: .zero)
    cell.identifier = id
    let l = layouts[row]
    switch tableColumn?.identifier.rawValue {
    case "name":
      cell.label.stringValue = l.name
    case "saved":
      cell.label.stringValue = l.saved.timeIntervalSince1970 > 0 ? dateFormatter.string(from: l.saved) : ""
    default:
      cell.label.stringValue = l.connections.map { c in
        let tabs = (c["tabs"] as? [Any])?.count ?? 0
        return "\(c["name"] as? String ?? "connection") (\(tabs))"
      }.joined(separator: ", ")
    }
    return cell
  }
}
