//
// Lace macOS frontend
// ScriptDriver: drives the app headlessly from $LACE_MACOS_SCRIPT (like the
// GTK frontend's LACE_GTK_SCRIPT), for smoke tests without a hand on the
// mouse. Only active when the variable is set.
//
// Commands, separated by ';' (a ';' inside SQL is written '\;'):
//   wait MS                  pause
//   connect URL              a connection string or SQLite path, as typed
//   saved-add NAME URL       save URL as a connection named NAME (its password not kept)
//   saved-add-password NAME URL   the same, the URL's password saved (in the
//                            Keychain when the preference is on)
//   folder-add NAME [PARENT] a folder of saved connections (in PARENT, by name)
//   keychain-refuse on|off   the Keychain stand-in hands no password back, as
//                            when the person denies the prompt
//   connect-saved NAME       connect through the saved connection named NAME
//   close-connection         close the current strip entry (⇧⌘W)
//   close-tab                ⌘W: the current inner tab (the last one follows
//                            keep_connection_tab), else the current entry
//   new-connection-tab       ⌘T: a connection form after the current entry
//   drag-entry FROM GAP      drop the window's FROM-th entry (0-based) on its
//                            strip at gap GAP (0: before the first tab), as a drag does
//   drag-entry-to N GAP      drop the current entry on window N's strip at GAP
//   drag-tab FROM GAP        the same for the current connection's inner tabs
//   expect-entry-titles A|B  the window's strip titles, in order
//   expect-tab-titles A|B    the current connection's inner tab titles, in order
//   expect-saved N           N saved connections
//   expect-recent N [TEXT]   N recent connections, the newest containing TEXT
//   expect-paging METHOD     the table's last page was read by offset,
//                            keyset or keyset-backward
//   save-session | restore-session   write session.json / reopen it
//   expect-connections N     N connection tabs in the workspace
//   expect-tabs N            N inner tabs in the current connection
//   table NAME               open a table (kept, not a preview) and wait for
//                            its first page and count
//   object KIND NAME         open a view, routine or trigger from the sidebar's list
//   subview NAME             show a sub-view of the table tab (data, structure,
//                            indexes, foreign_keys, information) and wait for its rows
//   expect-subview NAME      the table tab shows that sub-view
//   expect-row TEXT          a row of the sub-view's list contains TEXT
//   expect-sidebar TEXT      a sidebar row is TEXT ("▾ VIEWS", a name)
//   sidebar-toggle KIND      fold or unfold a sidebar section (view, routine, trigger)
//   follow                   follow the focused cell's foreign key
//   referenced-by            open the first table referencing the focused row
//   query SQL                a new query tab holding SQL (a literal \n is a newline)
//   run | run-all | run-tx   run it (cursor / all / in a transaction) and wait
//   cell R C                 focus a cell of the current grid
//   edit TEXT | null | empty write the focused cell and wait
//   delete                   delete the selected rows (no confirmation)
//   add-row COL=TEXT ...     insert a row into the current table (TEXT null
//                            or empty write those; other columns are left out)
//   filter COL OP VALUE      add a filter (OP is a FilterOp key: eq, contains, null, ...)
//   sort COL [desc]          sort the table
//   expect-total N           the current grid's total (waits for the count)
//   expect-cell R C TEXT     a cell's display text (waits for its page)
//   expect-status TEXT       the query toolbar's status contains TEXT
//   expect-history N [TEXT]  the connection's history holds N entries, the
//                            last one containing TEXT
//   sheet add-row|history|preferences   open that sheet or window
//   dismiss-sheet            close it again
//   answer TEXT              what the next name prompt (rename, new table, the
//                            counter, a comment) answers; empty: cancel
//   ddl ACTION [ARG]         a table action or editor: rename-table NAME,
//                            truncate, drop, new-table NAME, new-view NAME,
//                            new-function NAME, edit-definition, autoinc N,
//                            comment TEXT, maintenance analyze|vacuum|optimize,
//                            action add|edit|drop|rename|primary-key|move|check
//                            (the sub-view's editor on the selected row).
//                            Confirmations are answered yes; the SQL is logged.
//   select-row TEXT          select the sub-view list's row containing TEXT
//   ddl-field ID VALUE       a field of the open editor sheet (name, type, ...)
//   ddl-save                 submit the editor and wait for it to close
//   expect-no-row TEXT       no row of the sub-view's list contains TEXT
//   expect-table NAME        the current table tab shows NAME
//   expect-no-sidebar TEXT   no sidebar row is TEXT (waits for a reload)
//   run-async                run the statement at the cursor without waiting
//   cancel                   cancel the connection's running statement
//   expect-banner | expect-no-banner   the lock banner shows / is gone (waits)
//   shot NAME                NAME.png of the window's content into $LACE_MACOS_SHOTS
//                            (NAME-sheet.png of an open sheet or the preferences)
//   quit                     exit 0, or 1 when an expectation failed
//   terminate                quit through NSApplication, as ⌘Q does (1 on failures)
//   close-window             close the workspace window, as the close button does
//   expect-session-windows N session.json holds N workspaces (windows)
//   expect-entries N         N strip entries in the current window (forms included)
//   expect-entry KIND        the selected entry is a form, a connection, or none
//   expect-title TEXT        the selected entry's strip title
//   expect-windows N         N workspace windows
//   new-window               a new window, as ⌘N (the script stays on its window)
//   window N                 the window made N-th (1, 2, ...) becomes the script's
//                            (when the script's window closes, the newest open one does)
//   expect-open-connections N   the app's open database connections
//   expect-badge N           N strip entries of the window show a transaction badge
//   expect-alert TEXT        an alert over the window contains TEXT (waits)
//   expect-no-alert          no alert over the window (after a moment)
//   answer-alert go|cancel   press the alert's action or Cancel button
//   form-uri TEXT            type into the current form's URI line and apply it
//   form-field KEY VALUE     set a form field (name, driver, file, host, port,
//                            database, user, password, save-password, ssh,
//                            ssh-host, ssh-port, ssh-user, ssh-key,
//                            ssh-password, save-ssh-password; true/false for boxes)
//   form-folder NAME         the form's Folder pop-up ("" for the top level)
//   form-pick NAME           highlight a saved connection (or Recent entry) as a click does
//   form-action ACTION       open, background, new-window, to-window:N, save,
//                            save-as-new, test, clear (waits for a connect)
//   expect-field KEY [VALUE] the form's field holds VALUE (none: empty)
//   expect-form-message TEXT the form's message line contains TEXT
//   expect-uri-masked        the URI line shows ••••• and not the typed password
//   expect-open-uses TEXT    the string Open would connect with contains TEXT
//                            (a Keychain password; never logged)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
final class ScriptDriver {
  static var isRequested: Bool {
    !(ProcessInfo.processInfo.environment["LACE_MACOS_SCRIPT"] ?? "").isEmpty
  }

