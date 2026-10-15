//
// Lace macOS frontend
// FilterBarView: a text search over the text columns plus column filters
// shown as chips; FilterEditorController edits one filter in a popover
//
// The SQL comes from liblace (SQL.whereClause): the bar only holds Filter
// values, so the GTK frontend and this one build the same WHERE.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class FilterBarView: NSView {
  var schema: Schema?
  private(set) var filters: [Filter] = []
  var onChanged: (() -> Void)?

  private let searchField = NSSearchField()
  private let chips = NSStackView()
  private let addButton = NSButton()
  private var editor: FilterEditorController?

  override init(frame: NSRect) {
    super.init(frame: frame)
    searchField.placeholderString = "Search text columns"
    searchField.sendsWholeSearchString = true
    searchField.sendsSearchStringImmediately = false
    searchField.target = self
    searchField.action = #selector(searchChanged)
    searchField.controlSize = .small
    searchField.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    searchField.translatesAutoresizingMaskIntoConstraints = false
    searchField.widthAnchor.constraint(equalToConstant: 220).isActive = true

    chips.orientation = .horizontal
    chips.spacing = 4
    chips.alignment = .centerY

    addButton.title = "Filter"
    addButton.image = NSImage(systemSymbolName: "plus", accessibilityDescription: nil)
    addButton.imagePosition = .imageLeading
    addButton.bezelStyle = .roundRect
    addButton.controlSize = .small
    addButton.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    addButton.target = self
    addButton.action = #selector(addTapped)
    addButton.toolTip = "Add a column filter"

    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let row = NSStackView(views: [searchField, chips, addButton, spacer])
    row.orientation = .horizontal
    row.spacing = 8
    row.alignment = .centerY
    row.edgeInsets = NSEdgeInsets(top: 5, left: 8, bottom: 5, right: 8)
    row.translatesAutoresizingMaskIntoConstraints = false
    addSubview(row)
    NSLayoutConstraint.activate([
      row.leadingAnchor.constraint(equalTo: leadingAnchor),
      row.trailingAnchor.constraint(equalTo: trailingAnchor),
      row.topAnchor.constraint(equalTo: topAnchor),
      row.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -1),
    ])
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func draw(_ dirtyRect: NSRect) {
    NSColor.separatorColor.setFill()
    NSRect(x: 0, y: 0, width: bounds.width, height: 1).fill()
  }

  /// The search text, nil when empty
  var search: String? {
    let t = searchField.stringValue.trimmingCharacters(in: .whitespaces)
    return t.isEmpty ? nil : t
  }

  var isEmpty: Bool { filters.isEmpty && search == nil }

  /// Restore without firing onChanged
  func setFilters(_ filters: [Filter], search: String?) {
    self.filters = filters
    searchField.stringValue = search ?? ""
    rebuildChips()
  }

  func add(_ filter: Filter) {
    filters.append(filter)
    rebuildChips()
    onChanged?()
  }

  /// Open the editor for a new filter, on a column when given
  func openAdd(column: String?) {
    let filter = Filter(column: column ?? schema?.columns.first?.name, op: .eq, value: "")
    present(filter: filter, index: nil, relativeTo: addButton)
  }

  func focusSearch() {
    window?.makeFirstResponder(searchField)
  }

  var hasKeyboardFocus: Bool {
    guard let responder = window?.firstResponder as? NSView else { return false }
    return responder.isDescendant(of: self)
  }

  @objc private func searchChanged() {
    onChanged?()
  }

  @objc private func addTapped() {
    openAdd(column: nil)
  }

  @objc private func chipTapped(_ sender: NSButton) {
    guard sender.tag >= 0, sender.tag < filters.count else { return }
    present(filter: filters[sender.tag], index: sender.tag, relativeTo: sender)
  }

  private func chipTitle(_ f: Filter) -> String {
    if f.op == .raw || f.column == nil {
      let v = f.value.trimmingCharacters(in: .whitespacesAndNewlines)
      return "SQL: " + (v.count > 30 ? String(v.prefix(30)) + "…" : v)
    }
    var t = "\(f.column ?? "") \(f.op.label)"
    if f.op.needsValue {
      t += " \(f.value)"
      if f.op.takesTwoValues, let v2 = f.value2 {
        t += " and \(v2)"
      }
    }
    return t
  }

  private func rebuildChips() {
    for v in chips.arrangedSubviews {
      chips.removeArrangedSubview(v)
      v.removeFromSuperview()
    }
    for (i, f) in filters.enumerated() {
      let chip = NSButton(title: chipTitle(f), target: self, action: #selector(chipTapped(_:)))
      chip.tag = i
      chip.bezelStyle = .roundRect
      chip.controlSize = .small
      chip.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
      chip.toolTip = "Edit or remove this filter"
      chips.addArrangedSubview(chip)
    }
  }

  private func present(filter: Filter, index: Int?, relativeTo view: NSView) {
    let ed = FilterEditorController(schema: schema, filter: filter, canRemove: index != nil)
    ed.onApply = { [weak self] f in
      guard let self else { return }
      if let index, index < filters.count {
        filters[index] = f
      } else {
        filters.append(f)
      }
      rebuildChips()
      onChanged?()
    }
    ed.onRemove = { [weak self] in
      guard let self, let index, index < filters.count else { return }
      filters.remove(at: index)
      rebuildChips()
      onChanged?()
    }
    editor = ed
    ed.show(relativeTo: view.bounds, of: view)
  }
}

