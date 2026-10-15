//
// Lace macOS frontend
// The table structure sheet: one row per column of the schema
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class StructureViewController: NSViewController, NSTableViewDataSource, NSTableViewDelegate {
  private let table: String
  private let schema: Schema
  private let tableView = NSTableView()

  init(table: String, schema: Schema) {
    self.table = table
    self.schema = schema
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 720, height: 420))

    let title = NSTextField(labelWithString: "\(table) · \(Format.plural(schema.columns.count, "column"))")
    title.font = .boldSystemFont(ofSize: 13)

    for (id, name, width) in [
      ("name", "Column", 180), ("type", "Type", 140), ("null", "Null", 50), ("key", "Key", 50),
      ("default", "Default", 140), ("extra", "Extra", 160),
    ] as [(String, String, CGFloat)] {
      let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
      c.title = name
      c.width = width
      c.minWidth = 40
      tableView.addTableColumn(c)
    }
    tableView.dataSource = self
    tableView.delegate = self
    tableView.usesAlternatingRowBackgroundColors = true
    tableView.allowsColumnReordering = false
    tableView.rowHeight = 20
    tableView.style = .plain
    let scroll = NSScrollView()
    scroll.documentView = tableView
    scroll.hasVerticalScroller = true
    scroll.borderType = .bezelBorder
    scroll.translatesAutoresizingMaskIntoConstraints = false

    let close = NSButton(title: "Close", target: self, action: #selector(closeTapped))
    close.bezelStyle = .rounded
    close.keyEquivalent = "\r"

    let buttons = NSStackView(views: [close])
    buttons.orientation = .horizontal
    buttons.alignment = .centerY

    let stack = NSStackView(views: [title, scroll, buttons])
    stack.orientation = .vertical
    stack.alignment = .trailing
    stack.spacing = 12
    stack.translatesAutoresizingMaskIntoConstraints = false
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 20),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -20),
      stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 20),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -20),
      title.leadingAnchor.constraint(equalTo: stack.leadingAnchor),
      scroll.leadingAnchor.constraint(equalTo: stack.leadingAnchor),
      scroll.trailingAnchor.constraint(equalTo: stack.trailingAnchor),
    ])
    view = root
  }

  override func cancelOperation(_ sender: Any?) {
    dismiss(self)
  }

  @objc private func closeTapped() {
    dismiss(self)
  }

  func numberOfRows(in tableView: NSTableView) -> Int { schema.columns.count }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    guard let tableColumn else { return nil }
    let c = schema.columns[row]
    let text: String
    switch tableColumn.identifier.rawValue {
    case "name": text = c.name
    case "type": text = c.typeName ?? c.type.name
    case "null": text = c.nullable ? "yes" : "no"
    case "key": text = c.primaryKey ? "PK" : ""
    case "default": text = c.defaultValue ?? ""
    default:
      var extra: [String] = []
      if c.autoIncrement { extra.append("auto-increment") }
      if c.generated { extra.append("generated") }
      if let fk = c.foreignKey { extra.append("→ \(fk)") }
      if c.maxLength > 0 { extra.append("max \(c.maxLength)") }
      text = extra.joined(separator: ", ")
    }
    let cell = TextCellView.make(in: tableView, text: text)
    if tableColumn.identifier.rawValue == "type" || tableColumn.identifier.rawValue == "default" {
      cell.label.font = .monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
    } else {
      cell.label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    }
    return cell
  }
}
