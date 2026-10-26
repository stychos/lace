//
// Lace macOS frontend
// FormSheetController: the structure editors' sheet, one row per field
// (text, a checkbox, a pick from a list with or without free text, a list
// of names picked one at a time in order). The owner builds the fields,
// reads them back by id on submit, and closes the sheet itself once the
// change ran (an error from the server shows in the sheet's banner).
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit

@MainActor
final class FormSheetController: NSViewController, NSTextFieldDelegate {
  enum Kind {
    case text
    case toggle
    /// Options to pick from; typing allowed when freeText
    case pick(freeText: Bool)
    /// Comma-separated names, each picked from the options (appended in order) or typed
    case list
  }

  @MainActor final class Field {
    let id: String
    let label: String
    let kind: Kind
    var options: [String] = []
    var disabledReason: String?
    var hint: String?
    let text = NSTextField()
    let combo = NSComboBox()
    let check = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    let adder = NSPopUpButton(frame: .zero, pullsDown: true)

    init(id: String, label: String, kind: Kind) {
      self.id = id
      self.label = label
      self.kind = kind
    }

    var isEnabled: Bool { disabledReason == nil }
  }

  let formTitle: String
  private let subtitle: String?
  private let submitTitle: String
  private(set) var fields: [Field] = []
  private let banner = BannerView(frame: .zero)
  private let submitButton = NSButton()
  private let spinner = NSProgressIndicator()
  private var busy = false
  /// Validate, build the statement and run it; the sheet stays open
  var onSubmit: ((FormSheetController) -> Void)?

  init(title: String, subtitle: String? = nil, submitTitle: String = "Save") {
    formTitle = title
    self.subtitle = subtitle
    self.submitTitle = submitTitle
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  // MARK: Building

  @discardableResult
  private func add(_ f: Field) -> Field {
    fields.append(f)
    return f
  }

  func addText(id: String, label: String, value: String? = nil, hint: String? = nil) {
    let f = add(Field(id: id, label: label, kind: .text))
    f.text.stringValue = value ?? ""
    f.hint = hint
  }

  func addToggle(id: String, label: String, on: Bool) {
    let f = add(Field(id: id, label: label, kind: .toggle))
    f.check.state = on ? .on : .off
  }

  func addPick(id: String, label: String, value: String? = nil, options: [String] = [], freeText: Bool) {
    let f = add(Field(id: id, label: label, kind: .pick(freeText: freeText)))
    f.options = options
    f.combo.stringValue = value ?? ""
  }

  func addList(id: String, label: String, values: [String] = [], options: [String] = [], hint: String? = nil) {
    let f = add(Field(id: id, label: label, kind: .list))
    f.options = options
    f.text.stringValue = values.joined(separator: ", ")
    f.hint = hint ?? "comma-separated, in order"
  }

  func disable(id: String, reason: String) {
    field(id)?.disabledReason = reason
    if isViewLoaded, let f = field(id) {
      apply(f)
    }
  }

  /// The picker's names, once they are in (the field keeps its text meanwhile)
  func setOptions(id: String, _ names: [String]) {
    guard let f = field(id) else { return }
    f.options = names
    if isViewLoaded {
      fillOptions(f)
    }
  }

  func field(_ id: String) -> Field? { fields.first { $0.id == id } }

  // MARK: Values

  /// Trimmed; "" when empty or not a text field
  func text(_ id: String) -> String {
    guard let f = field(id) else { return "" }
    switch f.kind {
    case .pick: return f.combo.stringValue.trimmingCharacters(in: .whitespaces)
    case .toggle: return ""
    default: return f.text.stringValue.trimmingCharacters(in: .whitespaces)
    }
  }

  /// nil when empty
  func value(_ id: String) -> String? {
    let t = text(id)
    return t.isEmpty ? nil : t
  }

  func on(_ id: String) -> Bool { field(id)?.check.state == .on }

  /// A list field's names, trimmed, empty ones left out
  func names(_ id: String) -> [String] {
    text(id).split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
  }

  /// Set a field from a script ("on"/"off" for a toggle); false for an unknown id
  @discardableResult
  func set(_ id: String, _ value: String) -> Bool {
    guard let f = field(id) else { return false }
    loadViewIfNeeded()
    switch f.kind {
    case .toggle: f.check.state = value == "on" || value == "yes" || value == "true" ? .on : .off
    case .pick: f.combo.stringValue = value
    default: f.text.stringValue = value
    }
    return true
  }

  // MARK: View

  override func loadView() {
    let height = min(620, 170 + fields.count * 34)
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 560, height: height))
    let title = NSTextField(labelWithString: formTitle)
    title.font = .systemFont(ofSize: 15, weight: .semibold)
    var top: [NSView] = [title]
    if let subtitle {
      let sub = NSTextField(wrappingLabelWithString: subtitle)
      sub.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
      sub.textColor = .secondaryLabelColor
      sub.preferredMaxLayoutWidth = 520
      top.append(sub)
    }
    let grid = NSGridView(numberOfColumns: 2, rows: 0)
    grid.rowSpacing = 8
    grid.columnSpacing = 10
    grid.column(at: 0).xPlacement = .trailing
    grid.column(at: 0).width = 150
    grid.column(at: 1).xPlacement = .fill
    for f in fields {
      let label = NSTextField(labelWithString: f.label)
      label.font = .systemFont(ofSize: 12, weight: .medium)
      label.lineBreakMode = .byTruncatingTail
      grid.addRow(with: [label, control(for: f)])
      apply(f)
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
    flipped.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor).isActive = true