/// One filter: column (or raw SQL), operator, value(s)
final class FilterEditorController: NSViewController, NSPopoverDelegate {
  var onApply: ((Filter) -> Void)?
  var onRemove: (() -> Void)?

  private let popover = NSPopover()
  private let columns: [String]
  private var filter: Filter
  private let canRemove: Bool
  private let columnPopup = NSPopUpButton()
  private let opPopup = NSPopUpButton()
  private let valueField = NSTextField()
  private let andLabel = NSTextField(labelWithString: "and")
  private let value2Field = NSTextField()
  private static let rawTitle = "Raw SQL condition"
  private static let ops: [FilterOp] = FilterOp.allCases.filter { $0 != .raw }

  init(schema: Schema?, filter: Filter, canRemove: Bool) {
    columns = schema?.columns.map(\.name) ?? []
    self.filter = filter
    self.canRemove = canRemove
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 380, height: 140))

    columnPopup.addItems(withTitles: columns)
    columnPopup.menu?.addItem(.separator())
    columnPopup.addItem(withTitle: FilterEditorController.rawTitle)
    columnPopup.target = self
    columnPopup.action = #selector(columnChanged)
    if filter.op == .raw || filter.column == nil {
      columnPopup.selectItem(withTitle: FilterEditorController.rawTitle)
    } else if let c = filter.column, columns.contains(c) {
      columnPopup.selectItem(withTitle: c)
    }

    opPopup.addItems(withTitles: FilterEditorController.ops.map(\.label))
    opPopup.target = self
    opPopup.action = #selector(opChanged)
    if let i = FilterEditorController.ops.firstIndex(of: filter.op) {
      opPopup.selectItem(at: i)
    }

    valueField.stringValue = filter.value
    valueField.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
    valueField.translatesAutoresizingMaskIntoConstraints = false
    value2Field.stringValue = filter.value2 ?? ""
    value2Field.font = valueField.font
    value2Field.translatesAutoresizingMaskIntoConstraints = false
    andLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)

    let selectors = NSStackView(views: [columnPopup, opPopup])
    selectors.orientation = .horizontal
    selectors.spacing = 6

    let values = NSStackView(views: [valueField, andLabel, value2Field])
    values.orientation = .horizontal
    values.spacing = 6
    values.translatesAutoresizingMaskIntoConstraints = false

    let remove = NSButton(title: "Remove", target: self, action: #selector(removeTapped))
    remove.bezelStyle = .rounded
    remove.controlSize = .small
    remove.isHidden = !canRemove
    let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelTapped))
    cancel.bezelStyle = .rounded
    cancel.controlSize = .small
    let apply = NSButton(title: "Apply", target: self, action: #selector(applyTapped))
    apply.bezelStyle = .rounded
    apply.controlSize = .small
    apply.keyEquivalent = "\r"
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let buttons = NSStackView(views: [remove, spacer, cancel, apply])
    buttons.orientation = .horizontal
    buttons.spacing = 6

    let stack = NSStackView(views: [selectors, values, buttons])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 10
    stack.translatesAutoresizingMaskIntoConstraints = false
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 12),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -12),
      stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 12),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -12),
      values.leadingAnchor.constraint(equalTo: stack.leadingAnchor),
      values.trailingAnchor.constraint(equalTo: stack.trailingAnchor),
      buttons.leadingAnchor.constraint(equalTo: stack.leadingAnchor),
      buttons.trailingAnchor.constraint(equalTo: stack.trailingAnchor),
      valueField.widthAnchor.constraint(greaterThanOrEqualToConstant: 120),
    ])
    view = root
    updateForSelection()
  }

  private var isRaw: Bool { columnPopup.titleOfSelectedItem == FilterEditorController.rawTitle }

  private var selectedOp: FilterOp {
    let i = opPopup.indexOfSelectedItem
    return i >= 0 && i < FilterEditorController.ops.count ? FilterEditorController.ops[i] : .eq
  }

  private func updateForSelection() {
    let raw = isRaw
    opPopup.isHidden = raw
    let op = selectedOp
    valueField.isEnabled = raw || op.needsValue
    valueField.placeholderString = raw ? "a SQL condition, e.g. price > 10 AND qty < 5" : "value"
    let two = !raw && op.takesTwoValues
    andLabel.isHidden = !two
    value2Field.isHidden = !two
  }

  @objc private func columnChanged() { updateForSelection() }
  @objc private func opChanged() { updateForSelection() }

  func show(relativeTo rect: NSRect, of view: NSView) {
    popover.contentViewController = self
    popover.behavior = .transient
    popover.delegate = self
    popover.show(relativeTo: rect, of: view, preferredEdge: .maxY)
    view.window?.makeFirstResponder(valueField)
  }

  @objc private func applyTapped() {
    let result: Filter
    if isRaw {
      let cond = valueField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
      guard !cond.isEmpty else {
        NSSound.beep()
        return
      }
      result = Filter(column: nil, op: .raw, value: cond)
    } else {
      guard let column = columnPopup.titleOfSelectedItem else {
        NSSound.beep()
        return
      }
      let op = selectedOp
      result = Filter(
        column: column, op: op, value: valueField.stringValue,
        value2: op.takesTwoValues ? value2Field.stringValue : nil)
    }
    popover.close()
    onApply?(result)
  }

  @objc private func removeTapped() {
    popover.close()
    onRemove?()
  }

  @objc private func cancelTapped() {
    popover.close()
  }
}