  /// Start the driver on the given window if $LACE_MACOS_SCRIPT is set
  static func startIfRequested(in window: WorkspaceWindowController) {
    guard let script = ProcessInfo.processInfo.environment["LACE_MACOS_SCRIPT"], !script.isEmpty else { return }
    let driver = ScriptDriver(window: window, script: script)
    Task { await driver.run() }
  }

  /// Expectations that failed so far (the app's exit status when it quits by itself)
  private(set) static var failureCount = 0

  /// The window the commands act on (`window N` changes it)
  private var windowController: WorkspaceWindowController
  private var workspace: WorkspaceViewController { windowController.workspace }
  private let commands: [String]
  private var checks = 0
  private var failures = 0 {
    didSet { Self.failureCount = failures }
  }
  private let timeout: Double = 30
  /// The answers queued for the name prompts (the `answer` command)
  nonisolated(unsafe) private static var promptAnswers: [String] = []

  /// The next scripted answer, taken; nil when the script gave none
  static func takePromptAnswer() -> String? {
    promptAnswers.isEmpty ? nil : promptAnswers.removeFirst()
  }

  private init(window: WorkspaceWindowController, script: String) {
    windowController = window
    commands = script.replacingOccurrences(of: "\\;", with: "\u{1}")
      .split(separator: ";").map {
        $0.replacingOccurrences(of: "\u{1}", with: ";").trimmingCharacters(in: .whitespacesAndNewlines)
      }.filter { !$0.isEmpty }
    // Scripts never answer dialogs; errors they cause go to the log. The
    // Keychain would ask too: passwords stay in the private HOME's file,
    // unless the run named a stand-in file for the Keychain's items
    LaceApp.shared.prefs.confirmDelete = false
    if !Keychain.isStandIn {
      LaceApp.shared.prefs.keychainPasswords = false
    }
    DdlRunner.autoConfirm = true
    DdlRunner.logger = { line in FileHandle.standardError.write(Data("script: \(line)\n".utf8)) }
    Alerts.logger = { title, message in
      FileHandle.standardError.write(Data("script: alert \(title): \(message)\n".utf8))
    }
  }

