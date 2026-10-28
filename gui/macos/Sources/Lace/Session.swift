//
// Lace macOS frontend
// Session: the session.json every frontend shares (version 2; its JSON
// layer is the shared C code, LaceKit's SessionFile): workspaces (windows,
// geometry under "macos") → connections (by saved id, else a password-less
// connection string; a connection form is {"form": true, "order": N} at its
// place, nothing else, so no typed text or password is ever written) → tabs
// (a table with its filters, search and sorts by column name, or a query's
// text). A save reads the file again and keeps
// what other frontends keep there; a file that does not parse is never
// written. The old macos-session.json is read when there is no
// session.json and renamed macos-session.json.migrated after the first
// save. Saved when the app quits and when a window closes (without that
// window); restored at launch unless the preference is off or -s /
// --no-session was given.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
enum Session {
  static let fileName = "session.json"
  static let legacyFileName = "macos-session.json"
  static var path: URL { Prefs.configDirectory.appendingPathComponent(fileName) }
  static var legacyPath: URL { Prefs.configDirectory.appendingPathComponent(legacyFileName) }
  /// macos-session.json was read: renamed after the first save
  private static var legacyRead = false
  /// A restore is running: saves leave the file alone (a window closed or
  /// the app quit half way would write a half-restored session over it)
  private static var restoring = false

  /// -s / --no-session on the command line
  static var disabledByArguments: Bool {
    CommandLine.arguments.dropFirst().contains { $0 == "-s" || $0 == "--no-session" }
  }

  // MARK: Save