    let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelTapped))
    cancel.bezelStyle = .rounded
    cancel.keyEquivalent = "\u{1b}"
    submitButton.title = submitTitle
    submitButton.bezelStyle = .rounded
    submitButton.keyEquivalent = "\r"
    submitButton.target = self
    submitButton.action = #selector(submitTapped)
    spinner.style = .spinning
    spinner.controlSize = .small
    spinner.isDisplayedWhenStopped = false
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let buttons = NSStackView(views: [spinner, spacer, cancel, submitButton])
    buttons.orientation = .horizontal
    buttons.spacing = 8

    let stack = NSStackView(views: top + [banner, scroll, buttons])
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

  private func control(for f: Field) -> NSView {
    switch f.kind {
    case .text:
      f.text.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
      f.text.placeholderString = f.hint
      f.text.delegate = self
      return f.text
    case .toggle:
      f.check.title = ""
      return f.check
    case .pick(let freeText):
      f.combo.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
      f.combo.isEditable = freeText
      f.combo.completes = freeText
      f.combo.numberOfVisibleItems = 12
      fillOptions(f)
      return f.combo
    case .list:
      f.text.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
      f.text.placeholderString = f.hint
      f.text.delegate = self
      f.text.setContentHuggingPriority(.defaultLow, for: .horizontal)
      f.adder.controlSize = .small
      f.adder.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
      f.adder.target = self
      f.adder.action = #selector(adderPicked(_:))
      fillOptions(f)
      let row = NSStackView(views: [f.text, f.adder])
      row.orientation = .horizontal
      row.spacing = 6
      return row
    }
  }

  private func fillOptions(_ f: Field) {
    switch f.kind {
    case .pick:
      let current = f.combo.stringValue
      f.combo.removeAllItems()
      f.combo.addItems(withObjectValues: f.options)
      f.combo.stringValue = current
    case .list:
      f.adder.removeAllItems()
      f.adder.addItem(withTitle: "Add…")
      f.adder.addItems(withTitles: f.options)
      f.adder.isEnabled = !f.options.isEmpty && f.isEnabled
    default:
      break
    }
  }

  private func apply(_ f: Field) {
    let on = f.isEnabled && !busy
    f.text.isEnabled = on
    f.combo.isEnabled = on
    f.check.isEnabled = on
    f.adder.isEnabled = on && !f.options.isEmpty
    if let why = f.disabledReason {
      f.text.placeholderString = why
      f.combo.placeholderString = why
      f.text.toolTip = why
      f.combo.toolTip = why
      f.check.toolTip = why
    }
  }

  @objc private func adderPicked(_ sender: NSPopUpButton) {
    guard let f = fields.first(where: { $0.adder === sender }), sender.indexOfSelectedItem > 0 else { return }
    let name = sender.titleOfSelectedItem ?? ""
    let current = f.text.stringValue.trimmingCharacters(in: .whitespaces)
    f.text.stringValue = current.isEmpty ? name : "\(current), \(name)"
  }

  override func viewDidAppear() {
    super.viewDidAppear()
    if let first = fields.first(where: \.isEnabled) {
      switch first.kind {
      case .toggle: view.window?.makeFirstResponder(first.check)
      case .pick: view.window?.makeFirstResponder(first.combo)
      default: view.window?.makeFirstResponder(first.text)
      }
    }
  }

  // MARK: Submit, errors

  @objc private func submitTapped() {
    guard !busy else { return }
    banner.hide()
    onSubmit?(self)
  }

  @objc private func cancelTapped() {
    dismiss(nil)
  }

  func showError(_ message: String) {
    banner.show(message)
  }

  func setBusy(_ on: Bool) {
    busy = on
    submitButton.isEnabled = !on
    on ? spinner.startAnimation(nil) : spinner.stopAnimation(nil)
    for f in fields {
      apply(f)
    }
  }

  func close() {
    if presentingViewController != nil {
      dismiss(nil)
    }
  }
}
