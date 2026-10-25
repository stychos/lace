//
// Lace macOS frontend
// GridController: an NSTableView over a RowModel
//
// The table view is virtualized, so its row count is the model's (the whole
// table) and only the visible cells exist. A cell of a page that is not
// loaded asks the model for it and shows a placeholder; the model's
// onRowsChanged reloads the visible rows when the page lands. The grid keeps
// a focused cell (the selected row and a current column), header sorting
// (click: cycle ascending, descending, none; shift-click: a secondary
// column), column widths by name across reloads, the cell editor popover,
// copying and the context menu. What the owner does with edits, deletes,
// sorts and filters goes through GridDelegate.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
protocol GridDelegate: AnyObject {
  func gridSortChanged(_ grid: GridController)
  /// text nil is the explicit NULL action
  func grid(_ grid: GridController, editCell row: Int, column: Int, text: String?)
  func gridDeleteRows(_ grid: GridController)
  func grid(_ grid: GridController, cellFocused row: Int, column: Int)
  func gridSelectionChanged(_ grid: GridController)
  func gridRefresh(_ grid: GridController)
  /// The owner can insert rows (table tabs)
  func gridAddRow(_ grid: GridController)
  /// From the context menu: value nil asks for "is NULL", "" opens the
  /// filter editor on the column
  func grid(_ grid: GridController, filterColumn name: String, value: String?)
  /// ⌘-click or the context menu on a foreign key column: the referenced row
  func gridFollowForeignKey(_ grid: GridController)
  /// The context menu: a table whose key points at the focused row
  func gridReferencedBy(_ grid: GridController)
}

@MainActor
final class GridController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate {
  let model: RowModel
  let scrollView = NSScrollView()
  let tableView = GridTableView()
  weak var delegate: GridDelegate?
  /// Short messages for the user (read-only reasons)
  var toast: ((String) -> Void)?

  var isEditable = false
  var readOnlyReason: String?
  var allowsSorting = true
  var allowsFiltering = true
  /// "Add Row…" in the context menu (table tabs)
  var allowsAdding = false
  /// The first columns of the foreign keys: ⇒ in their header, followable
  var foreignKeyColumns: Set<String> = [] {
    didSet { if foreignKeyColumns != oldValue { rebuildColumns() } }
  }
  var showRowNumbers = false {
    didSet { if showRowNumbers != oldValue { rebuildColumns() } }
  }
  private(set) var sorts: [SortSpec] = []
  /// The model column the focused cell is in
  private(set) var currentColumn = 0

  private static let rowNumberID = NSUserInterfaceItemIdentifier("#")
  private let font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
  private var widths: [String: CGFloat] = [:]
  private var columnNames: [String] = []
  private var focusRow = -1
  private var editor: CellEditorController?

  init(model: RowModel) {
    self.model = model
    super.init()
    tableView.grid = self
    tableView.dataSource = self
    tableView.delegate = self
    tableView.usesAlternatingRowBackgroundColors = true
    tableView.allowsMultipleSelection = true
    tableView.allowsEmptySelection = true
    tableView.allowsColumnReordering = true
    tableView.allowsColumnResizing = true
    tableView.allowsColumnSelection = false
    tableView.columnAutoresizingStyle = .noColumnAutoresizing
    tableView.gridStyleMask = [.solidVerticalGridLineMask]
    tableView.intercellSpacing = NSSize(width: 1, height: 0)
    tableView.rowHeight = 20
    tableView.style = .plain
    tableView.target = self
    tableView.doubleAction = #selector(doubleClicked(_:))
    let menu = NSMenu()
    menu.delegate = self
    tableView.menu = menu

    scrollView.documentView = tableView
    scrollView.hasVerticalScroller = true
    scrollView.hasHorizontalScroller = true
    scrollView.autohidesScrollers = true
    scrollView.borderType = .noBorder
    scrollView.usesPredominantAxisScrolling = false

    model.onColumnsChanged = { [weak self] in self?.columnsChanged() }
    model.onCountChanged = { [weak self] old, new in self?.countChanged(old, new) }
    model.onRowsChanged = { [weak self] range in self?.rowsChanged(range) }

    // A selector observer is removed for us when the controller goes away
    NotificationCenter.default.addObserver(
      self, selector: #selector(columnResized(_:)), name: NSTableView.columnDidResizeNotification,
      object: tableView)
  }

