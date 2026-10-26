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

final class TableViewController: NSViewController, GridDelegate, NSMenuDelegate {
  let connection: Connection
  /// What the tab shows: a table, a database view, a routine or a trigger
  /// (a rename changes the name: rename(to:))
  private(set) var object: CatalogObject
  var table: String { object.name }
  let prefs: Prefs
  let model: RowModel
  let grid: GridController
  var onTouched: (() -> Void)?
  /// A statement that wrote (the SQL liblace sent), for history
  var record: ((String) -> Void)?
  /// Open a table filtered by a followed key (the connection page reuses an open tab)
  var onOpenFiltered: ((String, [Filter]) -> Void)?
  /// Open another object (a trigger from Information, a referencing table's keys)
  var onOpenObject: ((CatalogObject, SubView) -> Void)?
  /// The connection's objects for the Information view (the sidebar's list)
  var objectsProvider: (() -> [CatalogObject])?
  /// The connection's tables (the foreign key editor's referenced table)
  var tablesProvider: (() -> [String])?
  /// A structure change or table action ran: the connection page reloads,
  /// renames or closes the object's tabs and the sidebar (DdlRunner)
  var onDdlApplied: ((DdlAfter) -> Void)?
  /// Open a query view holding text (a template, a definition)
  var onNewQuery: ((String) -> Void)?
  var toast: ((String) -> Void)? {
    didSet {
      grid.toast = toast
      edit.toast = toast
    }
  }

  private let strip = NSSegmentedControl()
  private let actionsButton = NSPopUpButton(frame: .zero, pullsDown: true)
  let list = CatalogListController()
  let filterBar = FilterBarView(frame: .zero)
  private let banner = BannerView(frame: .zero)
  private let lockBanner = LockBannerView(frame: .zero)
  private let statusLabel = NSTextField(labelWithString: "")
  private let cellLabel = NSTextField(labelWithString: "")
  private let spinner = NSProgressIndicator()
  private let edit: EditController
  private(set) var schema: Schema?
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
  /// The sub-view shown (saved in the session)
  private(set) var subview: SubView
  /// What the other sub-views read when first shown; a refresh drops them
  private(set) var info: TableInfo?
  private var infoError: String?
  private var infoCount: Int64 = -1
  private var counting = false
  var dependents: Dependents?
  private var dependentsError: String?
  private(set) var definition: String?
  private var definitionError: String?
  private var catalogLoading = 0

  convenience init(connection: Connection, table: String, prefs: Prefs) {
    self.init(connection: connection, object: CatalogObject(kind: .table, name: table), prefs: prefs)
  }

  init(connection: Connection, object: CatalogObject, prefs: Prefs) {
    self.connection = connection
    self.object = object
    self.prefs = prefs
    subview = SubView.available(for: object.kind).contains(.data) ? .data : .structure
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

    strip.segmentStyle = .texturedSquare
    strip.trackingMode = .selectOne
    let available = SubView.available(for: object.kind)
    strip.segmentCount = available.count
    for (i, sv) in available.enumerated() {
      strip.setLabel(sv.label(for: object.kind), forSegment: i)
      strip.setTag(sv.rawValue, forSegment: i)
    }
    strip.selectedSegment = available.firstIndex(of: subview) ?? 0
    strip.target = self
    strip.action = #selector(stripChanged(_:))
    actionsButton.controlSize = .small
    actionsButton.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    actionsButton.menu = NSMenu()
    actionsButton.menu?.delegate = self
    actionsButton.menu?.addItem(withTitle: "Actions", action: nil, keyEquivalent: "")
    actionsButton.toolTip = "The table's actions and the sub-view's editors"
    let stripSpacer = NSView()
    stripSpacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let stripRow = NSStackView(views: [strip, stripSpacer, actionsButton])
    stripRow.orientation = .horizontal
    stripRow.edgeInsets = NSEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
    stripRow.setContentHuggingPriority(.required, for: .vertical)
    list.onActivate = { [weak self] tag in self?.activate(tag: tag) }
    list.onDelete = { [weak self] in self?.runStructureAction(.drop) }
    list.menuProvider = { [weak self] in self?.structureMenu() }
    list.scrollView.setContentHuggingPriority(.defaultLow, for: .vertical)
    lockBanner.onCancel = { [weak self] in self?.connection.cancel() }

    let stack = NSStackView(views: [lockBanner, stripRow, banner, filterBar, grid.scrollView, list.scrollView, SeparatorView(), footer])
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
    if SubView.available(for: object.kind).contains(.data) {
      loadSchema()
    }
    showSubView()
  }

