//
// Lace macOS frontend
// Session: macos-session.json next to the TUI's files, with the GTK session
// file's shape: windows → connections (by saved id, else a password-less
// connection string) → tabs (a table with its filters, search and sorts by
// column name, or a query's text). Saved when the app quits and when a
// window closes (without that window); restored at launch unless the
// preference is off or -s / --no-session was given.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
enum Session {
  static let fileName = "macos-session.json"
  static var path: URL { Prefs.configDirectory.appendingPathComponent(fileName) }

  /// -s / --no-session on the command line
  static var disabledByArguments: Bool {
    CommandLine.arguments.dropFirst().contains { $0 == "-s" || $0 == "--no-session" }
  }

  // MARK: Save

  static func save(except: WorkspaceWindowController? = nil) {
    let windows = LaceApp.shared.workspaces.filter { $0 !== except }.map(windowJSON)
    let root: [String: Any] = ["version": 1, "windows": windows]
    do {
      try FileManager.default.createDirectory(
        at: Prefs.configDirectory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
      let data = try JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys])
      try Prefs.writePrivate(data, to: path)
    } catch {
      NSLog("session: not saved: \(error)")
    }
  }

  private static func windowJSON(_ wc: WorkspaceWindowController) -> [String: Any] {
    var j: [String: Any] = ["name": wc.name]
    if let window = wc.window {
      j["frame"] = NSStringFromRect(window.frame)
      j["maximized"] = window.isZoomed
    }
    let connections = wc.workspace.connectionControllers
    j["connections"] = connections.map(connectionJSON)
    j["current"] = wc.workspace.currentIndex ?? 0
    return j
  }

  private static func connectionJSON(_ vc: ConnectionViewController) -> [String: Any] {
    var j: [String: Any] = ["name": vc.tabTitle, "sidebar": vc.isSidebarVisible]
    if let saved = vc.saved {
      j["saved_id"] = saved.id
    }
    if let cs = vc.connection.passwordlessConnectionString {
      j["connstr"] = cs
    }
    j["tabs"] = vc.content.tabControllers.map(tabJSON)
    j["current"] = vc.content.currentIndex ?? 0
    return j
  }

  private static func tabJSON(_ controller: NSViewController) -> [String: Any] {
    if let t = controller as? TableViewController {
      var j: [String: Any] = ["type": "table", "table": t.table, "filters_visible": t.filtersVisible]
      j["filters"] = t.filters.map { f -> [String: Any] in
        var fj: [String: Any] = ["op": f.op.key, "value": f.value]
        if let c = f.column { fj["column"] = c }
        if let v2 = f.value2 { fj["value2"] = v2 }
        return fj
      }
      if let s = t.search { j["search"] = s }
      j["sorts"] = t.sorts.map { ["column": $0.column, "desc": $0.descending] as [String: Any] }
      return j
    }
    if let q = controller as? QueryViewController {
      return ["type": "query", "text": q.text]
    }
    return [:]
  }

  // MARK: Restore

  /// Reopen the saved windows; the first one reuses `first` when it is
  /// empty. False when there was nothing to restore.
  @discardableResult
  static func restore(into first: WorkspaceWindowController?) async -> Bool {
    guard let data = try? Data(contentsOf: path),
      let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
      let windows = root["windows"] as? [[String: Any]], !windows.isEmpty
    else { return false }
    var reuse = first?.workspace.connectionControllers.isEmpty == true ? first : nil
    var any = false
    for wj in windows {
      let connections = wj["connections"] as? [[String: Any]] ?? []
      let wc: WorkspaceWindowController
      if let r = reuse {
        wc = r
        reuse = nil
      } else {
        wc = LaceApp.shared.newWorkspace()
      }
      if let name = wj["name"] as? String, !name.isEmpty {
        wc.rename(name)
      }
      if let frame = wj["frame"] as? String, let window = wc.window {
        window.setFrame(NSRectFromString(frame), display: true)
        if wj["maximized"] as? Bool == true, !window.isZoomed {
          window.zoom(nil)
        }
      }
      any = true
      for cj in connections {
        await restoreConnection(cj, into: wc.workspace)
      }
      if let current = wj["current"] as? Int {
        wc.workspace.select(index: current)
      }
    }
    return any
  }

  private static func restoreConnection(_ cj: [String: Any], into workspace: WorkspaceViewController) async {
    var saved: SavedConnection?
    var connstr: String?
    if let id = cj["saved_id"] as? String, let c = LaceApp.shared.savedConnection(id: id) {
      saved = c
      connstr = c.connectionString()
    }
    if connstr == nil {
      connstr = cj["connstr"] as? String
    }
    guard let connstr, !connstr.isEmpty else { return }
    guard let vc = await workspace.open(connectionString: connstr, saved: saved) else { return }
    vc.isSidebarVisible = cj["sidebar"] as? Bool ?? true
    for tj in cj["tabs"] as? [[String: Any]] ?? [] {
      switch tj["type"] as? String {
      case "table":
        guard let name = tj["table"] as? String, !name.isEmpty else { continue }
        let t = vc.content.openTable(name, preview: false)
        let filters = (tj["filters"] as? [[String: Any]] ?? []).compactMap { fj -> Filter? in
          guard let op = fj["op"] as? String else { return nil }
          return Filter(
            column: fj["column"] as? String, op: FilterOp(key: op), value: fj["value"] as? String ?? "",
            value2: fj["value2"] as? String)
        }
        let sorts = (tj["sorts"] as? [[String: Any]] ?? []).compactMap { sj -> SortSpec? in
          guard let c = sj["column"] as? String else { return nil }
          return SortSpec(column: c, descending: sj["desc"] as? Bool ?? false)
        }
        t.restore(
          filters: filters, search: tj["search"] as? String, sorts: sorts,
          filtersVisible: tj["filters_visible"] as? Bool ?? false)
      case "query":
        vc.content.newQuery(text: tj["text"] as? String)
      default:
        continue
      }
    }
    if let current = cj["current"] as? Int {
      vc.content.select(index: current)
    }
  }
}