  /// Keep a width the user set, by column name, across reloads
  @objc private func columnResized(_ note: Notification) {
    guard let column = note.userInfo?["NSTableColumn"] as? NSTableColumn, let i = modelColumn(of: column),
      i < model.columns.count
    else { return }
    widths[model.columns[i].name] = column.width
  }

  // MARK: Columns

  private func columnsChanged() {
    // A reset leaves the columns empty until the first page lands: keep the
    // header (widths, sort indicators) meanwhile
    guard !model.columns.isEmpty else {
      tableView.reloadData()
      return
    }
    let names = model.columns.map(\.name)
    if names != columnNames || tableView.tableColumns.isEmpty {
      columnNames = names
      sorts.removeAll { !names.contains($0.column) }
      rebuildColumns()
    } else {
      tableView.reloadData()
    }
  }

  private func rebuildColumns() {
    for c in tableView.tableColumns {
      tableView.removeTableColumn(c)
    }
    if showRowNumbers {
      let c = NSTableColumn(identifier: GridController.rowNumberID)
      c.title = "#"
      c.width = 56
      c.minWidth = 36
      c.maxWidth = 120
      c.headerCell.alignment = .right
      tableView.addTableColumn(c)
    }
    for (i, col) in model.columns.enumerated() {
      let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("c\(i)"))
      let fk = foreignKeyColumns.contains(col.name)
      c.title = fk ? "\(col.name) ⇒" : col.name
      var tip = col.typeName ?? col.type.name
      if col.primaryKey { tip += " · primary key" }
      if fk { tip += " · foreign key: ⌘-click follows it" }
      if !col.nullable { tip += " · NOT NULL" }
      c.headerToolTip = tip
      c.width = widths[col.name] ?? 120
      c.minWidth = 40
      c.maxWidth = 4000
      c.headerCell.alignment = col.type.isNumeric ? .right : .left
      tableView.addTableColumn(c)
    }
    tableView.reloadData()
    updateSortIndicators()
  }

  /// Column widths from the first page: the header and up to 200 rows of
  /// display text, for columns without a width kept from an earlier load
  func measureColumns() {
    let rows = min(model.loadedExtent, 200)
    let attrs: [NSAttributedString.Key: Any] = [.font: font]
    for (i, col) in model.columns.enumerated() {
      guard widths[col.name] == nil, let tc = tableColumn(forModelColumn: i) else { continue }
      var w = (col.name as NSString).size(withAttributes: attrs).width + 28
      for r in 0..<rows {
        guard let v = model.value(row: r, column: i) else { continue }
        let text = v.displayText(maxChars: 80)
        w = max(w, (text as NSString).size(withAttributes: attrs).width + 12)
      }
      let width = min(max(w.rounded(.up), 48), 420)
      tc.width = width
      widths[col.name] = width
    }
  }

  private func tableColumn(forModelColumn i: Int) -> NSTableColumn? {
    tableView.tableColumns.first { $0.identifier.rawValue == "c\(i)" }
  }

  private func modelColumn(of tc: NSTableColumn) -> Int? {
    tc.identifier == GridController.rowNumberID ? nil : Int(tc.identifier.rawValue.dropFirst())
  }

  // MARK: Sorting

  func setSorts(_ sorts: [SortSpec]) {
    self.sorts = sorts
    updateSortIndicators()
  }

  func tableView(_ tableView: NSTableView, didClick tableColumn: NSTableColumn) {
    guard allowsSorting, let col = modelColumn(of: tableColumn), col < model.columns.count else { return }
    let name = model.columns[col].name
    let shift = NSApp.currentEvent?.modifierFlags.contains(.shift) ?? false
    let existing = sorts.firstIndex { $0.column == name }
    if shift {
      if let i = existing {
        if sorts[i].descending {
          sorts.remove(at: i)
        } else {
          sorts[i].descending = true
        }
      } else {
        sorts.append(SortSpec(column: name))
      }
    } else if let i = existing, sorts.count == 1 {
      if sorts[i].descending {
        sorts = []
      } else {
        sorts[i].descending = true
      }
    } else {
      sorts = [SortSpec(column: name)]
    }
    updateSortIndicators()
    delegate?.gridSortChanged(self)
  }

  private func updateSortIndicators() {
    for tc in tableView.tableColumns {
      tableView.setIndicatorImage(nil, in: tc)
    }
    tableView.highlightedTableColumn = nil
    for (n, s) in sorts.enumerated() {
      guard let i = model.columns.firstIndex(where: { $0.name == s.column }),
        let tc = tableColumn(forModelColumn: i)
      else { continue }
      let name = s.descending ? "NSDescendingSortIndicator" : "NSAscendingSortIndicator"
      tableView.setIndicatorImage(NSImage(named: name), in: tc)
      if n == 0 {
        tableView.highlightedTableColumn = tc
      }
    }
  }

  // MARK: Rows

  private func countChanged(_ old: Int, _ new: Int) {
    tableView.noteNumberOfRowsChanged()
    if focusRow >= new {
      focusRow = -1
    }
  }

  private func rowsChanged(_ range: Range<Int>) {
    let visible = tableView.rows(in: tableView.visibleRect)
    let lo = max(range.lowerBound, visible.location)
    let hi = min(range.upperBound, visible.location + visible.length)
    guard lo < hi, tableView.tableColumns.count > 0 else { return }
    tableView.reloadData(
      forRowIndexes: IndexSet(integersIn: lo..<hi),
      columnIndexes: IndexSet(integersIn: 0..<tableView.tableColumns.count))
  }

  func numberOfRows(in tableView: NSTableView) -> Int { model.count }

  func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
    guard let tableColumn else { return nil }
    let id = NSUserInterfaceItemIdentifier("GridCellView")
    let cell =
      (tableView.makeView(withIdentifier: id, owner: nil) as? GridCellView)
      ?? {
        let c = GridCellView(frame: .zero)
        c.identifier = id
        c.label.font = font
        return c
      }()
    if tableColumn.identifier == GridController.rowNumberID {
      cell.configure(text: Format.count(row + 1), style: .rowNumber, alignment: .right)
      cell.isCurrent = false
      return cell
    }
    guard let col = modelColumn(of: tableColumn) else { return nil }
    let numeric = col < model.columns.count && model.columns[col].type.isNumeric
    if let v = model.value(row: row, column: col) {
      if v.isNull {
        cell.configure(text: "NULL", style: .null, alignment: numeric ? .right : .left)
      } else {
        cell.configure(text: v.displayText(maxChars: 200), style: .value, alignment: numeric ? .right : .left)
      }
    } else {
      model.request(row: row)
      cell.configure(text: "…", style: .placeholder, alignment: .center)
    }
    cell.isCurrent = row == focusRow && col == currentColumn
    return cell
  }

  func tableViewSelectionDidChange(_ notification: Notification) {
    let selection = tableView.selectedRowIndexes
    if selection.count == 1, let only = selection.first {
      focusRow = only
    } else if !selection.contains(focusRow) {
      focusRow = selection.last ?? -1
    }
    updateCurrentHighlight()
    delegate?.gridSelectionChanged(self)
    if focusRow >= 0 {
      delegate?.grid(self, cellFocused: focusRow, column: currentColumn)
    }
  }

  // MARK: Focus

  var focusedCell: (row: Int, column: Int)? {
    guard focusRow >= 0, focusRow < model.count, currentColumn < model.columns.count else { return nil }
    return (focusRow, currentColumn)
  }

  var selectedRows: [Int] { Array(tableView.selectedRowIndexes) }
  var selectedCount: Int { tableView.selectedRowIndexes.count }

  func unselectAll() {
    tableView.deselectAll(nil)
  }

  /// Select the row, make the column current, scroll both into view and
  /// give the grid the keyboard
  func focusCell(row: Int, column: Int) {
    guard model.count > 0 else {
      tableView.window?.makeFirstResponder(tableView)
      return
    }
    let r = min(max(row, 0), model.count - 1)
    currentColumn = min(max(column, 0), max(model.columns.count - 1, 0))
    focusRow = r
    tableView.selectRowIndexes(IndexSet(integer: r), byExtendingSelection: false)
    tableView.scrollRowToVisible(r)
    if let tc = tableColumn(forModelColumn: currentColumn) {
      let ci = tableView.column(withIdentifier: tc.identifier)
      if ci >= 0 {
        tableView.scrollColumnToVisible(ci)
      }
    }
    tableView.window?.makeFirstResponder(tableView)
    updateCurrentHighlight()
    delegate?.grid(self, cellFocused: r, column: currentColumn)
  }

  func scrollTo(row: Int) {
    focusCell(row: row, column: currentColumn)
  }

  /// Move the current column in visual order, skipping the row numbers
  fileprivate func moveColumn(by delta: Int) {
    let cols = tableView.tableColumns.filter { $0.identifier != GridController.rowNumberID }
    guard !cols.isEmpty else { return }
    let cur = cols.firstIndex { modelColumn(of: $0) == currentColumn } ?? 0
    let next = min(max(cur + delta, 0), cols.count - 1)
    guard let m = modelColumn(of: cols[next]) else { return }
    currentColumn = m
    tableView.scrollColumnToVisible(tableView.column(withIdentifier: cols[next].identifier))
    updateCurrentHighlight()
    if focusRow >= 0 {
      delegate?.grid(self, cellFocused: focusRow, column: m)
    }
  }

  fileprivate func setCurrentColumn(fromTableColumnIndex i: Int) {
    guard i >= 0, i < tableView.tableColumns.count, let m = modelColumn(of: tableView.tableColumns[i]) else {
      return
    }
    if m != currentColumn {
      currentColumn = m
      updateCurrentHighlight()
      if focusRow >= 0 {
        delegate?.grid(self, cellFocused: focusRow, column: m)
      }
    }
  }

  private func updateCurrentHighlight() {
    tableView.enumerateAvailableRowViews { rowView, row in
      for i in 0..<rowView.numberOfColumns {
        guard let cell = rowView.view(atColumn: i) as? GridCellView else { continue }
        let tc = self.tableView.tableColumns[i]
        cell.isCurrent = row == self.focusRow && self.modelColumn(of: tc) == self.currentColumn
      }
    }
  }

  // MARK: Editing

  @objc private func doubleClicked(_ sender: Any?) {
    let row = tableView.clickedRow
    let col = tableView.clickedColumn
    guard row >= 0, col >= 0 else { return }
    setCurrentColumn(fromTableColumnIndex: col)
    focusRow = row
    beginEdit(initialText: nil)
  }

  /// Open the editor on the focused cell; initialText (typed) replaces the
  /// cell's text, nil starts from the cell's full edit text selected
  func beginEdit(initialText: String?) {
    guard let (row, col) = focusedCell else { return }
    guard isEditable else {
      toast?("Read only: \(readOnlyReason ?? "this result cannot be edited")")
      return
    }
    guard let value = model.value(row: row, column: col) else { return }
    if value.isOmitted {
      toast?("This value is too large to load and cannot be edited")
      return
    }
    guard let tc = tableColumn(forModelColumn: col) else { return }
    let ci = tableView.column(withIdentifier: tc.identifier)
    guard ci >= 0 else { return }
    tableView.scrollRowToVisible(row)
    let rect = tableView.frameOfCell(atColumn: ci, row: row)
    let column = model.columns[col]
    let ed = CellEditorController(
      column: column.name, typeName: column.typeName ?? column.type.name,
      text: initialText ?? value.editText, selectAll: initialText == nil)
    ed.onSave = { [weak self] text in
      guard let self else { return }
      editor = nil
      delegate?.grid(self, editCell: row, column: col, text: text)
      tableView.window?.makeFirstResponder(tableView)
    }
    ed.onCancel = { [weak self] in
      guard let self else { return }
      editor = nil
      tableView.window?.makeFirstResponder(tableView)
    }
    editor = ed
    ed.show(relativeTo: rect, of: tableView)
  }

  fileprivate func requestDelete() {
    delegate?.gridDeleteRows(self)
  }

  /// The focused cell's exact text, or the selected rows as tab-separated lines
  func copySelection() {
    let pb = NSPasteboard.general
    let rows = selectedRows
    if rows.count > 1 {
      let lines = rows.compactMap { r -> String? in
        guard model.isLoaded(row: r) else { return nil }
        return (0..<model.columns.count).map { model.value(row: r, column: $0)?.displayText() ?? "" }
          .joined(separator: "\t")
      }
      pb.clearContents()
      pb.setString(lines.joined(separator: "\n"), forType: .string)
    } else if let (row, col) = focusedCell, let v = model.value(row: row, column: col) {
      pb.clearContents()
      pb.setString(v.isNull ? "" : v.editText, forType: .string)
    }
  }

  // MARK: Context menu

  func menuNeedsUpdate(_ menu: NSMenu) {
    menu.removeAllItems()
    let row = tableView.clickedRow
    let tci = tableView.clickedColumn
    guard row >= 0 else { return }
    if tci >= 0 {
      setCurrentColumn(fromTableColumnIndex: tci)
    }
    if !tableView.selectedRowIndexes.contains(row) {
      tableView.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
    }
    focusRow = row
    func add(_ title: String, _ action: Selector, enabled: Bool = true) {
      let item = menu.addItem(withTitle: title, action: action, keyEquivalent: "")
      item.target = self
      item.isEnabled = enabled
    }
    add("Copy", #selector(menuCopy))
    if isEditable {
      add("Edit…", #selector(menuEdit))
      add("Set NULL", #selector(menuSetNull))
      add("Set Empty", #selector(menuSetEmpty))
    }
    if allowsFiltering, let (r, c) = focusedCell, c < model.columns.count {
      menu.addItem(.separator())
      let value = model.value(row: r, column: c)
      let name = model.columns[c].name
      if let value, !value.isNull, !value.isOmitted {
        add("Filter by This Value", #selector(menuFilterValue))
      }
      add("Filter: \(name) Is NULL", #selector(menuFilterNull))
      add("Add Filter on \(name)…", #selector(menuFilterAdd))
    }
    if let (_, c) = focusedCell, c < model.columns.count {
      menu.addItem(.separator())
      add("Follow Foreign Key", #selector(menuFollow), enabled: foreignKeyColumns.contains(model.columns[c].name))
      add("Referenced By…", #selector(menuReferencedBy), enabled: canFollow)
    }
    if isEditable {
      menu.addItem(.separator())
      let n = max(tableView.selectedRowIndexes.count, 1)
      add(n > 1 ? "Delete \(Format.count(n)) Rows" : "Delete Row", #selector(menuDelete))
    }
    menu.addItem(.separator())
    if isEditable && allowsAdding {
      add("Add Row…", #selector(menuAddRow))
    }
    add("Refresh", #selector(menuRefresh))
  }

  @objc private func menuAddRow() { delegate?.gridAddRow(self) }
  @objc private func menuFollow() { delegate?.gridFollowForeignKey(self) }
  @objc private func menuReferencedBy() { delegate?.gridReferencedBy(self) }
  /// The rows are a table's (the owner says): the referencing tables can be listed
  var canFollow = false
  fileprivate func cmdClicked(row: Int, column tci: Int) {
    setCurrentColumn(fromTableColumnIndex: tci)
    focusRow = row
    if let (_, c) = focusedCell, c < model.columns.count, foreignKeyColumns.contains(model.columns[c].name) {
      delegate?.gridFollowForeignKey(self)
    }
  }

  @objc private func menuCopy() { copySelection() }
  @objc private func menuEdit() { beginEdit(initialText: nil) }
  @objc private func menuDelete() { requestDelete() }
  @objc private func menuRefresh() { delegate?.gridRefresh(self) }

  @objc private func menuSetNull() {
    if let (r, c) = focusedCell {
      delegate?.grid(self, editCell: r, column: c, text: nil)
    }
  }

  @objc private func menuSetEmpty() {
    if let (r, c) = focusedCell {
      delegate?.grid(self, editCell: r, column: c, text: "")
    }
  }

  @objc private func menuFilterValue() {
    guard let (r, c) = focusedCell, let v = model.value(row: r, column: c), !v.isNull, !v.isOmitted else {
      return
    }
    delegate?.grid(self, filterColumn: model.columns[c].name, value: v.editText)
  }

  @objc private func menuFilterNull() {
    if let (_, c) = focusedCell {
      delegate?.grid(self, filterColumn: model.columns[c].name, value: nil)
    }
  }

  @objc private func menuFilterAdd() {
    if let (_, c) = focusedCell {
      delegate?.grid(self, filterColumn: model.columns[c].name, value: "")
    }
  }
}

/// The table view: keyboard handling for cells and the click-to-column mapping
final class GridTableView: NSTableView {
  weak var grid: GridController?

  override var acceptsFirstResponder: Bool { true }

  override func keyDown(with event: NSEvent) {
    guard let grid else {
      super.keyDown(with: event)
      return
    }
    let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
    let shift = flags.contains(.shift)
    let plain = flags.subtracting([.shift, .numericPad, .function]).isEmpty
    if plain {
      switch event.keyCode {
      case 123:  // left
        grid.moveColumn(by: -1)
        return
      case 124:  // right
        grid.moveColumn(by: 1)
        return
      case 48:  // tab
        grid.moveColumn(by: shift ? -1 : 1)
        return
      case 36, 76, 120:  // return, enter, F2
        grid.beginEdit(initialText: nil)
        return
      case 51, 117:  // backspace, forward delete
        grid.requestDelete()
        return
      default:
        break
      }
      // A printable character starts an edit with it
      if grid.isEditable, !flags.contains(.function), let chars = event.characters,
        let scalar = chars.unicodeScalars.first, scalar.value >= 0x20, scalar.value != 0x7f
      {
        grid.beginEdit(initialText: chars)
        return
      }
    }
    super.keyDown(with: event)
  }

  override func mouseDown(with event: NSEvent) {
    let point = convert(event.locationInWindow, from: nil)
    let col = column(at: point)
    if col >= 0 {
      grid?.setCurrentColumn(fromTableColumnIndex: col)
    }
    let r = row(at: point)
    if event.modifierFlags.contains(.command), r >= 0, col >= 0 {
      selectRowIndexes(IndexSet(integer: r), byExtendingSelection: false) /* ⌘-click: follow the key */
      grid?.cmdClicked(row: r, column: col)
      return
    }
    super.mouseDown(with: event)
  }

  @objc func copy(_ sender: Any?) {
    grid?.copySelection()
  }
}

/// One cell: a label whose colour follows the value kind and the row's
/// selection; the focused cell draws an accent border
final class GridCellView: NSTableCellView {
  enum Style {
    case value, null, placeholder, rowNumber
  }

  let label = NSTextField(labelWithString: "")
  var isCurrent = false {
    didSet { if isCurrent != oldValue { needsDisplay = true } }
  }
  private var style: Style = .value

  override init(frame: NSRect) {
    super.init(frame: frame)
    label.translatesAutoresizingMaskIntoConstraints = false
    label.lineBreakMode = .byTruncatingTail
    label.cell?.usesSingleLineMode = true
    label.cell?.truncatesLastVisibleLine = true
    label.isSelectable = false
    addSubview(label)
    textField = label
    NSLayoutConstraint.activate([
      label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 5),
      label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -5),
      label.centerYAnchor.constraint(equalTo: centerYAnchor),
    ])
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  func configure(text: String, style: Style, alignment: NSTextAlignment) {
    label.stringValue = text
    label.alignment = alignment
    self.style = style
    applyColors()
  }

  override var backgroundStyle: NSView.BackgroundStyle {
    didSet { applyColors() }
  }

  private func applyColors() {
    if backgroundStyle == .emphasized {
      label.textColor = .alternateSelectedControlTextColor
      return
    }
    switch style {
    case .value: label.textColor = .labelColor
    case .null: label.textColor = .secondaryLabelColor
    case .placeholder: label.textColor = .quaternaryLabelColor
    case .rowNumber: label.textColor = .secondaryLabelColor
    }
  }

  override func draw(_ dirtyRect: NSRect) {
    super.draw(dirtyRect)
    if isCurrent {
      NSColor.controlAccentColor.setStroke()
      let path = NSBezierPath(rect: bounds.insetBy(dx: 1, dy: 1))
      path.lineWidth = 2
      path.stroke()
    }
  }
}