  func tearDown() {
    model.fetch = nil
  }

  /// The object was renamed (a rename ran): the tab follows
  func rename(to name: String) {
    object = CatalogObject(kind: object.kind, name: name, table: object.table, detail: object.detail)
    edit.table = name
  }

  /// A change to the object ran: everything is read again
  func reloadAfterChange() {
    dropCatalog()
    if SubView.available(for: object.kind).contains(.data) {
      loadSchema()
    } else {
      renderList()
    }
  }

  /// The lock banner: a statement the user asked for runs on the connection
  func setLocked(_ statement: Connection.UserStatement?) {
    lockBanner.statement = statement
    let locked = statement != nil
    grid.tableView.isEnabled = !locked
    list.tableView.isEnabled = !locked
    filterBar.isHidden = filterBar.isHidden || locked ? filterBar.isHidden : false
    strip.isEnabled = !locked
    actionsButton.isEnabled = !locked
  }

  /// The lock banner's text, for scripts; nil when hidden
  var lockBannerText: String? { lockBanner.isHidden ? nil : lockBanner.text }

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
        grid.foreignKeyColumns = Set(s.foreignKeys.compactMap(\.columns.first))
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
      renderList()
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
    dropCatalog()
    if !SubView.available(for: object.kind).contains(.data) {
      renderList()
    } else if schema == nil || subview != .data {
      loadSchema()  // the columns, keys and indexes again
    } else {
      startLoad()
    }
  }

  // MARK: Sub-views

  func dropCatalog() {
    info = nil
    infoError = nil
    infoCount = -1
    dependents = nil
    dependentsError = nil
    definition = nil
    definitionError = nil
  }

  func setSubView(_ sv: SubView) {
    guard SubView.available(for: object.kind).contains(sv) else {
      toast?("A \(object.kind.word.lowercased()) has no \(sv.label(for: object.kind))")
      return
    }
    guard sv != subview else { return }
    subview = sv
    showSubView()
  }

  private func showSubView() {
    let available = SubView.available(for: object.kind)
    strip.selectedSegment = available.firstIndex(of: subview) ?? 0
    let data = subview == .data
    filterBar.isHidden = !data || (filterBar.filters.isEmpty && filterBar.search == nil && filterBar.isHidden)
    grid.scrollView.isHidden = !data
    list.scrollView.isHidden = data
    if !data {
      renderList()
    }
  }

  @objc private func stripChanged(_ sender: Any?) {
    let tag = strip.tag(forSegment: strip.selectedSegment)
    if let sv = SubView(rawValue: tag) {
      setSubView(sv)
      if sv != .data {
        list.focus()
      } else {
        focusGrid()
      }
    }
  }

  /// The list of the current sub-view, from what is loaded; asks for what is not
  func renderList() {
    guard subview != .data else { return }
    let dataLike = SubView.available(for: object.kind).contains(.data)
    if dataLike && schema == nil {
      list.show(nil, message: inFlight > 0 ? "Loading…" : "The structure could not be read")
      return
    }
    switch subview {
    case .structure:
      if object.kind == .table, let schema {
        list.show(CatalogTable.structure(schema))
      } else {
        loadDefinition()
        if let definition {
          list.show(CatalogTable.definition(definition, width: 110, columns: object.kind == .view ? schema : nil))
        } else {
          list.show(nil, message: definitionError ?? "Loading…")
        }
      }
    case .indexes:
      if let schema, let t = CatalogTable.indexes(schema) {
        list.show(t)
      } else {
        list.show(nil, message: "No indexes or constraints")
      }
    case .foreignKeys:
      guard let schema else { return }
      loadDependents()
      list.show(
        CatalogTable.foreignKeys(
          schema, table: table, dependents: dependents, loaded: dependents != nil, error: dependentsError))
    case .information:
      loadInfo()
      // the exact count: asked for here, or Data's when it was counted and unfiltered
      let exact: Int64 =
        infoCount >= 0
        ? infoCount
        : (dataLike && model.totalKnown && !model.totalApproximate && filterBar.isEmpty ? Int64(model.total) : -1)
      list.show(
        CatalogTable.info(
          object, info: info, error: infoError, exact: exact, counting: counting, objects: objectsProvider?() ?? []
        ).appending(infoActionRows()))
    case .data:
      break
    }
  }

  private func loadInfo() {
    guard info == nil, infoError == nil, object.kind == .table || object.kind == .view else { return }
    guard catalogLoading == 0 else { return }
    catalogLoading += 1
    let connection = self.connection
    let table = self.table
    Task { [weak self] in
      let outcome = await Result { try await connection.tableInfo(of: table) }
      guard let self else { return }
      catalogLoading -= 1
      switch outcome {
      case .success(let i): info = i
      case .failure(let error): infoError = "\(error)"
      }
      renderList()
    }
  }

  private func loadDependents() {
    guard dependents == nil, dependentsError == nil, catalogLoading == 0 else { return }
    catalogLoading += 1
    let connection = self.connection
    let table = self.table
    Task { [weak self] in
      let outcome = await Result { try await connection.dependents(of: table) }
      guard let self else { return }
      catalogLoading -= 1
      switch outcome {
      case .success(let d): dependents = d
      case .failure(let error): dependentsError = "\(error)"
      }
      renderList()
    }
  }

  func loadDefinition() {
    guard definition == nil, definitionError == nil, catalogLoading == 0 else { return }
    catalogLoading += 1
    let connection = self.connection
    var object = self.object
    if object.kind == .trigger, object.table == nil,
      let o = objectsProvider?().first(where: { $0.kind == .trigger && $0.name == object.name })
    {
      object = o  // restored without its table: the object list knows it
    }
    Task { [weak self] in
      let outcome = await Result { try await connection.definition(of: object) }
      guard let self else { return }
      catalogLoading -= 1
      switch outcome {
      case .success(let sql): definition = sql
      case .failure(let error): definitionError = "\(error)"
      }
      renderList()
    }
  }

  /// Return or a double click on a list row
  private func activate(tag: Int) {
    if runInfoTag(tag) { return }
    if subview == .structure, object.kind == .table, tag >= CatalogTable.Tag.column {
      runStructureAction(.edit)
      return
    }
    if subview == .structure, object.kind != .table {
      editDefinitionInQuery()
      return
    }
    if subview == .indexes, tag >= CatalogTable.Tag.index {
      runStructureAction(.rename)
      return
    }
    if subview == .foreignKeys, tag >= CatalogTable.Tag.foreignKey, tag < CatalogTable.Tag.dependent {
      runStructureAction(.edit)
      return
    }
    if tag == CatalogTable.Tag.rows, subview == .information {
      guard !counting else { return }
      counting = true
      renderList()
      let connection = self.connection
      let table = self.table
      Task { [weak self] in
        let outcome = await Result { try await connection.count(table: table, approximate: false, user: true) }
        guard let self else { return }
        counting = false
        if case .success(let c) = outcome { infoCount = c.count } else { toast?("The count failed") }
        renderList()
      }
      return
    }
    if tag >= CatalogTable.Tag.trigger, let objects = objectsProvider?() {
      let i = tag - CatalogTable.Tag.trigger
      if i < objects.count, objects[i].kind == .trigger {
        onOpenObject?(objects[i], .structure)
      }
      return
    }
    if tag >= CatalogTable.Tag.dependent, tag < CatalogTable.Tag.trigger, let deps = dependents {
      let i = tag - CatalogTable.Tag.dependent
      if i < deps.entries.count {
        onOpenObject?(CatalogObject(kind: .table, name: deps.entries[i].table), .foreignKeys)
      }
    }
  }

  /// The current list's rows as text (the script driver)
  var listRows: [String] { (0..<list.rowCount).compactMap { list.rowText($0) } }

  /// Session restore of the sub-view
  func restore(subview: SubView) {
    guard SubView.available(for: object.kind).contains(subview) else { return }
    self.subview = subview
    showSubView()
  }

  /// Show the table's rows filtered by a followed key (the filters replace the tab's)
  func showFiltered(_ filters: [Filter]) {
    filterBar.setFilters(filters, search: nil)
    filterBar.isHidden = false
    if subview != .data {
      subview = .data
      showSubView()
    }
    onTouched?()
    if schema != nil {
      startLoad()
    }
  }

  @objc func followForeignKey(_ sender: Any?) {
    guard subview == .data else { return }
    Follow.follow(grid: grid, schema: schema, toast: toast) { [weak self] table, filters in
      self?.onOpenFiltered?(table, filters)
    }
  }

  @objc func referencedBy(_ sender: Any?) {
    guard subview == .data, grid.focusedCell != nil else {
      toast?("No row to follow")
      return
    }
    if let dependents {
      Follow.referencedBy(grid: grid, dependents: dependents, table: table, toast: toast) { [weak self] t, f in
        self?.onOpenFiltered?(t, f)
      }
      return
    }
    let connection = self.connection
    let table = self.table
    Task { [weak self] in
      let outcome = await Result { try await connection.dependents(of: table) }
      guard let self else { return }
      switch outcome {
      case .success(let d):
        dependents = d
        referencedBy(nil)
      case .failure(let error):
        toast?("\(error)")
      }
    }
  }

  /// The Table menu: the object's actions always, the editors in their lists
  func validateUserInterfaceItem(_ item: any NSValidatedUserInterfaceItem) -> Bool {
    switch item.action {
    case #selector(structureAdd(_:)): return availableStructureActions().contains(.add)
    case #selector(structureEdit(_:)): return availableStructureActions().contains(.edit)
    case #selector(structureDrop(_:)): return availableStructureActions().contains(.drop)
    case #selector(structureRename(_:)): return availableStructureActions().contains(.rename)
    case #selector(structurePrimaryKey(_:)): return availableStructureActions().contains(.primaryKey)
    case #selector(structureMove(_:)): return availableStructureActions().contains(.move)
    case #selector(structureCheck(_:)): return availableStructureActions().contains(.check)
    case #selector(truncateTable(_:)): return object.kind == .table
    case #selector(renameTable(_:)): return object.kind == .table || object.kind == .view
    case #selector(editInQuery(_:)): return object.kind != .table
    case #selector(newFunction(_:)): return connection.driver != .sqlite
    default: return true
    }
  }

  @objc func showData(_ sender: Any?) { setSubView(.data) }
  @objc func showStructure(_ sender: Any?) { setSubView(.structure) }
  @objc func showIndexes(_ sender: Any?) { setSubView(.indexes) }
  @objc func showForeignKeys(_ sender: Any?) { setSubView(.foreignKeys) }
  @objc func showInformation(_ sender: Any?) { setSubView(.information) }

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

  // MARK: NSMenuDelegate (the strip's actions button)

  func menuNeedsUpdate(_ menu: NSMenu) {
    guard menu === actionsButton.menu else { return }
    menu.removeAllItems()
    menu.addItem(withTitle: "Actions", action: nil, keyEquivalent: "")  // the pull-down's title
    for item in structureMenu().items {
      menu.addItem(item.copy() as! NSMenuItem)
    }
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

  func gridFollowForeignKey(_ grid: GridController) {
    followForeignKey(nil)
  }

  func gridReferencedBy(_ grid: GridController) {
    referencedBy(nil)
  }
}
