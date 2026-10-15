//
// Lace macOS frontend
// ScriptDriver: drives the app headlessly from $LACE_MACOS_SCRIPT (like the
// GTK frontend's LACE_GTK_SCRIPT), for smoke tests without a hand on the
// mouse. Only active when the variable is set.
//
// Commands, separated by ';' (a ';' inside SQL is written '\;'):
//   wait MS                  pause
//   connect URL              a connection string or SQLite path, as typed
//   saved-add NAME URL       save URL as a connection named NAME (no Keychain)
//   connect-saved NAME       connect through the saved connection named NAME
//   close-connection         disconnect the current connection tab
//   close-tab                close the current inner tab
//   expect-saved N           N saved connections
//   expect-recent N [TEXT]   N recent connections, the newest containing TEXT
//   expect-paging METHOD     the table's last page was read by offset,
//                            keyset or keyset-backward
//   save-session | restore-session   write macos-session.json / reopen it
//   expect-connections N     N connection tabs in the workspace
//   expect-tabs N            N inner tabs in the current connection
//   table NAME               open a table (kept, not a preview) and wait for
//                            its first page and count
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
//   sheet add-row|history|connect|preferences   open that sheet or window
//   dismiss-sheet            close it again
//   shot NAME                NAME.png of the window's content into $LACE_MACOS_SHOTS
//                            (NAME-sheet.png of an open sheet or the preferences)
//   quit                     exit 0, or 1 when an expectation failed
//   terminate                quit through NSApplication, as ⌘Q does (1 on failures)
//   close-window             close the workspace window, as the close button does
//   expect-session-windows N macos-session.json holds N windows
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

  /// Start the driver on the given workspace if $LACE_MACOS_SCRIPT is set
  static func startIfRequested(in workspace: WorkspaceViewController) {
    guard let script = ProcessInfo.processInfo.environment["LACE_MACOS_SCRIPT"], !script.isEmpty else { return }
    let driver = ScriptDriver(workspace: workspace, script: script)
    Task { await driver.run() }
  }

  private let workspace: WorkspaceViewController
  private let commands: [String]
  private var checks = 0
  private var failures = 0
  private let timeout: Double = 30

  private init(workspace: WorkspaceViewController, script: String) {
    self.workspace = workspace
    commands = script.replacingOccurrences(of: "\\;", with: "\u{1}")
      .split(separator: ";").map {
        $0.replacingOccurrences(of: "\u{1}", with: ";").trimmingCharacters(in: .whitespacesAndNewlines)
      }.filter { !$0.isEmpty }
    // Scripts never answer dialogs; errors they cause go to the log. The
    // Keychain would ask too: passwords stay in the private HOME's file.
    LaceApp.shared.prefs.confirmDelete = false
    LaceApp.shared.prefs.keychainPasswords = false
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

  /// Poll a condition on the main actor until it holds or the timeout passes
  private func waitUntil(_ what: String, _ condition: () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while !condition() {
      if Date() > deadline {
        check(false, "timed out waiting for \(what)")
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
      case "saved-add":
        guard words.count >= 2, var c = SavedConnection(connectionString: words[1...].joined(separator: " ")) else {
          check(false, "saved-add: bad arguments")
          continue
        }
        c.name = words[0]
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
        check(connection?.closeCurrentTab() == true, "close-tab: no tab")
        await settle()
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
        case "connect":
          workspace.showConnectSheet(editing: nil)
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
          n = (root["windows"] as? [Any])?.count ?? -1
        }
        check(n == Int(arg), "expect-session-windows \(arg): got \(n)")
      case "quit":
        log("\(checks) checks, \(failures) failed")
        exit(failures == 0 ? 0 : 1)
      default:
        check(false, "unknown command \(verb)")
      }
    }
    log("\(checks) checks, \(failures) failed (no quit: staying open)")
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