  private func log(_ s: String) {
    FileHandle.standardError.write(Data("script: \(s)\n".utf8))
  }

  private func check(_ ok: Bool, _ what: String) {
    checks += 1
    if !ok {
      failures += 1
      log("FAIL \(what)")
    }
  }

  private var connection: ConnectionViewController? { workspace.current }
  private var table: TableViewController? { connection?.content.currentController as? TableViewController }
  private var query: QueryViewController? { connection?.content.currentController as? QueryViewController }
  private var grid: GridController? { table?.grid ?? query?.grid }
  private var model: RowModel? { grid?.model }

  /// The newest alert over the script's window containing text
  private func alert(containing text: String) -> Alerts.Shown? {
    Alerts.shown.last { $0.window === windowController.window && (text.isEmpty || $0.text.contains(text)) }
  }

  /// Poll a condition on the main actor until it holds or the timeout passes
  /// (quiet: the caller reports the failure itself; a shorter timeout)
  private func waitUntil(_ what: String, quiet: Bool = false, _ condition: () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(quiet ? 5 : timeout)
    while !condition() {
      if Date() > deadline {
        if !quiet {
          check(false, "timed out waiting for \(what)")
        }
        return false
      }
      try? await Task.sleep(for: .milliseconds(25))
    }
    return true
  }

  private func settle() async {
    _ = await waitUntil("the view to settle") {
      if let table { return !table.isLoading && !table.connection.isBusy }
      if let query { return !query.isRunning && !query.connection.isBusy }
      return connection?.connection.isBusy != true
    }
    try? await Task.sleep(for: .milliseconds(50))
  }

