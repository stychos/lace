//
// Lace macOS frontend
// Checks: the shared config.json through the TUI's config.c (macos.json
// migrated once, other frontends' members kept, a file that does not parse
// never overwritten) and session.json through the shared session_json.c
// (merge keeps foreign members, a version-1 file converts, a legacy
// macos-session.json's geometry moves under "macos")
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

private func readJSON(_ url: URL) -> [String: Any]? {
  guard let data = try? Data(contentsOf: url) else { return nil }
  return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
}

private func write(_ text: String, to url: URL) {
  try? FileManager.default.removeItem(at: url)
  FileManager.default.createFile(atPath: url.path, contents: Data(text.utf8))
}

func checkSharedFiles(tmp: URL) {
  let dir = tmp.appendingPathComponent("shared-files", isDirectory: true)
  try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  checkSharedConfigMigrates(dir)
  checkSharedConfigKeepsOthers(dir)
  checkSharedConfigUnparsable(dir)
  checkSharedConfigSettings(dir)
  checkSessionFile()
  checkSavedWorkspaces(home: tmp)
  checkDatabaseConnectionString()
}

/// workspaces.json through the TUIs' saved_workspaces.c: a flat ncurses
/// layout comes back nested; put replaces by name, rename re-stores at the
/// end, remove; a file that does not parse is never written
private func checkSavedWorkspaces(home: URL) {
  // The private HOME main.swift set ($HOME, which the C side reads), never
  // NSHomeDirectory(): that is the account's real home whatever $HOME says
  guard ProcessInfo.processInfo.environment["HOME"] == home.path else {
    Checks.fail("saved workspaces: $HOME is not the checks' private one; not touching workspaces.json")
    return
  }
  let dir = home.appendingPathComponent("Library/Application Support/lace")
  try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  let file = dir.appendingPathComponent("workspaces.json")
  try? FileManager.default.removeItem(at: file)
  Checks.equal((try? SavedWorkspaces.list())?.count, 0, "no workspaces.json: no layouts")
  write(
    #"{"workspaces": [{"name": "old", "saved": 100, "workspace": {"tabs": [{"type": "TABLE", "connection_id": "c1", "table": "t"}]}}]}"#,
    to: file)
  let old = (try? SavedWorkspaces.list()) ?? []
  Checks.equal(old.map(\.name), ["old"], "a flat layout is listed")
  Checks.equal(old.first?.connections.first?["saved_id"] as? String, "c1", "a flat layout comes back nested")
  Checks.equal(old.first?.saved.timeIntervalSince1970, 100, "saved time")
  let layout: [String: Any] = ["name": "x", "connections": [["saved_id": "c2", "tabs": [] as [Any]]], "current": 0]
  do {
    try SavedWorkspaces.put(name: "new", workspace: layout)
    try SavedWorkspaces.put(name: "new", workspace: layout)  // replaced, not added
    try SavedWorkspaces.rename("old", to: "older")
    Checks.equal(try SavedWorkspaces.list().map(\.name), ["new", "older"], "put replaces, rename goes last")
    try SavedWorkspaces.remove(name: "new")
    Checks.equal(try SavedWorkspaces.list().map(\.name), ["older"], "removed")
  } catch {
    Checks.fail("saved workspaces: \(error)")
  }
  Checks.expect((try? SavedWorkspaces.rename("older", to: "older")) == nil, "a rename onto an existing name is refused")
  Checks.expect((try? SavedWorkspaces.remove(name: "nope")) == nil, "removing a missing layout is refused")
  let attrs = try? FileManager.default.attributesOfItem(atPath: file.path)
  Checks.equal(((attrs?[.posixPermissions] as? Int) ?? 0) & 0o777, 0o600, "workspaces.json is 0600")
  write(#"{"workspaces": "#, to: file)
  Checks.expect((try? SavedWorkspaces.list()) == nil, "a broken workspaces.json does not list")
  Checks.expect((try? SavedWorkspaces.put(name: "y", workspace: layout)) == nil, "a broken workspaces.json is not written")
  Checks.equal(try? String(contentsOf: file, encoding: .utf8), #"{"workspaces": "#, "the broken file untouched")
  try? FileManager.default.removeItem(at: file)
}

/// The database switch's target: the same server, user, passwords and tunnel
private func checkDatabaseConnectionString() {
  Checks.equal(
    Connection.connectionString("postgres://u:p%40ss@db.example:5433/shop", database: "crm"),
    "postgres://u:p%40ss@db.example:5433/crm", "another database, the password kept")
  let tls = Connection.connectionString(
    "postgres://u:p@db.example/shop?sslmode=verify-full&connect_timeout=5", database: "crm") ?? ""
  Checks.expect(
    tls.hasPrefix("postgres://u:p@db.example") && tls.contains("/crm?") && tls.contains("sslmode=verify-full")
      && tls.contains("connect_timeout=5"),
    "the URL's options kept: \(tls.replacingOccurrences(of: ":p@", with: ":•@"))")
  let key = Connection.connectionString(
    "postgres+ssh://jump@bastion:22/app@10.0.0.5:5432/shop?ssh_key=%2Ftmp%2Fk&sslmode=require", database: "crm") ?? ""
  Checks.equal(key.components(separatedBy: "ssh_key=").count, 2, "ssh_key written once: \(key)")
  Checks.expect(key.contains("sslmode=require"), "an option of a tunnelled URL kept: \(key)")
  let ssh = Connection.connectionString(
    "postgres+ssh://jump@bastion:22/app:secret@10.0.0.5:5432/shop", database: "crm") ?? ""
  Checks.expect(
    ssh.hasPrefix("postgres+ssh://jump@bastion") && ssh.contains("app:secret@10.0.0.5") && ssh.hasSuffix("/crm"),
    "the tunnel kept: \(ssh.replacingOccurrences(of: "secret", with: "•"))")
}

/// macos.json's values go into general/macos once, for what config.json lacks
private func checkSharedConfigMigrates(_ dir: URL) {
  let config = dir.appendingPathComponent("migrate-config.json")
  let legacy = dir.appendingPathComponent("macos.json")
  try? FileManager.default.removeItem(at: config)
  try? FileManager.default.removeItem(at: legacy.appendingPathExtension("migrated"))
  write(#"{"confirm_delete": false, "page_size": 500, "keychain_passwords": false}"#, to: legacy)
  let c = SharedConfig(path: config, legacyMacOS: legacy)
  Checks.equal(c.deleteConfirmation, false, "migrated delete_confirmation")
  Checks.equal(c.pageSize, 500, "migrated page_size")
  Checks.equal(c.keychainPasswords, false, "migrated keychain_passwords")
  Checks.expect(
    FileManager.default.fileExists(atPath: legacy.appendingPathExtension("migrated").path),
    "macos.json.migrated exists")
  Checks.expect(!FileManager.default.fileExists(atPath: legacy.path), "macos.json renamed")
  let json = readJSON(config)
  let general = json?["general"] as? [String: Any]
  Checks.equal(general?["delete_confirmation"] as? Bool, false, "config.json general.delete_confirmation")
  Checks.equal(general?["page_size"] as? Int, 500, "config.json general.page_size")
  Checks.equal((json?["macos"] as? [String: Any])?["keychain_passwords"] as? Bool, false, "config.json macos.keychain_passwords")
  let attrs = try? FileManager.default.attributesOfItem(atPath: config.path)
  Checks.equal(((attrs?[.posixPermissions] as? Int) ?? 0) & 0o777, 0o600, "config.json is 0600")

  // A key config.json has already wins over macos.json's
  let config2 = dir.appendingPathComponent("migrate-config2.json")
  write(#"{"general": {"page_size": 2500}}"#, to: config2)
  write(#"{"page_size": 500, "restore_session": false}"#, to: legacy)
  let c2 = SharedConfig(path: config2, legacyMacOS: legacy)
  Checks.equal(c2.pageSize, 2500, "config.json's page_size kept")
  Checks.equal(c2.restoreSession, false, "macos.json's restore_session taken")
  try? FileManager.default.removeItem(at: legacy.appendingPathExtension("migrated"))

  // A config.json that does not parse: nothing migrates, macos.json stays
  let bad = dir.appendingPathComponent("migrate-bad.json")
  write(#"{"general": "#, to: bad)
  write(#"{"page_size": 500}"#, to: legacy)
  _ = SharedConfig(path: bad, legacyMacOS: legacy)
  Checks.expect(FileManager.default.fileExists(atPath: legacy.path), "macos.json kept beside a bad config.json")
  Checks.equal(try? String(contentsOf: bad, encoding: .utf8), #"{"general": "#, "the bad config.json untouched")
  try? FileManager.default.removeItem(at: legacy)
}

/// A save keeps what the TUIs and GTK keep in the file, and general's keys macOS does not show
private func checkSharedConfigKeepsOthers(_ dir: URL) {
  let config = dir.appendingPathComponent("keep-config.json")
  let hotkeys: [String: Any] = ["palette": ["CTRL+P"]]
  let notcurses: [String: Any] = ["a": 1]
  let gtk: [String: Any] = ["b": 2]
  write(
    #"{"general": {"show_header": false, "page_size": 1000}, "hotkeys": {"palette": ["CTRL+P"]}, "notcurses": {"a": 1}, "gtk": {"b": 2}}"#,
    to: config)
  let c = SharedConfig(path: config, legacyMacOS: nil)
  Checks.equal(c.pageSize, 1000, "page_size read")
  c.pageSize = 2000
  do {
    try c.save()
  } catch {
    Checks.fail("save: \(error)")
  }
  let json = readJSON(config)
  Checks.expect((json?["hotkeys"] as? NSDictionary)?.isEqual(to: hotkeys) == true, "hotkeys kept: \(json?["hotkeys"] ?? "nil")")
  Checks.expect((json?["notcurses"] as? NSDictionary)?.isEqual(to: notcurses) == true, "notcurses kept")
  Checks.expect((json?["gtk"] as? NSDictionary)?.isEqual(to: gtk) == true, "gtk kept")
  let general = json?["general"] as? [String: Any]
  Checks.equal(general?["show_header"] as? Bool, false, "general.show_header kept")
  Checks.equal(general?["page_size"] as? Int, 2000, "general.page_size written")
}

/// A config.json that does not parse loads defaults and is never overwritten
private func checkSharedConfigUnparsable(_ dir: URL) {
  let config = dir.appendingPathComponent("bad-config.json")
  write(#"{"general": "#, to: config)
  let c = SharedConfig(path: config, legacyMacOS: nil)
  Checks.expect(c.loadError != nil, "a load error for a bad config.json")
  Checks.equal(c.pageSize, 1000, "defaults for a bad config.json")
  var threw = false
  do {
    try c.save()
  } catch {
    threw = true
  }
  Checks.expect(threw, "save over a bad config.json throws")
  Checks.equal(try? String(contentsOf: config, encoding: .utf8), #"{"general": "#, "the bad file untouched")
}

/// The connection tab's settings and every general key round-trip
private func checkSharedConfigSettings(_ dir: URL) {
  let config = dir.appendingPathComponent("settings-config.json")
  try? FileManager.default.removeItem(at: config)
  let c = SharedConfig(path: config, legacyMacOS: nil)
  Checks.equal(c.keepConnectionTab, true, "keep_connection_tab default")
  Checks.equal(c.lastWorkspaceClose, .quit, "on_last_workspace_close default")
  Checks.equal(c.autoOpenFirstTable, false, "auto_open_first_table default")
  Checks.equal(c.keychainPasswords, true, "keychain_passwords default")
  Checks.equal(c.restoreCursorPosition, true, "restore_cursor_position default")
  Checks.equal(c.quitConfirmation, false, "quit_confirmation default")
  Checks.equal(c.colorScheme, .system, "color_scheme default")
  c.restoreCursorPosition = false
  c.quitConfirmation = true
  c.colorScheme = .dark
  c.keepConnectionTab = false
  c.lastWorkspaceClose = .new
  c.autoOpenFirstTable = true
  c.keychainPasswords = false
  c.restoreSession = false
  c.approximateCounts = false
  c.historyMode = 1
  c.historyMaxSize = 77
  c.daemonSpawnMode = 3
  c.daemonIdleTimeout = 600
  c.rowNumbers = 2
  c.deleteConfirmation = false
  do {
    try c.save()
  } catch {
    Checks.fail("settings save: \(error)")
  }
  let json = readJSON(config)
  let general = json?["general"] as? [String: Any]
  Checks.equal(general?["on_last_workspace_close"] as? String, "new", "on_last_workspace_close written as a word")
  let back = SharedConfig(path: config, legacyMacOS: nil)
  Checks.equal(back.keepConnectionTab, false, "keep_connection_tab back")
  Checks.equal(back.lastWorkspaceClose, .new, "on_last_workspace_close back")
  Checks.equal(back.autoOpenFirstTable, true, "auto_open_first_table back")
  Checks.equal(back.keychainPasswords, false, "keychain_passwords back")
  Checks.equal(back.restoreSession, false, "restore_session back")
  Checks.equal(back.approximateCounts, false, "approximate_counts back")
  Checks.equal(back.historyMode, 1, "history_mode back")
  Checks.equal(back.historyMaxSize, 77, "history_max_size back")
  Checks.equal(back.daemonSpawnMode, 3, "daemon_spawn_mode back")
  Checks.equal(back.daemonIdleTimeout, 600, "daemon_idle_timeout back")
  Checks.equal(back.rowNumbers, 2, "row_numbers back")
  Checks.equal(back.deleteConfirmation, false, "delete_confirmation back")
  Checks.equal(back.restoreCursorPosition, false, "restore_cursor_position back")
  Checks.equal(back.quitConfirmation, true, "quit_confirmation back")
  Checks.equal(back.colorScheme, .dark, "color_scheme back")
  Checks.equal(general?["color_scheme"] as? Int, 2, "color_scheme written as 2")
}

private func checkSessionFile() {
  // A merge keeps other frontends' members, writes version 2
  let ws: [[String: Any]] = [["name": "W", "macos": ["frame": "{{0, 0}, {800, 600}}"], "connections": []]]
  do {
    let text = try SessionFile.merge(
      existing: #"{"version": 2, "workspaces": [], "ncurses": {"x": 1}}"#, workspaces: ws, current: 0)
    let json = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any]
    Checks.equal(json?["version"] as? Int, 2, "merge: version 2")
    Checks.equal((json?["ncurses"] as? [String: Any])?["x"] as? Int, 1, "merge: ncurses kept")
    let w = (json?["workspaces"] as? [[String: Any]])?.first
    Checks.equal((w?["macos"] as? [String: Any])?["frame"] as? String, "{{0, 0}, {800, 600}}", "merge: the frame")
    Checks.equal(json?["current"] as? Int, 0, "merge: current")
  } catch {
    Checks.fail("merge: \(error)")
  }
  // No file yet
  do {
    let text = try SessionFile.merge(existing: nil, workspaces: ws, current: 0)
    Checks.expect(text.contains("\"workspaces\""), "merge without a file")
  } catch {
    Checks.fail("merge without a file: \(error)")
  }

  // A version-1 (ncurses) file converts
  do {
    let v1 =
      #"{"version": 1, "workspaces": [{"name": "T", "current_tab": 0, "tabs": [{"type": "TABLE", "connection_id": "id-1", "table_name": "big"}]}]}"#
    let w = try SessionFile.workspaces(text: v1, legacy: false)
    let conns = w.first?["connections"] as? [[String: Any]]
    Checks.equal(conns?.first?["saved_id"] as? String, "id-1", "v1: saved_id")
    let tabs = conns?.first?["tabs"] as? [[String: Any]]
    Checks.equal(tabs?.first?["table"] as? String, "big", "v1: the table tab")
  } catch {
    Checks.fail("v1: \(error)")
  }

  // A legacy macos-session.json: geometry under "macos"
  do {
    let legacy = #"{"windows": [{"name": "G", "frame": "{{0,0},{9,9}}", "maximized": true, "connections": []}]}"#
    let w = try SessionFile.workspaces(text: legacy, legacy: true)
    Checks.equal(w.count, 1, "legacy: one window")
    let macos = w.first?["macos"] as? [String: Any]
    Checks.equal(macos?["maximized"] as? Bool, true, "legacy: maximized under macos")
    Checks.equal(macos?["frame"] as? String, "{{0,0},{9,9}}", "legacy: frame under macos")
    Checks.expect(w.first?["frame"] == nil, "legacy: no top-level frame")
  } catch {
    Checks.fail("legacy: \(error)")
  }

  // A file that does not parse: never overwritten (merge throws), not read
  var threw = false
  do {
    _ = try SessionFile.merge(existing: #"{"version": 2, "work"#, workspaces: ws, current: 0)
  } catch {
    threw = true
  }
  Checks.expect(threw, "merge over a bad file throws")
  threw = false
  do {
    _ = try SessionFile.workspaces(text: "{nope", legacy: false)
  } catch {
    threw = true
  }
  Checks.expect(threw, "reading a bad file throws")
}
