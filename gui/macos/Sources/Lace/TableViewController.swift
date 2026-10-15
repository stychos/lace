//
// Lace macOS frontend
// TableViewController: one table, as a tab of a connection
//
// Loads the schema, then page 0 through the row model, then the count
// (approximate for big server tables when the preference is on, shown as
// ≈). Filters and header sorts rebuild the WHERE and ORDER BY (liblace's
// builders) and reload under a new generation. Pages are read with keyset
// paging when they can be (the table has a primary key, no sort column is
// a server float): every page orders by the sort columns then the key, a
// page next to a loaded one is read as the rows after its last row (or
// before its first, in reverse), and only a jump uses OFFSET. A backward
// keyset page that comes back short (rows changed meanwhile) is read again
// with OFFSET. Edits and deletes go through
// EditController. `onTouched` tells the connection page that a preview tab
// is being worked in and should be kept.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class TableViewController: NSViewController, GridDelegate {
  let connection: Connection
  let table: String
  let prefs: Prefs
  let model: RowModel
  let grid: GridController
  var onTouched: (() -> Void)?
  /// A statement that wrote (the SQL liblace sent), for history
  var record: ((String) -> Void)?
  var toast: ((String) -> Void)? {
    didSet {
      grid.toast = toast
      edit.toast = toast
    }
  }

  private let filterBar = FilterBarView(frame: .zero)
  private let banner = BannerView(frame: .zero)
  private let statusLabel = NSTextField(labelWithString: "")
  private let cellLabel = NSTextField(labelWithString: "")
  private let spinner = NSProgressIndicator()
  private let edit: EditController
  private var schema: Schema?
  private var whereBody: String?
  private var orderBy: String?
  private var keyset: Keyset?
  /// How the last page was read (the script driver checks it)
  private(set) var lastPaging = "none"
  private var countPending = false
  private var inFlight = 0 {
    didSet { inFlight > 0 ? spinner.startAnimation(nil) : spinner.stopAnimation(nil) }
  }
  private var editable = false

  init(connection: Connection, table: String, prefs: Prefs) {
    self.connection = connection
    self.table = table
    self.prefs = prefs
    model = RowModel(pageSize: prefs.pageSize)
    grid = GridController(model: model)
    edit = EditController(connection: connection, model: model, grid: grid)
    super.init(nibName: nil, bundle: nil)
    edit.table = table
    edit.confirmDelete = prefs.confirmDelete
    edit.onRowsDeleted = { [weak self] _ in self?.updateStatus() }
    edit.onWrite = { [weak self] sql in self?.record?(sql) }
    grid.delegate = self
    grid.allowsAdding = true
    grid.showRowNumbers = prefs.rowNumbers != .never
    model.fetch = { [weak self] _, page in self?.fetch(page: page) }
    model.onTotalChanged = { [weak self] in self?.updateStatus() }
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 600))
    filterBar.isHidden = true
    filterBar.onChanged = { [weak self] in
      guard let self else { return }
      onTouched?()
      startLoad()
    }

    statusLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    statusLabel.lineBreakMode = .byTruncatingTail
    cellLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    cellLabel.textColor = .secondaryLabelColor
    cellLabel.lineBreakMode = .byTruncatingTail
    spinner.style = .spinning
    spinner.controlSize = .small
    spinner.isDisplayedWhenStopped = false
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let footer = NSStackView(views: [statusLabel, spinner, spacer, cellLabel])
    footer.orientation = .horizontal
    footer.alignment = .centerY
    footer.spacing = 8
    footer.edgeInsets = NSEdgeInsets(top: 3, left: 8, bottom: 3, right: 8)
    footer.setContentHuggingPriority(.required, for: .vertical)

    let stack = NSStackView(views: [banner, filterBar, grid.scrollView, SeparatorView(), footer])
    stack.orientation = .vertical
    stack.alignment = .width
    stack.spacing = 0
    stack.translatesAutoresizingMaskIntoConstraints = false
    grid.scrollView.setContentHuggingPriority(.defaultLow, for: .vertical)
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    view = root
    edit.hostView = root
  }

  override func viewDidLoad() {
    super.viewDidLoad()
    loadSchema()
  }

  func tearDown() {
    model.fetch = nil
  }

  // MARK: Loading

  private func loadSchema() {
    inFlight += 1
    let connection = self.connection
    let table = self.table
    Task { [weak self] in
      let outcome = await Result { try await connection.schema(of: table) }
      guard let self else { return }
      inFlight -= 1
      switch outcome {
      case .success(let s):
        schema = s
        edit.schema = s
        filterBar.schema = s
        editable = !s.primaryKey.isEmpty
        grid.readOnlyReason = editable ? nil : "the table has no primary key"
        grid.showRowNumbers = prefs.rowNumbers(for: s)
      case .failure(let error):
        banner.show("\(error)")
        editable = false
        grid.readOnlyReason = "the table's structure could not be read"
      }
      grid.isEditable = editable
      startLoad()
    }
  }

  private func rebuildClauses() {
    let driver = connection.driver
    whereBody = schema.flatMap {
      SQL.whereClause(filters: filterBar.filters, search: filterBar.search, schema: $0, driver: driver)
    }
    keyset = schema.flatMap { Keyset(schema: $0, sorts: grid.sorts, driver: driver) }
    // Every page, OFFSET ones too, uses the keyset order, so they agree
    orderBy = keyset?.orderBy(forward: true) ?? SQL.orderBy(grid.sorts, driver: driver)
  }

  /// The SQL for a page: after the previous page's last row or before the
  /// next page's first row when one is loaded, else LIMIT/OFFSET
  private func pageSQL(page: Int, allowKeyset: Bool) -> (sql: String, reversed: Bool, keyset: Bool)? {
    let size = model.pageSize
    let driver = connection.driver
    if allowKeyset, let keyset {
      func and(_ edge: String) -> String {
        whereBody.map { "(\($0)) AND \(edge)" } ?? edge
      }
      if page > 0, let (prev, _) = model.rowResult(page * size - 1), prev.rowCount == size,
        let edge = keyset.whereClause(rows: prev, row: size - 1, forward: true),
        let sql = SQL.tablePage(
          driver: driver, table: table, where: and(edge), orderBy: keyset.orderBy(forward: true), limit: size,
          offset: 0)
      {
        return (sql, false, true)
      }
      if let (next, index) = model.rowResult((page + 1) * size), index == 0,
        let edge = keyset.whereClause(rows: next, row: 0, forward: false),
        let sql = SQL.tablePage(
          driver: driver, table: table, where: and(edge), orderBy: keyset.orderBy(forward: false), limit: size,
          offset: 0)
      {
        return (sql, true, true)
      }
    }
    guard
      let sql = SQL.tablePage(
        driver: driver, table: table, where: whereBody, orderBy: orderBy, limit: size, offset: page * size)
    else { return nil }
    return (sql, false, false)
  }

  private func startLoad() {
    banner.hide()
    rebuildClauses()
    countPending = false
    model.reset()
    model.request(row: 0)
    updateStatus()
  }

  private func fetch(page: Int) {
    let size = model.pageSize
    guard let plan = pageSQL(page: page, allowKeyset: true) else { return }
    let generation = model.generation
    let connection = self.connection
    inFlight += 1
    Task { [weak self] in
      var outcome = await Result { try await connection.read(plan.sql) }
      var used = plan
      if plan.reversed, case .success(let r) = outcome, r.rowCount < size, let self,
        generation == model.generation, let again = pageSQL(page: page, allowKeyset: false)
      {
        // Short backward page: rows changed under us; the offset read is right
        used = again
        outcome = await Result { try await connection.read(again.sql) }
      }
      guard let self else { return }
      inFlight -= 1
      lastPaging = used.keyset ? (used.reversed ? "keyset-backward" : "keyset") : "offset"
      switch outcome {
      case .success(let loaded):
        let result = used.reversed ? loaded.reversed() : loaded
        let first = page == 0 && generation == model.generation && model.columns.isEmpty
        model.pageLoaded(generation: generation, page: page, result: result)
        if generation == model.generation {
          if first {
            grid.measureColumns()
          }
          if !model.totalKnown {
            startCount()
          }
        }
      case .failure(let error):
        model.pageFailed(generation: generation, page: page)
        if generation == model.generation, (error as? LaceError)?.isCancelled != true {
          banner.show("\(error)")
        }
      }
      updateStatus()
    }
  }

  private func startCount() {
    guard !countPending else { return }
    countPending = true
    let generation = model.generation
    let approximate = prefs.approximateCounts && whereBody == nil
    let whereBody = whereBody
    let connection = self.connection
    let table = self.table
    inFlight += 1
    Task { [weak self] in
      let outcome = await Result {
        try await connection.count(table: table, where: whereBody, approximate: approximate)
      }
      guard let self else { return }
      inFlight -= 1
      countPending = false
      guard generation == model.generation, case .success(let counted) = outcome, counted.count >= 0 else {
        return  // no count: the model keeps growing by pages
      }
      model.setTotal(Int(counted.count), known: true, approximate: counted.approximate)
    }
  }

  // MARK: Status

  private func updateStatus() {
    var s: String
    let total = model.total
    if !model.totalKnown {
      s = "\(Format.count(total))+ rows"
    } else if model.totalApproximate {
      s = "≈ \(Format.plural(total, "row"))"
    } else {
      s = Format.plural(total, "row")
    }
    if !filterBar.isEmpty {
      s += " · filtered"
    }
    let selected = grid.selectedCount
    if selected > 0 {
      s += " · \(Format.count(selected)) selected"
    }
    if !editable, let why = grid.readOnlyReason {
      s += " · read only: \(why)"
    }
    statusLabel.stringValue = s
  }

  // MARK: Public

  var filters: [Filter] { filterBar.filters }
  var search: String? { filterBar.search }
  var sorts: [SortSpec] { grid.sorts }
  var filtersVisible: Bool { !filterBar.isHidden }

  /// Session restore: the state loads with the schema if that is still on
  /// its way, else now
  func restore(filters: [Filter], search: String?, sorts: [SortSpec], filtersVisible: Bool) {
    filterBar.setFilters(filters, search: search)
    filterBar.isHidden = !filtersVisible && filters.isEmpty && search == nil
    grid.setSorts(sorts)
    if schema != nil {
      startLoad()
    }
  }
  /// A schema, page or count load is in flight
  var isLoading: Bool { inFlight > 0 || countPending }

  func addFilter(_ filter: Filter) {
    filterBar.isHidden = false
    filterBar.add(filter)
  }

  func setSorts(_ sorts: [SortSpec]) {
    grid.setSorts(sorts)
    startLoad()
  }

  func focusGrid() {
    if let cell = grid.focusedCell {
      grid.focusCell(row: cell.row, column: cell.column)
    } else if model.count > 0 {
      grid.focusCell(row: 0, column: 0)
    } else {
      view.window?.makeFirstResponder(grid.tableView)
    }
  }

  func refresh() {
    if schema == nil {
      loadSchema()
    } else {
      startLoad()
    }
  }

  // MARK: Actions (responder chain)

  @objc func refresh(_ sender: Any?) {
    refresh()
  }

  @objc func toggleFilters(_ sender: Any?) {
    if !filterBar.isHidden && !filterBar.hasKeyboardFocus {
      filterBar.focusSearch()
      return
    }
    filterBar.isHidden.toggle()
    if filterBar.isHidden {
      focusGrid()
    } else {
      filterBar.focusSearch()
    }
  }

  @objc func gotoRow(_ sender: Any?) {
    let total = model.total
    let body = "Row number (1 to \(Format.count(total))\(model.totalKnown ? "" : "+"))"
    Task { [weak self] in
      guard let self else { return }
      guard let text = await Alerts.prompt("Go to Row", body, action: "Go", window: view.window),
        let n = Int(text.trimmingCharacters(in: .whitespaces)), n >= 1
      else { return }
      var row = n - 1
      if model.total > 0 && row >= model.total {
        row = model.total - 1
      }
      grid.scrollTo(row: row)
    }
  }

  @objc func showStructure(_ sender: Any?) {
    guard let schema else {
      toast?("The table's structure is not loaded")
      return
    }
    presentAsSheet(StructureViewController(table: table, schema: schema))
  }

  @objc func deleteRows(_ sender: Any?) {
    edit.deleteSelected()
  }

  /// The add-row sheet (nil when the table cannot take rows)
  @discardableResult
  func makeAddRow() -> AddRowViewController? {
    guard let schema else {
      toast?("The table's structure is not loaded")
      return nil
    }
    guard editable else {
      toast?("Read only: \(grid.readOnlyReason ?? "this table cannot be edited")")
      return nil
    }
    let vc = AddRowViewController(connection: connection, table: table, schema: schema)
    vc.onInserted = { [weak self] sql in self?.rowInserted(sql) }
    return vc
  }

  @objc func addRow(_ sender: Any?) {
    guard let vc = makeAddRow() else { return }
    presentAsSheet(vc)
  }

  /// A scripted insert: fields by column name (nil text is NULL, "" Empty)
  func insertRow(_ fields: [(column: String, text: String?)]) async -> Bool {
    guard let vc = makeAddRow() else { return false }
    vc.loadViewIfNeeded()
    for f in fields where !vc.set(column: f.column, text: f.text) {
      toast?("add row: no column \(f.column)")
      return false
    }
    return await vc.insert()
  }

  private func rowInserted(_ sql: String?) {
    toast?("Row inserted")
    if let sql {
      record?(sql)
    }
    onTouched?()
    startLoad()
  }

  // MARK: GridDelegate

  func gridSortChanged(_ grid: GridController) {
    onTouched?()
    startLoad()
  }

  func grid(_ grid: GridController, editCell row: Int, column: Int, text: String?) {
    onTouched?()
    edit.editCell(row: row, column: column, text: text)
  }

  func gridDeleteRows(_ grid: GridController) {
    edit.deleteSelected()
  }

  func grid(_ grid: GridController, cellFocused row: Int, column: Int) {
    guard column < model.columns.count else {
      cellLabel.stringValue = ""
      return
    }
    let c = model.columns[column]
    cellLabel.stringValue = "Row \(Format.count(row + 1)) · \(c.name) · \(c.typeName ?? c.type.name)"
  }

  func gridSelectionChanged(_ grid: GridController) {
    updateStatus()
  }

  func gridRefresh(_ grid: GridController) {
    refresh()
  }

  func gridAddRow(_ grid: GridController) {
    addRow(nil)
  }

  func grid(_ grid: GridController, filterColumn name: String, value: String?) {
    filterBar.isHidden = false
    onTouched?()
    if let value, !value.isEmpty {
      filterBar.add(Filter(column: name, op: .eq, value: value))
    } else if value == nil {
      filterBar.add(Filter(column: name, op: .null, value: ""))
    } else {
      filterBar.openAdd(column: name)
    }
  }
}