  func run() async {
    log("\(commands.count) commands")
    for command in commands {
      // The script's window closed (its last entry did): on with the newest
      if !LaceApp.shared.workspaces.contains(where: { $0 === windowController }), let newest = LaceApp.shared.workspaces.last {
        windowController = newest
      }
      let parts = command.split(separator: " ", maxSplits: 1).map(String.init)
      let verb = parts[0]
      let arg = parts.count > 1 ? parts[1] : ""
      let words = arg.split(separator: " ").map(String.init)
      switch verb {
      case "wait":
        try? await Task.sleep(for: .milliseconds(Int(arg) ?? 100))
      case "connect":
        let cs: String
        if arg.contains("://") {
          cs = arg
        } else {
          guard case .success(let built) = Driver.sqliteConnectionString(path: arg) else {
            check(false, "connect: bad path \(arg)")
            continue
          }
          cs = built
        }
        let vc = await workspace.open(connectionString: cs)
        check(vc != nil, "connect \(arg)")
      case "saved-add", "saved-add-password":
        guard words.count >= 2, var c = SavedConnection(connectionString: words[1...].joined(separator: " ")) else {
          check(false, "\(verb): bad arguments")
          continue
        }
        c.name = words[0]
        c.savePassword = verb == "saved-add-password" && c.password != nil
        do {
          try LaceApp.shared.store(c)
        } catch {
          check(false, "saved-add: \(error)")
        }
      case "connect-saved":
        guard let c = LaceApp.shared.saved.entries.first(where: { $0.connection.name == arg })?.connection else {
          check(false, "connect-saved: no saved connection \(arg)")
          continue
        }
        let vc = await workspace.open(connectionString: c.connectionString(), saved: c)
        check(vc != nil, "connect-saved \(arg)")
      case "close-tab":
        check(workspace.currentEntry != nil, "close-tab: nothing to close")
        workspace.closeTab(nil)
        try? await Task.sleep(for: .milliseconds(100))
        await settle()
      case "new-connection-tab":
        workspace.newConnectionTab(nil)
      case "drag-entry", "drag-tab":
        guard words.count == 2, let from = Int(words[0]), let gap = Int(words[1]) else {
          check(false, "\(verb): bad arguments")
          continue
        }
        guard let strip = verb == "drag-entry" ? workspace.stripView : connection?.content.stripView,
          from >= 0, from < strip.itemIDs.count
        else {
          check(false, "\(verb) \(arg): no such tab")
          continue
        }
        strip.drop(strip.itemIDs[from], from: strip, at: strip.point(forGap: gap))
        try? await Task.sleep(for: .milliseconds(100))
      case "drag-entry-to":
        guard words.count == 2, let n = Int(words[0]), let gap = Int(words[1]),
          let to = LaceApp.shared.workspace(number: n), let id = workspace.currentEntry?.id
        else {
          check(false, "drag-entry-to \(arg): no such window or no entry")
          continue
        }
        let strip = to.workspace.stripView
        check(
          strip.drop(id, from: workspace.stripView, at: strip.point(forGap: gap)),
          "drag-entry-to \(arg): the drop was refused")
        try? await Task.sleep(for: .milliseconds(300))
      case "expect-entry-titles":
        let got = workspace.stripView.items.map(\.title).joined(separator: "|")
        check(got == arg, "expect-entry-titles \(arg): got \(got)")
      case "expect-tab-titles":
        let got = connection?.content.stripView.items.map(\.title).joined(separator: "|") ?? "<no connection>"
        check(got == arg, "expect-tab-titles \(arg): got \(got)")
      case "close-connection":
        guard let connection else {
          check(false, "close-connection: no connection")
          continue
        }
        workspace.closeConnection(id: connection.id)
        try? await Task.sleep(for: .milliseconds(100))
      case "save-session":
        Session.save()
      case "restore-session":
        let ok = await Session.restore(into: LaceApp.shared.workspaces.first)
        check(ok, "restore-session: nothing restored")
        await settle()
      case "expect-connections":
        let n = workspace.connectionControllers.count
        check(n == Int(arg), "expect-connections \(arg): got \(n)")
      case "expect-tabs":
        let n = connection?.content.tabControllers.count ?? -1
        check(n == Int(arg), "expect-tabs \(arg): got \(n)")
      case "expect-recent":
        let recents = LaceApp.shared.recents.entries
        check(recents.count == Int(words.first ?? ""), "expect-recent \(arg): got \(recents.count)")
        if words.count > 1 {
          let want = words[1...].joined(separator: " ")
          check(recents.first?.url.contains(want) == true, "expect-recent newest \(want): got \(recents.first?.url ?? "<none>")")
        }
      case "expect-paging":
        check(table?.lastPaging == arg, "expect-paging \(arg): got \(table?.lastPaging ?? "<no table>")")
      case "expect-saved":
        check(LaceApp.shared.saved.entries.count == Int(arg), "expect-saved \(arg): got \(LaceApp.shared.saved.entries.count)")
      case "table":
        guard let connection else {
          check(false, "table: no connection")
          continue
        }
        let vc = connection.content.openTable(arg, preview: false)
        _ = await waitUntil("table \(arg) to load") { !vc.isLoading && (vc.model.totalKnown || vc.model.count > 0) }
        await settle()
      case "object":
        guard let connection, words.count >= 2, let kind = ObjectKind(key: words[0]) else {
          check(false, "object: bad arguments or no connection")
          continue
        }
        let name = words[1...].joined(separator: " ")
        _ = await waitUntil("the sidebar to list \(name)") { connection.sidebar.rowTexts.contains(name) }
        check(connection.sidebar.open(kind: kind, name: name), "object \(arg): not in the sidebar")
        await settle()
      case "subview":
        guard let table, let sv = SubView(key: arg) else {
          check(false, "subview: bad arguments or no table")
          continue
        }
        table.setSubView(sv)
        if sv != .data {
          _ = await waitUntil("the \(arg) rows") { !table.listRows.isEmpty && table.listRows != ["Loading…"] }
        }
        await settle()
      case "expect-subview":
        check(table?.subview.key(for: table?.object.kind ?? .table) == arg, "expect-subview \(arg): got \(table?.subview.key(for: table?.object.kind ?? .table) ?? "<no table>")")
      case "expect-row":
        guard let table else {
          check(false, "expect-row: no table")
          continue
        }
        let ok = await waitUntil("a row with \(arg)") { table.listRows.contains { $0.contains(arg) } }
        check(ok, "expect-row \(arg): rows \(table.listRows)")
      case "expect-sidebar":
        guard let connection else {
          check(false, "expect-sidebar: no connection")
          continue
        }
        let ok = await waitUntil("sidebar row \(arg)") { connection.sidebar.rowTexts.contains(arg) }
        check(ok, "expect-sidebar \(arg): rows \(connection.sidebar.rowTexts)")
      case "sidebar-toggle":
        guard let connection, let kind = ObjectKind(key: arg) else {
          check(false, "sidebar-toggle: bad arguments")
          continue
        }
        connection.sidebar.toggle(kind)
      case "follow":
        guard let grid else {
          check(false, "follow: no grid")
          continue
        }
        grid.delegate?.gridFollowForeignKey(grid)
        try? await Task.sleep(for: .milliseconds(100))
        await settle()
      case "referenced-by":
        guard let table else {
          check(false, "referenced-by: no table")
          continue
        }
        let deps = try? await table.connection.dependents(of: table.table)
        guard let first = deps?.entries.first else {
          check(false, "referenced-by: no table references \(table.table)")
          continue
        }
        switch Follow.filters(for: first, grid: table.grid) {
        case .failure(let why): check(false, "referenced-by: \(why.text)")
        case .success(let t): connection?.content.openFiltered(t.table, filters: t.filters)
        }
        await settle()
      case "query":
        guard let connection else {
          check(false, "query: no connection")
          continue
        }
        connection.content.newQuery(text: arg.replacingOccurrences(of: "\\n", with: "\n"))
      case "run", "run-all", "run-tx":
        guard let query else {
          check(false, "\(verb): no query view")
          continue
        }
        query.run(mode: verb == "run" ? .atCursor : verb == "run-all" ? .all : .transaction)
        _ = await waitUntil("the query to finish") { !query.isRunning }
        await settle()
      case "cell":
        guard let grid, words.count == 2, let r = Int(words[0]), let c = Int(words[1]) else {
          check(false, "cell: bad arguments or no grid")
          continue
        }
        grid.focusCell(row: r, column: c)
        _ = await waitUntil("row \(r) to load") { grid.model.isLoaded(row: r) }
      case "edit", "null", "empty":
        guard let grid, let cell = grid.focusedCell else {
          check(false, "\(verb): no focused cell")
          continue
        }
        let text: String? = verb == "null" ? nil : verb == "empty" ? "" : arg
        grid.delegate?.grid(grid, editCell: cell.row, column: cell.column, text: text)
        try? await Task.sleep(for: .milliseconds(50))
        await settle()
      case "delete":
        guard let grid else {
          check(false, "delete: no grid")
          continue
        }
        grid.delegate?.gridDeleteRows(grid)
        try? await Task.sleep(for: .milliseconds(100))
        await settle()
      case "add-row":
        guard let table else {
          check(false, "add-row: no table")
          continue
        }
        let fields: [(column: String, text: String?)] = words.compactMap { word in
          guard let eq = word.firstIndex(of: "=") else { return nil }
          let value = String(word[word.index(after: eq)...])
          let text: String? = value == "null" ? nil : value == "empty" ? "" : value
          return (String(word[..<eq]), text)
        }
        let ok = await table.insertRow(fields)
        check(ok, "add-row \(arg)")
        await settle()
      case "expect-history":
        guard let connection, let n = Int(words.first ?? "") else {
          check(false, "expect-history: bad arguments or no connection")
          continue
        }
        let entries = connection.history.entries
        check(entries.count == n, "expect-history \(n): got \(entries.count)")
        if words.count > 1 {
          let want = words[1...].joined(separator: " ")
          let last = entries.last?.sql ?? "<none>"
          check(last.contains(want), "expect-history last \(want): got \(last)")
        }
      case "filter":
        guard let table, words.count >= 2 else {
          check(false, "filter: bad arguments or no table")
          continue
        }
        let op = FilterOp(key: words[1])
        let value = words.count > 2 ? words[2...].joined(separator: " ") : ""
        table.addFilter(Filter(column: words[0], op: op, value: value))
        await settle()
      case "sort":
        guard let table, let column = words.first else {
          check(false, "sort: bad arguments or no table")
          continue
        }
        table.setSorts([SortSpec(column: column, descending: words.count > 1 && words[1] == "desc")])
        await settle()
      case "expect-total":
        guard let model, let n = Int(arg) else {
          check(false, "expect-total: bad arguments or no grid")
          continue
        }
        _ = await waitUntil("the total") { model.totalKnown }
        check(model.total == n, "expect-total \(n): got \(model.total)")
      case "expect-cell":
        guard let model, words.count >= 3, let r = Int(words[0]), let c = Int(words[1]) else {
          check(false, "expect-cell: bad arguments or no grid")
          continue
        }
        let want = words[2...].joined(separator: " ")
        model.request(row: r)
        _ = await waitUntil("row \(r)") { model.isLoaded(row: r) }
        let got = model.value(row: r, column: c)?.displayText() ?? "<missing>"
        check(got == want, "expect-cell \(r) \(c) \(want): got \(got)")
      case "expect-status":
        guard let query else {
          check(false, "expect-status: no query view")
          continue
        }
        check(query.statusText.contains(arg), "expect-status \(arg): got \(query.statusText)")
      case "sheet":
        switch arg {
        case "add-row":
          guard let table else {
            check(false, "sheet add-row: no table")
            continue
          }
          table.addRow(nil)
        case "history":
          guard let connection else {
            check(false, "sheet history: no connection")
            continue
          }
          connection.showHistory(nil)
        case "preferences":
          LaceApp.shared.showPreferences()
        default:
          check(false, "sheet: unknown \(arg)")
        }
        try? await Task.sleep(for: .milliseconds(300))
      case "dismiss-sheet":
        if let sheet = workspace.view.window?.attachedSheet {
          workspace.view.window?.endSheet(sheet)
        }
        for w in NSApp.windows where w.title == "Preferences" {
          w.close()
        }
        try? await Task.sleep(for: .milliseconds(100))
      case "answer":
        Self.promptAnswers.append(arg)
      case "ddl":
        guard let table else {
          check(false, "ddl: no table tab")
          continue
        }
        let what = words.first ?? ""
        let rest = words.count > 1 ? words[1...].joined(separator: " ") : ""
        switch what {
        case "rename-table":
          Self.promptAnswers.append(rest)
          table.renameTable(nil)
        case "truncate": table.truncateTable(nil)
        case "drop": table.dropTable(nil)
        case "new-table":
          Self.promptAnswers.append(rest)
          table.newTable(nil)
        case "new-view":
          Self.promptAnswers.append(rest)
          table.newView(nil)
        case "new-function":
          Self.promptAnswers.append(rest)
          table.newFunction(nil)
        case "edit-definition": table.editInQuery(nil)
        case "autoinc":
          Self.promptAnswers.append(rest)
          table.setAutoincrement()
        case "comment":
          Self.promptAnswers.append(rest)
          table.setComment()
        case "maintenance":
          table.maintenance(rest == "vacuum" ? .vacuum : rest == "optimize" ? .optimize : .analyze)
        case "action":
          let actions: [String: StructureAction] = [
            "add": .add, "edit": .edit, "drop": .drop, "rename": .rename, "primary-key": .primaryKey, "move": .move,
            "check": .check,
          ]
          guard let a = actions[rest] else {
            check(false, "ddl action: unknown \(rest)")
            continue
          }
          table.runStructureAction(a)
        default:
          check(false, "ddl: unknown \(what)")
        }
        try? await Task.sleep(for: .milliseconds(200))
        await settle()
      case "select-row":
        guard let table else {
          check(false, "select-row: no table")
          continue
        }
        let ok = await waitUntil("a row with \(arg)") { table.list.selectRow(containing: arg) }
        check(ok, "select-row \(arg): rows \(table.listRows)")
      case "ddl-field":
        guard let sheet = table?.presentedViewControllers?.compactMap({ $0 as? FormSheetController }).first,
          words.count >= 1
        else {
          check(false, "ddl-field: no editor sheet")
          continue
        }
        let value = words.count > 1 ? words[1...].joined(separator: " ") : ""
        check(sheet.set(words[0], value), "ddl-field \(words[0]): no such field")
      case "ddl-save":
        guard let table, let sheet = table.presentedViewControllers?.compactMap({ $0 as? FormSheetController }).first
        else {
          check(false, "ddl-save: no editor sheet")
          continue
        }
        sheet.onSubmit?(sheet)
        let closed = await waitUntil("the editor to close") {
          table.presentedViewControllers?.contains { $0 === sheet } != true
        }
        check(closed, "ddl-save: the editor stayed open")
        await settle()
      case "expect-no-row":
        guard let table else {
          check(false, "expect-no-row: no table")
          continue
        }
        let ok = await waitUntil("no row with \(arg)") { !table.listRows.contains { $0.contains(arg) } }
        check(ok, "expect-no-row \(arg): rows \(table.listRows)")
      case "expect-table":
        check(table?.table == arg, "expect-table \(arg): got \(table?.table ?? "<no table>")")
      case "expect-no-sidebar":
        guard let connection else {
          check(false, "expect-no-sidebar: no connection")
          continue
        }
        let ok = await waitUntil("no sidebar row \(arg)") { !connection.sidebar.rowTexts.contains(arg) }
        check(ok, "expect-no-sidebar \(arg): rows \(connection.sidebar.rowTexts)")
      case "run-async":
        guard let query else {
          check(false, "run-async: no query view")
          continue
        }
        query.run(mode: .atCursor)
      case "cancel":
        connection?.connection.cancel()
      case "expect-banner", "expect-no-banner":
        let want = verb == "expect-banner"
        let ok = await waitUntil(want ? "the lock banner" : "the lock banner to go") {
          let text = table?.lockBannerText ?? query?.lockBannerText
          return (text != nil) == want
        }
        check(ok, "\(verb): banner \(table?.lockBannerText ?? query?.lockBannerText ?? "<none>")")
      case "shot":
        shot(arg)
      case "terminate":
        // Through NSApplication, as ⌘Q does (the session is saved then)
        log("\(checks) checks, \(failures) failed (terminating)")
        if failures > 0 {
          exit(1)
        }
        NSApp.terminate(nil)
      case "close-window":
        workspace.view.window?.performClose(nil)
        try? await Task.sleep(for: .milliseconds(200))
      case "expect-session-windows":
        var n = -1
        if let data = try? Data(contentsOf: Session.path),
          let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        {
          n = (root["workspaces"] as? [Any])?.count ?? -1
        }
        check(n == Int(arg), "expect-session-windows \(arg): got \(n)")
      case "expect-entries":
        check(workspace.entryCount == Int(arg), "expect-entries \(arg): got \(workspace.entryCount)")
      case "expect-entry":
        check(workspace.currentKind == arg, "expect-entry \(arg): got \(workspace.currentKind)")
      case "expect-title":
        check(workspace.currentTitle == arg, "expect-title \(arg): got \(workspace.currentTitle ?? "<none>")")
      case "expect-windows":
        let n = LaceApp.shared.workspaces.count
        check(n == Int(arg), "expect-windows \(arg): got \(n)")
      case "new-window":
        LaceApp.shared.newWorkspace()
        try? await Task.sleep(for: .milliseconds(100))
      case "window":
        guard let n = Int(arg), let wc = LaceApp.shared.workspace(number: n) else {
          check(false, "window \(arg): no such window")
          continue
        }
        windowController = wc
        wc.window?.makeKeyAndOrderFront(nil)
      case "expect-open-connections":
        let n = LaceApp.shared.openConnectionCount
        check(n == Int(arg), "expect-open-connections \(arg): got \(n)")
      case "expect-badge":
        let ok = await waitUntil("\(arg) badges", quiet: true) { workspace.badgeCount == Int(arg) }
        check(ok, "expect-badge \(arg): got \(workspace.badgeCount)")
      case "expect-alert":
        let ok = await waitUntil("an alert with \(arg)", quiet: true) { alert(containing: arg) != nil }
        check(ok, "expect-alert \(arg): alerts \(Alerts.shown.map(\.text))")
      case "expect-no-alert":
        try? await Task.sleep(for: .milliseconds(300))
        let mine = Alerts.shown.filter { $0.window === windowController.window }
        check(mine.isEmpty, "expect-no-alert: \(mine.map(\.text))")
      case "answer-alert":
        let ok = await waitUntil("an alert to answer", quiet: true) { alert(containing: "") != nil }
        guard ok, let shown = alert(containing: "") else {
          check(false, "answer-alert: no alert over the window")
          continue
        }
        Alerts.answer(shown, go: arg == "go")
        try? await Task.sleep(for: .milliseconds(200))
      case "form-uri", "form-field", "form-folder", "form-pick", "form-action", "expect-field", "expect-form-message",
        "expect-uri-masked", "expect-open-uses":
        guard let form = workspace.currentForm else {
          check(false, "\(verb): the current entry is not a form")
          continue
        }
        await formCommand(form, verb, arg, words)
      case "folder-add":
        let parent = words.count > 1 ? LaceApp.shared.saved.folders.first { $0.name == words[1] }?.path : []
        do {
          try LaceApp.shared.saved.addFolder(named: words.first ?? "", inFolder: parent ?? [])
        } catch {
          check(false, "folder-add: \(error)")
        }
      case "keychain-refuse":
        check(Keychain.isStandIn, "keychain-refuse: no Keychain stand-in")
        Keychain.standInRefuses = arg == "on"
      case "quit":
        log("\(checks) checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
      default:
        check(false, "unknown command \(verb)")
      }
    }
    if failures > 0 {
      // Nothing to wait for: the run failed already
      log("\(checks) checks, \(failures) failed (no quit)")
      exit(1)
    }
    log("\(checks) checks, \(failures) failed (no quit: staying open)")
  }

  private func formCommand(_ form: ConnectionFormViewController, _ verb: String, _ arg: String, _ words: [String]) async {
    let rest = words.count > 1 ? words[1...].joined(separator: " ") : ""
    switch verb {
    case "form-uri":
      form.setURI(arg)
    case "form-field":
      check(!words.isEmpty && form.setField(words[0], rest), "form-field \(words.first ?? ""): no such field")
    case "form-folder":
      check(form.setFolder(named: arg), "form-folder \(arg): no such folder")
    case "form-pick":
      check(form.pick(name: arg), "form-pick \(arg): no such entry")
      try? await Task.sleep(for: .milliseconds(100))
    case "form-action":
      form.perform(arg)
      try? await Task.sleep(for: .milliseconds(50))
      _ = await waitUntil("the form's \(arg)") { !form.isBusy }
      try? await Task.sleep(for: .milliseconds(100))
    case "expect-field":
      let got = form.field(words.first ?? "")
      check(got == rest, "expect-field \(arg): got \(got ?? "<no such field>")")
    case "expect-form-message":
      let got = form.messageText ?? ""
      check(got.contains(arg), "expect-form-message \(arg): got \(got)")
    case "expect-uri-masked":
      let uri = form.uriText
      let pw = form.field("password") ?? ""
      check(
        uri.contains(ConnectionFields.passwordMask) && (pw.isEmpty || !uri.contains(pw)),
        "expect-uri-masked: got \(uri)")
    case "expect-open-uses":
      let cs = form.openTarget()?.connectionString ?? ""
      check(cs.contains(arg), "expect-open-uses: the string Open would use does not hold it")
    default:
      break
    }
  }

  /// A PNG of the window's content (no screen-recording permission needed)
  private func shot(_ name: String) {
    // The window's frame view, so the background and title bar are in the picture
    guard let dir = ProcessInfo.processInfo.environment["LACE_MACOS_SHOTS"], !dir.isEmpty,
      let content = workspace.view.window?.contentView, let view = content.superview ?? Optional(content)
    else {
      log("shot \(name): LACE_MACOS_SHOTS not set or no window")
      return
    }
    view.layoutSubtreeIfNeeded()
    if let window = workspace.view.window {
      log(
        "shot \(name): window \(NSStringFromRect(window.frame)) content \(NSStringFromRect(content.frame)) root \(NSStringFromRect(workspace.view.frame))"
      )
      if ProcessInfo.processInfo.environment["LACE_MACOS_FRAMES"] != nil {
        func dump(_ v: NSView, _ depth: Int) {
          guard depth < 7, !v.isHidden else { return }
          log("  \(String(repeating: "  ", count: depth))\(type(of: v)) \(NSStringFromRect(v.frame))")
          for s in v.subviews {
            dump(s, depth + 1)
          }
        }
        dump(content, 0)
      }
    }
    write(view, to: "\(name).png", in: dir)
    // A sheet or the preferences window is its own window
    let extra = workspace.view.window?.attachedSheet
      ?? NSApp.windows.first { $0.isVisible && $0.title == "Preferences" }
    if let extra, let content = extra.contentView {
      content.layoutSubtreeIfNeeded()
      write(content.superview ?? content, to: "\(name)-sheet.png", in: dir)
    }
  }

  private func write(_ view: NSView, to file: String, in dir: String) {
    guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
    view.cacheDisplay(in: view.bounds, to: rep)
    guard let png = rep.representation(using: .png, properties: [:]) else { return }
    let url = URL(fileURLWithPath: dir).appendingPathComponent(file)
    do {
      try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
      try png.write(to: url)
      log("shot \(url.path)")
    } catch {
      log("shot \(file): \(error)")
    }
  }
}
