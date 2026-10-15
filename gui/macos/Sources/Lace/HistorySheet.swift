//
// Lace macOS frontend
// Query history: a sheet listing the connection's history (newest first),
// searchable; Use puts a statement into the query editor, Copy, Remove and
// Clear do what they say
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class HistoryViewController: NSViewController, NSTableViewDataSource, NSTableViewDelegate,
  NSSearchFieldDelegate
{
  let history: History
  let connectionName: String
  /// A statement the user picked (into the query editor)
  var onUse: ((String) -> Void)?

  private let search = NSSearchField()
  private let tableView = NSTableView()
  private let emptyLabel = NSTextField(labelWithString: "No history")
  private let countLabel = NSTextField(labelWithString: "")
  private let useButton = NSButton()
  private let copyButton = NSButton()
  private let removeButton = NSButton()
  private let clearButton = NSButton()
  /// Shown rows: (index in the history, entry), newest first
  private var rows: [(index: Int, entry: History.Entry)] = []
  private let dateFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateStyle = .short
    f.timeStyle = .short
    f.doesRelativeDateFormatting = true
    return f
  }()

  init(history: History, connectionName: String) {
    self.history = history
    self.connectionName = connectionName
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 720, height: 480))

    let title = NSTextField(labelWithString: "Query history · \(connectionName)")
    title.font = .systemFont(ofSize: 15, weight: .semibold)
    title.lineBreakMode = .byTruncatingTail
    let note: String
    if history.isPersistent {
      note = "Kept in history/\(history.connectionID ?? "").json, shared with the other Lace frontends."
    } else if history.mode == .off {
      note = "History is off (Preferences)."
    } else {
      note = "Kept for this session only: the connection is not a saved one."
    }
    let noteLabel = NSTextField(wrappingLabelWithString: note)
    noteLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    noteLabel.textColor = .secondaryLabelColor

    search.placeholderString = "Search statements"
    search.delegate = self
    search.sendsWholeSearchString = false

    for (id, title, width) in [("when", "When", 130), ("kind", "", 44), ("sql", "Statement", 480)] {
      let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
      col.title = title
      col.width = CGFloat(width)
      col.minWidth = 40
      col.resizingMask = id == "sql" ? .autoresizingMask : []
      tableView.addTableColumn(col)
    }
    tableView.dataSource = self
    tableView.delegate = self
    tableView.usesAlternatingRowBackgroundColors = true
    tableView.allowsMultipleSelection = true
    tableView.doubleAction = #selector(useTapped)
    tableView.target = self
    tableView.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
    let scroll = NSScrollView()
    scroll.documentView = tableView
    scroll.hasVerticalScroller = true
    scroll.borderType = .bezelBorder
    scroll.translatesAutoresizingMaskIntoConstraints = false
    scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
    emptyLabel.textColor = .tertiaryLabelColor
    emptyLabel.font = .systemFont(ofSize: 14)
    emptyLabel.translatesAutoresizingMaskIntoConstraints = false
    scroll.addSubview(emptyLabel)
    NSLayoutConstraint.activate([
      emptyLabel.centerXAnchor.constraint(equalTo: scroll.centerXAnchor),
      emptyLabel.centerYAnchor.constraint(equalTo: scroll.centerYAnchor),
    ])

    countLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    countLabel.textColor = .secondaryLabelColor
    for (button, label, action) in [
      (useButton, "Use", #selector(useTapped)), (copyButton, "Copy", #selector(copyTapped)),
      (removeButton, "Remove", #selector(removeTapped)), (clearButton, "Clear…", #selector(clearTapped)),
    ] {
      button.title = label
      button.bezelStyle = .rounded
      button.target = self
      button.action = action
    }
    useButton.keyEquivalent = "\r"
    let close = NSButton(title: "Close", target: self, action: #selector(closeTapped))
    close.bezelStyle = .rounded
    close.keyEquivalent = "\u{1b}"
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let buttons = NSStackView(views: [countLabel, clearButton, spacer, removeButton, copyButton, close, useButton])
    buttons.orientation = .horizontal
    buttons.spacing = 8

    let stack = NSStackView(views: [title, noteLabel, search, scroll, buttons])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 10
    stack.edgeInsets = NSEdgeInsets(top: 18, left: 20, bottom: 18, right: 20)
    stack.translatesAutoresizingMaskIntoConstraints = false
    for v in [title, noteLabel, search, scroll, buttons] {
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
    reload()
  }

  override func viewWillAppear() {
    super.viewWillAppear()
    history.onChanged = { [weak self] in self?.reload() }
    view.window?.makeFirstResponder(search)
  }

  override func viewWillDisappear() {
    super.viewWillDisappear()
    history.onChanged = nil
  }

  private func reload() {
    let needle = search.stringValue.trimmingCharacters(in: .whitespaces).lowercased()
    let all = history.entries
    rows = all.enumerated().reversed().filter { needle.isEmpty || $0.element.sql.lowercased().contains(needle) }
      .map { (index: $0.offset, entry: $0.element) }
    tableView.reloadData()
    emptyLabel.isHidden = !rows.isEmpty
    emptyLabel.stringValue = all.isEmpty ? "No history" : "No matching statements"
    countLabel.stringValue = all.isEmpty ? "" : Format.plural(all.count, "statement")
    clearButton.isEnabled = !all.isEmpty
    updateButtons()
  }

  private func updateButtons() {
    let n = tableView.selectedRowIndexes.count
    useButton.isEnabled = n == 1 && onUse != nil
    copyButton.isEnabled = n >= 1
    removeButton.isEnabled = n >= 1
  }

  private var selectedEntries: [(index: Int, entry: History.Entry)] {
    tableView.selectedRowIndexes.compactMap { $0 < rows.count ? rows[$0] : nil }
  }

  // MARK: Actions

  @objc private func useTapped() {
    guard let row = selectedEntries.first else { return }
    onUse?(row.entry.sql)
    dismiss(nil)
  }

  @objc private func copyTapped() {
    let text = selectedEntries.map(\.entry.sql).joined(separator: ";\n")
    guard !text.isEmpty else { return }
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(text, forType: .string)
  }

  @objc private func removeTapped() {
    // Highest index first, so the others keep their positions
    for index in selectedEntries.map(\.index).sorted(by: >) {
      history.remove(at: index)
    }
  }

  @objc private func clearTapped() {
    Task { [weak self] in
      guard let self else { return }
      let ok = await Alerts.confirm(
        "Clear history", "Remove every statement from the history of \(connectionName)?", action: "Clear",
        window: view.window)
      if ok {
        history.clear()
      }
    }
  }

  @objc private func closeTapped() {
    dismiss(nil)
  }

  // MARK: Table

  func numberOfRows(in tableView: NSTableView) -> Int { rows.count }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    guard row < rows.count, let id = tableColumn?.identifier.rawValue else { return nil }
    let entry = rows[row].entry
    let text: String
    switch id {
    case "when": text = dateFormatter.string(from: entry.date)
    case "kind": text = entry.kind.tag
    default: text = entry.sql.split(whereSeparator: \.isNewline).first.map(String.init) ?? entry.sql
    }
    let cell = TextCellView.make(in: tableView, text: text)
    cell.textField?.font = id == "sql" ? .monospacedSystemFont(ofSize: 11, weight: .regular) : .systemFont(ofSize: 11)
    cell.textField?.textColor = id == "sql" ? .labelColor : .secondaryLabelColor
    cell.toolTip = id == "sql" ? entry.sql : entry.kind.name
    return cell
  }

  func tableViewSelectionDidChange(_ notification: Notification) {
    updateButtons()
  }

  func controlTextDidChange(_ obj: Notification) {
    reload()
  }
}