  static func save(except: WorkspaceWindowController? = nil) {
    if restoring {
      if ScriptDriver.isRequested {
        FileHandle.standardError.write(Data("script: session not saved (a restore is running)\n".utf8))
      }
      return
    }
    let windows = LaceApp.shared.workspaces.filter { $0 !== except }
    let front = LaceApp.shared.frontWorkspace
    let current = windows.firstIndex { $0 === front } ?? 0
    let workspaces = windows.map(windowJSON)
    if ScriptDriver.isRequested {
      FileHandle.standardError.write(
        Data("script: session saved, \(workspaces.count) windows\(except == nil ? "" : " (a window closing)")\n".utf8))
    }
    do {
      try FileManager.default.createDirectory(
        at: Prefs.configDirectory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
      // The file as it is now: other frontends' members are kept, one that
      // does not parse is left alone (merge throws)
      var existing: String?
      if FileManager.default.fileExists(atPath: path.path) {
        existing = try String(contentsOf: path, encoding: .utf8)
      }
      let text = try SessionFile.merge(existing: existing, workspaces: workspaces, current: current)
      try Prefs.writePrivate(Data(text.utf8), to: path)
      if legacyRead, rename(legacyPath.path, legacyPath.path + ".migrated") == 0 {
        legacyRead = false
      }
    } catch {
      NSLog("session: not saved: \(error)")
      if ScriptDriver.isRequested {
        FileHandle.standardError.write(Data("script: session not saved: \(error)\n".utf8))
      }
    }
  }

  private static func windowJSON(_ wc: WorkspaceWindowController) -> [String: Any] {
    var j: [String: Any] = ["name": wc.name]
    if let window = wc.window {
      j["macos"] = ["frame": NSStringFromRect(window.frame), "maximized": window.isZoomed] as [String: Any]
    }
    j["connections"] = wc.workspace.entryControllers.enumerated().map { i, entry -> [String: Any] in
      if let vc = entry as? ConnectionViewController {
        return connectionJSON(vc)
      }
      // A form: its place and nothing else (a connection gets no "order":
      // the TUIs would add a placeholder tab where one is)
      return ["form": true, "order": i]
    }
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
      // what differs from a table on Data, so other frontends read the file as before
      if t.object.kind != .table {
        j["object"] = t.object.kind.key
        if let ot = t.object.table { j["object_table"] = ot }
        if let od = t.object.detail { j["object_detail"] = od }
      }
      if t.subview != .data { j["subview"] = t.subview.key(for: t.object.kind) }
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

  /// The saved workspaces: session.json, else the old macos-session.json;
  /// nil when there is none or it does not parse
  static func savedWorkspaces() -> [[String: Any]]? {
    var file = path
    var legacy = false
    if !FileManager.default.fileExists(atPath: file.path) {
      file = legacyPath
      legacy = true
    }
    guard let text = try? String(contentsOf: file, encoding: .utf8) else { return nil }
    do {
      let workspaces = try SessionFile.workspaces(text: text, legacy: legacy)
      if legacy {
        legacyRead = true
      }
      return workspaces
    } catch {
      NSLog("session: \(error)")
      return nil
    }
  }

  /// Reopen the saved windows; the first one reuses `first` while it holds
  /// only the untouched form a window starts with (which then goes). Forms
  /// come back empty in their places; a window that gets nothing back keeps
  /// one form. False when there was nothing to restore.
  @discardableResult
  static func restore(into first: WorkspaceWindowController?) async -> Bool {
    guard let windows = savedWorkspaces(), !windows.isEmpty else { return false }
    restoring = true
    defer { restoring = false }
    var reuse: WorkspaceWindowController?
    if let first, first.workspace.entryCount == 1, let form = first.workspace.currentForm, form.isPristine {
      reuse = first
    }
    var any = false
    for wj in windows {
      let connections = wj["connections"] as? [[String: Any]] ?? []
      let wc: WorkspaceWindowController
      var startForm: ConnectionFormViewController?
      if let r = reuse {
        wc = r
        startForm = r.workspace.currentForm
        reuse = nil
      } else {
        wc = LaceApp.shared.newWorkspace(withForm: false)
      }
      if let name = wj["name"] as? String, !name.isEmpty {
        wc.rename(name)
      }
      let geometry = wj["macos"] as? [String: Any]
      if let frame = geometry?["frame"] as? String, let window = wc.window {
        window.setFrame(NSRectFromString(frame), display: true)
        if geometry?["maximized"] as? Bool == true, !window.isZoomed {
          window.zoom(nil)
        }
      }
      any = true
      let current = wj["current"] as? Int ?? 0
      var select: UUID?
      for (i, cj) in connections.enumerated() {
        if wc.workspace.isClosed { break }  // closed while it was being restored
        let entry: (any WorkspaceEntry)?
        if cj["form"] as? Bool == true {
          entry = wc.workspace.addForm(at: wc.workspace.entryCount)
        } else {
          entry = await restoreConnection(cj, into: wc.workspace)
        }
        if i == current, let entry {
          select = entry.id
        }
      }
      if wc.workspace.isClosed { continue }
      if let startForm {
        wc.workspace.dropPristineForm(startForm)
      }
      if wc.workspace.entryCount == 0 {
        wc.workspace.addForm()  // nothing came back: never an empty window
      } else if let select, let i = wc.workspace.entryControllers.firstIndex(where: { $0.id == select }) {
        wc.workspace.select(index: i)
      }
    }
    return any
  }

  @discardableResult
  private static func restoreConnection(_ cj: [String: Any], into workspace: WorkspaceViewController) async
    -> ConnectionViewController?
  {
    var saved: SavedConnection?
    var connstr: String?
    if let id = cj["saved_id"] as? String, let c = LaceApp.shared.savedConnection(id: id) {
      saved = c
      connstr = c.connectionString()
    }
    if connstr == nil {
      connstr = cj["connstr"] as? String
    }
    guard let connstr, !connstr.isEmpty else { return nil }
    guard let vc = await workspace.open(connectionString: connstr, saved: saved) else { return nil }
    vc.isSidebarVisible = cj["sidebar"] as? Bool ?? true
    for tj in cj["tabs"] as? [[String: Any]] ?? [] {
      switch tj["type"] as? String {
      case "table":
        guard let name = tj["table"] as? String, !name.isEmpty else { continue }
        let kind = (tj["object"] as? String).flatMap(ObjectKind.init(key:)) ?? .table
        let object = CatalogObject(
          kind: kind, name: name, table: tj["object_table"] as? String, detail: tj["object_detail"] as? String)
        let t = vc.content.openObject(object, preview: false)
        if let sv = (tj["subview"] as? String).flatMap(SubView.init(key:)) {
          t.restore(subview: sv)
        }
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
    return vc
  }
}
