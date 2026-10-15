//
// Lace macOS frontend
// QueryViewController: the SQL editor and its results, as a tab of a connection
//
// ⌘↩ runs the statement at the cursor (or the selection), ⇧⌘↩ every
// statement, ⇧⌘T all of them inside BEGIN/COMMIT (ROLLBACK on error or
// cancel; refused while a transaction is open). Statement boundaries come
// from liblace (SQL.statement(at:), SQL.statements). The last statement is
// paged like a table when its shape allows (SQL.page, a count in the
// background, "N+ rows" until known); if the paged form fails it runs as
// written. Everything else runs once; the result's shape decides between
// rows and "N rows affected". Results of a plain single-table SELECT are
// editable when the table's key appears once in them.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class QueryViewController: NSViewController, GridDelegate, NSTextViewDelegate {
  enum RunMode {
    case atCursor, all, transaction
  }

  let connection: Connection
  let prefs: Prefs
  let model: RowModel
  let grid: GridController
  var onTitleChanged: (() -> Void)?
  /// A run included a statement that was not a read (DDL, DML): the table
  /// list may be stale
  var onTablesChanged: (() -> Void)?
  /// A statement that ran (as written), or the SQL of a verified write, for history
  var record: ((String) -> Void)?
  var toast: ((String) -> Void)? {
    didSet {
      grid.toast = toast
      edit.toast = toast
    }
  }
  /// For the tab title while the editor is empty
  var number = 1
  var isRunning: Bool { running }
  /// What the toolbar's status label says ("3 rows · 12 ms")
  var statusText: String { statusLabel.stringValue }

  private let editor = QueryTextView()
  private let editorScroll = NSScrollView()
  private let runButton = NSButton()
  private let runAllButton = NSButton()
  private let runTxButton = NSButton()
  private let stopButton = NSButton()
  private let statusLabel = NSTextField(labelWithString: "")
  private let banner = BannerView(frame: .zero)
  private let emptyLabel = NSTextField(labelWithString: "Results appear here. ⌘↩ runs the statement at the cursor.")
  private let messageLabel = NSTextField(wrappingLabelWithString: "")
  private let footerLabel = NSTextField(labelWithString: "")
  private let cellLabel = NSTextField(labelWithString: "")
  private let resultsPane = NSView()
  private let split = NSSplitView()
  private var splitPositioned = false
  private let edit: EditController

  private struct Script {
    var statements: [String]
    var mode: RunMode
    var ran = 0
    var elapsed: Double = 0
  }
  private var running = false
  private var baseSQL: String?
  private var wrap = false
  private var paged = false
  private var countPending = false
  private var sourceTable: String?
  private var schema: Schema?

  init(connection: Connection, prefs: Prefs) {
    self.connection = connection
    self.prefs = prefs
    model = RowModel(pageSize: prefs.pageSize)
    grid = GridController(model: model)
    edit = EditController(connection: connection, model: model, grid: grid)
    super.init(nibName: nil, bundle: nil)
    edit.uniqueNames = true
    edit.snapshotAll = true
    edit.confirmDelete = prefs.confirmDelete
    edit.onRowsDeleted = { [weak self] _ in self?.updateFooter() }
    edit.onWrite = { [weak self] sql in self?.record?(sql) }
    grid.delegate = self
    grid.allowsSorting = false
    grid.allowsFiltering = false
    grid.showRowNumbers = prefs.rowNumbers != .never
    model.fetch = { [weak self] _, page in self?.fetch(page: page) }
    model.onTotalChanged = { [weak self] in self?.updateFooter() }
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  // MARK: View

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 600))

    // Editor half
    editor.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
    editor.isRichText = false
    editor.allowsUndo = true
    editor.usesFindBar = true
    editor.isIncrementalSearchingEnabled = true
    editor.isAutomaticQuoteSubstitutionEnabled = false
    editor.isAutomaticDashSubstitutionEnabled = false
    editor.isAutomaticTextReplacementEnabled = false
    editor.isAutomaticSpellingCorrectionEnabled = false
    editor.isContinuousSpellCheckingEnabled = false
    editor.isGrammarCheckingEnabled = false
    editor.isVerticallyResizable = true
    editor.isHorizontallyResizable = false
    editor.autoresizingMask = [.width]
    editor.textContainer?.widthTracksTextView = true
    editor.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
    editor.minSize = NSSize(width: 0, height: 0)
    editor.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    editor.textContainerInset = NSSize(width: 4, height: 6)
    editor.delegate = self
    editor.onEscape = { [weak self] in self?.cancel() }
    editorScroll.documentView = editor
    editorScroll.hasVerticalScroller = true
    editorScroll.borderType = .noBorder
    editorScroll.translatesAutoresizingMaskIntoConstraints = false

    func toolButton(_ b: NSButton, _ symbol: String, _ tip: String, _ action: Selector) {
      b.image = NSImage(systemSymbolName: symbol, accessibilityDescription: tip)
      b.imagePosition = .imageOnly
      b.bezelStyle = .texturedRounded
      b.isBordered = false
      b.toolTip = tip
      b.target = self
      b.action = action
    }
    toolButton(runButton, "play.fill", "Run the statement at the cursor (⌘↩)", #selector(run(_:)))
    toolButton(runAllButton, "forward.fill", "Run every statement (⇧⌘↩)", #selector(runAll(_:)))
    toolButton(
      runTxButton, "checkmark.seal", "Run everything in one transaction (⇧⌘T)", #selector(runInTransaction(_:)))
    toolButton(stopButton, "stop.fill", "Stop (⌘.)", #selector(stop(_:)))
    stopButton.isEnabled = false
    statusLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    statusLabel.textColor = .secondaryLabelColor
    statusLabel.lineBreakMode = .byTruncatingTail
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let toolbar = NSStackView(views: [runButton, runAllButton, runTxButton, stopButton, spacer, statusLabel])
    toolbar.orientation = .horizontal
    toolbar.alignment = .centerY
    toolbar.spacing = 6
    toolbar.edgeInsets = NSEdgeInsets(top: 3, left: 8, bottom: 3, right: 8)

    let editorPane = NSStackView(views: [toolbar, SeparatorView(), editorScroll])
    editorPane.orientation = .vertical
    editorPane.alignment = .width
    editorPane.spacing = 0
    editorScroll.setContentHuggingPriority(.defaultLow, for: .vertical)

    // Results half
    emptyLabel.font = .systemFont(ofSize: 13)
    emptyLabel.textColor = .tertiaryLabelColor
    emptyLabel.alignment = .center
    emptyLabel.translatesAutoresizingMaskIntoConstraints = false
    messageLabel.font = .systemFont(ofSize: 13)
    messageLabel.textColor = .secondaryLabelColor
    messageLabel.alignment = .center
    messageLabel.translatesAutoresizingMaskIntoConstraints = false
    messageLabel.isHidden = true
    grid.scrollView.translatesAutoresizingMaskIntoConstraints = false
    grid.scrollView.isHidden = true
    resultsPane.addSubview(emptyLabel)
    resultsPane.addSubview(messageLabel)
    resultsPane.addSubview(grid.scrollView)
    NSLayoutConstraint.activate([
      emptyLabel.centerXAnchor.constraint(equalTo: resultsPane.centerXAnchor),
      emptyLabel.centerYAnchor.constraint(equalTo: resultsPane.centerYAnchor),
      messageLabel.centerXAnchor.constraint(equalTo: resultsPane.centerXAnchor),
      messageLabel.centerYAnchor.constraint(equalTo: resultsPane.centerYAnchor),
      messageLabel.widthAnchor.constraint(lessThanOrEqualTo: resultsPane.widthAnchor, constant: -40),
      grid.scrollView.leadingAnchor.constraint(equalTo: resultsPane.leadingAnchor),
      grid.scrollView.trailingAnchor.constraint(equalTo: resultsPane.trailingAnchor),
      grid.scrollView.topAnchor.constraint(equalTo: resultsPane.topAnchor),
      grid.scrollView.bottomAnchor.constraint(equalTo: resultsPane.bottomAnchor),
    ])
    resultsPane.setContentHuggingPriority(.defaultLow, for: .vertical)

    footerLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    footerLabel.lineBreakMode = .byTruncatingTail
    cellLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    cellLabel.textColor = .secondaryLabelColor
    cellLabel.lineBreakMode = .byTruncatingTail
    let footerSpacer = NSView()
    footerSpacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let footer = NSStackView(views: [footerLabel, footerSpacer, cellLabel])
    footer.orientation = .horizontal
    footer.alignment = .centerY
    footer.spacing = 8
    footer.edgeInsets = NSEdgeInsets(top: 3, left: 8, bottom: 3, right: 8)

    let resultsStack = NSStackView(views: [banner, resultsPane, SeparatorView(), footer])
    resultsStack.orientation = .vertical
    resultsStack.alignment = .width
    resultsStack.spacing = 0

    split.isVertical = false
    split.dividerStyle = .thin
    split.translatesAutoresizingMaskIntoConstraints = false
    split.addArrangedSubview(editorPane)
    split.addArrangedSubview(resultsStack)
    // The editor keeps its height when the window grows; the results take the rest
    split.setHoldingPriority(NSLayoutConstraint.Priority(251), forSubviewAt: 0)
    split.setHoldingPriority(NSLayoutConstraint.Priority(250), forSubviewAt: 1)
    editorPane.heightAnchor.constraint(greaterThanOrEqualToConstant: 80).isActive = true
    resultsStack.heightAnchor.constraint(greaterThanOrEqualToConstant: 80).isActive = true
    root.addSubview(split)
    NSLayoutConstraint.activate([
      split.leadingAnchor.constraint(equalTo: root.leadingAnchor),
      split.trailingAnchor.constraint(equalTo: root.trailingAnchor),
      split.topAnchor.constraint(equalTo: root.topAnchor),
      split.bottomAnchor.constraint(equalTo: root.bottomAnchor),
    ])
    view = root
    edit.hostView = root
    updateFooter()
  }

  override func viewDidLayout() {
    super.viewDidLayout()
    // The first layout with a height: about a third for the editor
    if !splitPositioned, split.bounds.height > 0 {
      splitPositioned = true
      split.setPosition(max(120, min(260, (split.bounds.height * 0.35).rounded())), ofDividerAt: 0)
    }
  }

  func tearDown() {
    model.fetch = nil
    if running {
      connection.cancel()
    }
  }

  // MARK: Text

  var text: String {
    get { editor.string }
    set {
      editor.string = newValue
      SQLHighlighter.highlight(editor, driver: connection.driver)
      onTitleChanged?()
    }
  }

  /// The first words of the first line, or "Query N"
  var tabTitle: String {
    let trimmed = editor.string.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let line = trimmed.split(whereSeparator: \.isNewline).first else { return "Query \(number)" }
    let words = line.split(whereSeparator: { $0 == " " || $0 == "\t" }).prefix(4).joined(separator: " ")
    return words.count > 28 ? String(words.prefix(28)) + "…" : words
  }

  func insertText(_ sql: String) {
    let range = editor.selectedRange()
    var insertion = sql
    let ns = editor.string as NSString
    if range.location > 0, ns.character(at: range.location - 1) != 0x0A {
      insertion = "\n" + sql
    }
    if editor.shouldChangeText(in: range, replacementString: insertion) {
      editor.replaceCharacters(in: range, with: insertion)
      editor.didChangeText()
    }
    focusEditor()
  }

  func focusEditor() {
    view.window?.makeFirstResponder(editor)
  }

  func focusResults() {
    if model.count > 0 {
      grid.focusCell(row: 0, column: 0)
    }
  }

  func textDidChange(_ notification: Notification) {
    SQLHighlighter.highlight(editor, driver: connection.driver)
    onTitleChanged?()
  }

  // MARK: Results pane

  private enum Pane {
    case empty, grid, message
  }

  private func show(_ pane: Pane) {
    emptyLabel.isHidden = pane != .empty
    grid.scrollView.isHidden = pane != .grid
    messageLabel.isHidden = pane != .message
  }

  private func setRunning(_ on: Bool) {
    running = on
    runButton.isEnabled = !on
    runAllButton.isEnabled = !on
    runTxButton.isEnabled = !on
    stopButton.isEnabled = on
  }

  private func updateFooter() {
    guard !model.columns.isEmpty else {
      footerLabel.stringValue = ""
      cellLabel.stringValue = ""
      return
    }
    var s: String
    let total = model.total
    s = model.totalKnown ? Format.plural(total, "row") : "\(Format.count(total))+ rows"
    let selected = grid.selectedCount
    if selected > 0 {
      s += " · \(Format.count(selected)) selected"
    }
    if let sourceTable, grid.isEditable {
      s += " · editing \(sourceTable)"
    } else if let why = grid.readOnlyReason {
      s += " · read only: \(why)"
    }
    footerLabel.stringValue = s
  }

  // MARK: Paging and editability

  private func fetch(page: Int) {
    guard paged, let baseSQL else { return }
    let size = model.pageSize
    let sql = SQL.page(of: baseSQL, wrap: wrap, limit: size, offset: page * size)
    let generation = model.generation
    let connection = self.connection
    Task { [weak self] in
      let outcome = await Result { try await connection.read(sql) }
      guard let self else { return }
      switch outcome {
      case .success(let result):
        model.pageLoaded(generation: generation, page: page, result: result)
        if generation == model.generation, !model.totalKnown {
          startCount()
        }
      case .failure(let error):
        model.pageFailed(generation: generation, page: page)
        if generation == model.generation, (error as? LaceError)?.isCancelled != true {
          banner.show("\(error)")
        }
      }
      updateFooter()
    }
  }

  private func startCount() {
    guard !countPending, let baseSQL else { return }
    countPending = true
    let generation = model.generation
    let sql = SQL.count(of: baseSQL)
    let connection = self.connection
    Task { [weak self] in
      let outcome = await Result { try await connection.count(sql: sql) }
      guard let self else { return }
      countPending = false
      guard generation == model.generation, case .success(let n) = outcome, n >= 0 else { return }
      model.setTotal(Int(n), known: true, approximate: false)
    }
  }

  private func detectSourceTable(_ sql: String) {
    sourceTable = SQL.simpleSelectTable(sql, driver: connection.driver)
    schema = nil
    edit.schema = nil
    edit.table = nil
    if let table = sourceTable {
      let generation = model.generation
      let connection = self.connection
      Task { [weak self] in
        let loaded = try? await connection.schema(of: table)
        guard let self, generation == model.generation else { return }
        schema = loaded
        if let loaded {
          grid.showRowNumbers = prefs.rowNumbers(for: loaded)
        }
        setEditableState()
      }
    }
    setEditableState()
  }

  private func setEditableState() {
    var editable = false
    var why: String?
    if sourceTable == nil {
      why = "not a single-table SELECT"
    } else if let schema {
      if schema.primaryKey.isEmpty {
        why = "the table has no primary key"
      } else if model.count > 0, let (page, index) = model.rowResult(0) {
        do {
          _ = try RowKey.build(for: index, in: page, schema: schema, uniqueNames: true)
          editable = true
        } catch RowKeyProblem.unusable {
          editable = true  // other rows may be fine; refused per row
        } catch {
          why = "every primary key column must appear once in the results"
        }
      } else {
        editable = true
      }
    } else {
      why = "the table's structure could not be read"
    }
    grid.isEditable = editable
    grid.readOnlyReason = why
    edit.schema = schema
    edit.table = sourceTable
    updateFooter()
  }

  // MARK: Running

  private func statements(for mode: RunMode) -> [String] {
    let text = editor.string
    let driver = connection.driver
    if mode == .atCursor {
      let selection = editor.selectedRange()
      if selection.length > 0, let range = Range(selection, in: text) {
        let selected = text[range].trimmingCharacters(in: .whitespacesAndNewlines)
        return selected.isEmpty ? [] : [selected]
      }
      guard let cursor = Range(NSRange(location: selection.location, length: 0), in: text)?.lowerBound,
        let range = SQL.statement(at: cursor, in: text, driver: driver)
      else { return [] }
      return [String(text[range])]
    }
    return SQL.statements(in: text, driver: driver).map { String(text[$0]) }
  }

  func run(mode: RunMode) {
    if running {
      toast?("A statement is running (⌘. stops it)")
      return
    }
    let statements = statements(for: mode)
    if statements.isEmpty {
      toast?("Nothing to run")
      return
    }
    if mode == .transaction, connection.transactionState != .none {
      toast?("A transaction is already open; COMMIT or ROLLBACK it first")
      return
    }
    banner.hide()
    setRunning(true)
    statusLabel.stringValue = "Running…"
    Task { [weak self] in
      guard let self else { return }
      await runScript(Script(statements: statements, mode: mode))
    }
  }

  private func runScript(_ initial: Script) async {
    var script = initial
    var began = false
    var failed = false
    var cancelled = false
    let wrote = script.statements.contains { !SQL.isReadOnly($0, driver: connection.driver) }
    defer {
      finish(script, failed: failed, cancelled: cancelled)
      if wrote {
        onTablesChanged?()
      }
    }

    if script.mode == .transaction {
      do {
        _ = try await connection.exec("BEGIN")
        began = true
      } catch {
        banner.show("\(error)")
        failed = true
        return
      }
    }
    for (i, sql) in script.statements.enumerated() {
      let last = i == script.statements.count - 1
      let shape = SQL.shape(sql, driver: connection.driver)
      let started = Date()
      var outcome: Result<Void, Error>
      if last && shape.isPageable {
        // Only the last statement's rows are shown, and only a plain read is
        // paged; if the paged form is refused the statement runs as written
        outcome = await startPaged(SQL.base(of: sql, shape: shape), wrap: shape.needsWrap)
        if case .failure(let error) = outcome, (error as? LaceError)?.isCancelled != true {
          outcome = await runWhole(sql, show: true)
        }
      } else {
        outcome = await runWhole(sql, show: last)
      }
      script.elapsed += Date().timeIntervalSince(started)
      switch outcome {
      case .success:
        script.ran += 1
        record?(sql)
      case .failure(let error):
        banner.show("\(error)")
        if (error as? LaceError)?.isCancelled == true {
          cancelled = true
        } else {
          failed = true
        }
        if began {
          do {
            _ = try await connection.exec("ROLLBACK")
            toast?("Rolled back")
          } catch {
            banner.show("\(error)")
          }
        }
        return
      }
    }
    if began {
      do {
        _ = try await connection.exec("COMMIT")
        toast?("Committed \(Format.plural(script.ran, "statement"))")
      } catch {
        banner.show("\(error)")
        failed = true
      }
    }
  }

  private func finish(_ script: Script, failed: Bool, cancelled: Bool) {
    let el = Format.elapsed(script.elapsed)
    if cancelled {
      statusLabel.stringValue = "Cancelled after \(Format.plural(script.ran, "statement")) · \(el)"
    } else if failed {
      statusLabel.stringValue = "Failed after \(Format.plural(script.ran, "statement")) · \(el)"
    } else if script.statements.count > 1 {
      statusLabel.stringValue = "\(Format.plural(script.ran, "statement")) · \(el)"
    } else if paged {
      statusLabel.stringValue = "First page in \(el)"
    }
    setRunning(false)
  }

  /// Run as written; show rows or the affected count when `show`
  private func runWhole(_ sql: String, show: Bool) async -> Result<Void, Error> {
    let started = Date()
    do {
      let result = try await connection.exec(sql)
      if show {
        showWholeResult(result, sql: sql, elapsed: Date().timeIntervalSince(started))
      }
      return .success(())
    } catch {
      return .failure(error)
    }
  }

  /// Show a pageable statement: reset the model and load its first page
  private func startPaged(_ base: String, wrap: Bool) async -> Result<Void, Error> {
    baseSQL = base
    self.wrap = wrap
    paged = true
    countPending = false
    model.reset()
    let generation = model.generation
    model.notePending(page: 0)
    let sql = SQL.page(of: base, wrap: wrap, limit: model.pageSize, offset: 0)
    do {
      let result = try await connection.read(sql)
      guard generation == model.generation else { return .success(()) }
      model.pageLoaded(generation: generation, page: 0, result: result)
      grid.measureColumns()
      show(.grid)
      detectSourceTable(base)
      if !model.totalKnown {
        startCount()
      }
      updateFooter()
      return .success(())
    } catch {
      model.pageFailed(generation: generation, page: 0)
      paged = false
      return .failure(error)
    }
  }

  private func showWholeResult(_ result: ResultSet, sql: String, elapsed: Double) {
    paged = false
    baseSQL = nil
    model.reset()
    if result.hasRows {
      let n = result.rowCount
      model.setPageSize(max(n, 1))
      model.pageLoaded(generation: model.generation, page: 0, result: result)
      model.setTotal(n, known: true, approximate: false)
      grid.measureColumns()
      show(.grid)
      statusLabel.stringValue =
        "\(Format.plural(n, "row"))\(result.truncated ? " (truncated by the daemon's limit)" : "") · \(Format.elapsed(elapsed))"
      detectSourceTable(sql)
    } else {
      let affected = result.rowsAffected
      let message =
        affected >= 0
        ? "\(Format.plural(Int(affected), "row")) affected · \(Format.elapsed(elapsed))"
        : "Done · \(Format.elapsed(elapsed))"
      messageLabel.stringValue = message
      statusLabel.stringValue = message
      show(.message)
      sourceTable = nil
      schema = nil
      grid.isEditable = false
      grid.readOnlyReason = nil
    }
    updateFooter()
  }

  func cancel() {
    guard running else { return }
    connection.cancel()
    statusLabel.stringValue = "Cancelling…"
  }

  func refresh() {
    guard paged, baseSQL != nil, !running else { return }
    model.invalidate()
    startCount()
  }

  // MARK: Actions (responder chain)

  @objc func run(_ sender: Any?) { run(mode: .atCursor) }
  @objc func runAll(_ sender: Any?) { run(mode: .all) }
  @objc func runInTransaction(_ sender: Any?) { run(mode: .transaction) }
  @objc func stop(_ sender: Any?) { cancel() }
  @objc func refresh(_ sender: Any?) { refresh() }
  @objc func deleteRows(_ sender: Any?) { edit.deleteSelected() }

  @objc func gotoRow(_ sender: Any?) {
    guard model.total > 0 else { return }
    Task { [weak self] in
      guard let self else { return }
      guard let text = await Alerts.prompt("Go to Row", "Row number", action: "Go", window: view.window),
        let n = Int(text.trimmingCharacters(in: .whitespaces)), n >= 1
      else { return }
      grid.scrollTo(row: min(n - 1, max(model.total - 1, 0)))
    }
  }

  // MARK: GridDelegate

  func gridSortChanged(_ grid: GridController) {}

  func grid(_ grid: GridController, editCell row: Int, column: Int, text: String?) {
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
    // Query columns carry no declared type: show the value's
    let value = model.value(row: row, column: column)
    let kind = value.map { $0.isNull ? "NULL" : $0.type.name } ?? c.type.name
    cellLabel.stringValue = "Row \(Format.count(row + 1)) · \(c.name) · \(kind)"
  }

  func gridSelectionChanged(_ grid: GridController) {
    updateFooter()
  }

  func gridRefresh(_ grid: GridController) {
    refresh()
  }

  func grid(_ grid: GridController, filterColumn name: String, value: String?) {}
  func gridAddRow(_ grid: GridController) {}
}

/// The SQL editor: Esc stops a running statement instead of completing a word
final class QueryTextView: NSTextView {
  var onEscape: (() -> Void)?

  override func cancelOperation(_ sender: Any?) {
    onEscape?()
  }

  override func complete(_ sender: Any?) {
    // No word completion popup on Esc
  }
}
