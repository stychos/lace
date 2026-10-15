//
// Lace macOS frontend
// Add row: a sheet with one field per column, the TUI's placeholder rules
//
// Auto-increment and generated columns are never part of the INSERT. A
// column with a default, or a required one (NOT NULL, no default), is left
// out while its field is empty, so the database fills it in or reports the
// missing value; a nullable column without a default starts as NULL. The
// NULL and Empty toggles write those explicitly, overriding a default.
// Typed text is converted by the column's type (Value.fromEditText).
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class AddRowViewController: NSViewController, NSTextFieldDelegate {
  let connection: Connection
  let table: String
  let schema: Schema
  /// The row went in; the SQL liblace sent (for history)
  var onInserted: ((String?) -> Void)?

  @MainActor private final class Field {
    let column: Column
    let text = NSTextField()
    let nullButton = NSButton()
    let emptyButton = NSButton()
    /// Auto-increment or generated: never in the INSERT
    let omitted: Bool

    init(column: Column) {
      self.column = column
      omitted = column.autoIncrement || column.generated
    }

    var isNull: Bool { nullButton.state == .on }
    var isEmpty: Bool { emptyButton.state == .on }
  }

  private var fields: [Field] = []
  private let banner = BannerView(frame: .zero)
  private let insertButton = NSButton()
  private let spinner = NSProgressIndicator()
  private var inserting = false

  init(connection: Connection, table: String, schema: Schema) {
    self.connection = connection
    self.table = table
    self.schema = schema
    super.init(nibName: nil, bundle: nil)
    fields = schema.columns.map(Field.init)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let rows = fields.count
    let height = min(640, 150 + rows * 36)
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 600, height: height))

    let title = NSTextField(labelWithString: "Add row to \(table)")
    title.font = .systemFont(ofSize: 15, weight: .semibold)
    let description = NSTextField(
      wrappingLabelWithString:
        "Empty fields are left to the database. NULL and Empty write those explicitly.")
    description.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    description.textColor = .secondaryLabelColor
    description.preferredMaxLayoutWidth = 560

    let grid = NSGridView(numberOfColumns: 3, rows: 0)
    grid.rowSpacing = 6
    grid.columnSpacing = 10
    grid.column(at: 0).xPlacement = .trailing
    grid.column(at: 0).width = 170
    grid.column(at: 1).xPlacement = .fill
    grid.column(at: 2).xPlacement = .leading
    for field in fields {
      let c = field.column
      let name = NSTextField(labelWithString: c.name)
      name.font = .systemFont(ofSize: 12, weight: .medium)
      name.lineBreakMode = .byTruncatingMiddle
      name.toolTip = c.name
      var meta = c.typeName ?? c.type.name
      if c.primaryKey {
        meta += " · key"
      }
      if c.generated {
        meta += " · generated"
      } else if c.autoIncrement {
        meta += " · auto"
      } else if let d = c.defaultValue, !d.isEmpty {
        meta += " · default \(d)"
      } else if !c.nullable {
        meta += " · required"
      }
      let metaLabel = NSTextField(labelWithString: meta)
      metaLabel.font = .systemFont(ofSize: 10)
      metaLabel.textColor = .secondaryLabelColor
      metaLabel.lineBreakMode = .byTruncatingTail
      metaLabel.toolTip = meta
      let label = NSStackView(views: [name, metaLabel])
      label.orientation = .vertical
      label.alignment = .trailing
      label.spacing = 1

      field.text.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
      field.text.placeholderString = placeholder(for: c)
      field.text.delegate = self
      field.text.isEnabled = !field.omitted
      field.text.setContentHuggingPriority(.defaultLow, for: .horizontal)
      field.text.widthAnchor.constraint(greaterThanOrEqualToConstant: 220).isActive = true

      for (button, label, tip) in [
        (field.nullButton, "NULL", "Write NULL"), (field.emptyButton, "Empty", "Write an empty string"),
      ] {
        button.title = label
        button.setButtonType(.pushOnPushOff)
        button.bezelStyle = .recessed
        button.showsBorderOnlyWhileMouseInside = true
        button.controlSize = .regular
        button.font = .systemFont(ofSize: 11, weight: .medium)
        button.toolTip = tip
        button.target = self
        button.action = #selector(toggled(_:))
        button.isEnabled = !field.omitted
      }
      let toggles = NSStackView(views: [field.nullButton, field.emptyButton])
      toggles.orientation = .horizontal
      toggles.spacing = 4
      grid.addRow(with: [label, field.text, toggles])

      if !field.omitted, c.nullable, c.defaultValue?.isEmpty ?? true {
        field.nullButton.state = .on
        field.text.isEditable = false
      }
    }

    let flipped = FlippedView()
    flipped.translatesAutoresizingMaskIntoConstraints = false
    grid.translatesAutoresizingMaskIntoConstraints = false
    flipped.addSubview(grid)
    NSLayoutConstraint.activate([
      grid.topAnchor.constraint(equalTo: flipped.topAnchor, constant: 4),
      grid.leadingAnchor.constraint(equalTo: flipped.leadingAnchor),
      grid.trailingAnchor.constraint(equalTo: flipped.trailingAnchor),
      grid.bottomAnchor.constraint(equalTo: flipped.bottomAnchor, constant: -4),
    ])
    let scroll = NSScrollView()
    scroll.documentView = flipped
    scroll.hasVerticalScroller = true
    scroll.drawsBackground = false
    scroll.translatesAutoresizingMaskIntoConstraints = false
    NSLayoutConstraint.activate([
      flipped.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor)
    ])

    let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelTapped))
    cancel.bezelStyle = .rounded
    cancel.keyEquivalent = "\u{1b}"
    insertButton.title = "Insert"
    insertButton.bezelStyle = .rounded
    insertButton.keyEquivalent = "\r"
    insertButton.target = self
    insertButton.action = #selector(insertTapped)
    spinner.style = .spinning
    spinner.controlSize = .small
    spinner.isDisplayedWhenStopped = false
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let buttons = NSStackView(views: [spinner, spacer, cancel, insertButton])
    buttons.orientation = .horizontal
    buttons.spacing = 8

    let stack = NSStackView(views: [title, description, banner, scroll, buttons])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 10
    stack.edgeInsets = NSEdgeInsets(top: 18, left: 20, bottom: 18, right: 20)
    stack.translatesAutoresizingMaskIntoConstraints = false
    for v in [banner, scroll, buttons] {
      v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40).isActive = true
    }
    scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    view = root
  }

  override func viewDidAppear() {
    super.viewDidAppear()
    if let first = fields.first(where: { !$0.omitted && $0.text.isEditable }) {
      view.window?.makeFirstResponder(first.text)
    }
  }

  private func placeholder(for c: Column) -> String {
    if c.generated { return "generated" }
    if c.autoIncrement { return "auto" }
    if let d = c.defaultValue, !d.isEmpty { return "default: \(d)" }
    if !c.nullable { return "required" }
    return ""
  }

  // MARK: Field state

  @objc private func toggled(_ sender: NSButton) {
    guard let field = fields.first(where: { $0.nullButton === sender || $0.emptyButton === sender }) else { return }
    let other = sender === field.nullButton ? field.emptyButton : field.nullButton
    if sender.state == .on {
      other.state = .off
      field.text.stringValue = ""
    }
    field.text.isEditable = sender.state == .off
    if sender.state == .off {
      view.window?.makeFirstResponder(field.text)
    }
  }

  func controlTextDidChange(_ obj: Notification) {
    guard let tf = obj.object as? NSTextField, let field = fields.first(where: { $0.text === tf }) else { return }
    if !tf.stringValue.isEmpty {
      field.nullButton.state = .off
      field.emptyButton.state = .off
    }
  }

  /// Set a field from a script: nil text is NULL, "" is Empty
  func set(column name: String, text: String?) -> Bool {
    guard let field = fields.first(where: { $0.column.name == name }), !field.omitted else { return false }
    field.nullButton.state = text == nil ? .on : .off
    field.emptyButton.state = text == "" ? .on : .off
    field.text.stringValue = text ?? ""
    field.text.isEditable = text != nil && text != ""
    return true
  }

  private struct InputError: Error {
    let message: String
  }

  /// The INSERT's columns and values by the rules above
  private func collect() -> Result<(columns: [String], values: [Value]), InputError> {
    var columns: [String] = []
    var values: [Value] = []
    for field in fields where !field.omitted {
      let c = field.column
      if field.isNull {
        columns.append(c.name)
        values.append(.null)
      } else if field.isEmpty {
        columns.append(c.name)
        values.append(.text(""))
      } else {
        let text = field.text.stringValue
        guard !text.isEmpty else { continue }
        do {
          values.append(try Value.fromEditText(text, like: c.type))
          columns.append(c.name)
        } catch {
          return .failure(InputError(message: "\(c.name): \(error)"))
        }
      }
    }
    return .success((columns, values))
  }

  // MARK: Insert

  @objc private func insertTapped() {
    Task { _ = await insert() }
  }

  @objc private func cancelTapped() {
    dismiss(nil)
  }

  /// Run the INSERT; true when the row went in (the sheet closes then)
  @discardableResult
  func insert() async -> Bool {
    guard !inserting else { return false }
    let (columns, values): ([String], [Value])
    switch collect() {
    case .success(let cv): (columns, values) = cv
    case .failure(let why):
      banner.show(why.message)
      return false
    }
    banner.hide()
    inserting = true
    insertButton.isEnabled = false
    spinner.startAnimation(nil)
    defer {
      inserting = false
      insertButton.isEnabled = true
      spinner.stopAnimation(nil)
    }
    do {
      let sql = try await connection.insert(into: table, columns: columns, values: values)
      onInserted?(sql)
      if presentingViewController != nil {
        dismiss(nil)
      }
      return true
    } catch {
      banner.show("\(error)")
      return false
    }
  }
}

/// A document view whose origin is the top, so a short form sits at the top
/// of its scroll view
final class FlippedView: NSView {
  override var isFlipped: Bool { true }
}
