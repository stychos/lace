//
// Lace macOS frontend
// Preferences: macos.json next to the TUI's files (the GTK frontend's
// gtk.json keys), written owner-only through a temp file and a rename
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

struct Prefs {
  enum RowNumbers: Int {
    case never = 0
    /// Hidden when the key is one auto-increment integer, which numbers the rows already
    case auto = 1
    case always = 2
  }

  var daemonSpawnMode: LaceClient.SpawnMode = .unix
  var daemonIdleTimeout: Int32 = 0
  var confirmDelete = true
  var restoreSession = true
  var approximateCounts = true
  var pageSize = RowModel.defaultPageSize
  var rowNumbers: RowNumbers = .auto
  /// Off, this session only, or history/<saved id>.json (the GTK keys)
  var historyMode: History.Mode = .persistent
  var historyMaxSize = History.defaultSize
  /// Saved passwords go to the Keychain instead of connections.json
  var keychainPasswords = true

  static let fileName = "macos.json"

  /// $HOME when set (as the TUI and liblace take it, so a test's private
  /// HOME is honoured), else the account's home
  static var homeDirectory: URL {
    if let home = ProcessInfo.processInfo.environment["HOME"], !home.isEmpty {
      return URL(fileURLWithPath: home, isDirectory: true)
    }
    return FileManager.default.homeDirectoryForCurrentUser
  }

  /// ~/Library/Application Support/lace (the TUI's and the GTK frontend's)
  static var configDirectory: URL {
    homeDirectory.appendingPathComponent("Library/Application Support/lace", isDirectory: true)
  }

  static var path: URL { configDirectory.appendingPathComponent(fileName) }

  /// Defaults, then whatever macos.json holds (out-of-range values are ignored)
  static func load() -> Prefs {
    var p = Prefs()
    guard let data = try? Data(contentsOf: path),
      let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else { return p }
    if let v = json["daemon_spawn_mode"] as? Int, let mode = LaceClient.SpawnMode(rawValue: Int32(v)) {
      p.daemonSpawnMode = mode
    }
    if let v = json["daemon_idle_timeout"] as? Int, (0...86400).contains(v) {
      p.daemonIdleTimeout = Int32(v)
    }
    if let v = json["confirm_delete"] as? Bool { p.confirmDelete = v }
    if let v = json["restore_session"] as? Bool { p.restoreSession = v }
    if let v = json["approximate_counts"] as? Bool { p.approximateCounts = v }
    if let v = json["page_size"] as? Int, (100...100_000).contains(v) { p.pageSize = v }
    if let v = json["row_numbers"] as? Int, let r = RowNumbers(rawValue: v) { p.rowNumbers = r }
    if let v = json["history_mode"] as? Int, let m = History.Mode(rawValue: v) { p.historyMode = m }
    if let v = json["history_max_size"] as? Int, History.sizeRange.contains(v) { p.historyMaxSize = v }
    if let v = json["keychain_passwords"] as? Bool { p.keychainPasswords = v }
    return p
  }

  func save() throws {
    let dir = Prefs.configDirectory
    try FileManager.default.createDirectory(
      at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let json: [String: Any] = [
      "daemon_spawn_mode": Int(daemonSpawnMode.rawValue),
      "daemon_idle_timeout": Int(daemonIdleTimeout),
      "confirm_delete": confirmDelete,
      "restore_session": restoreSession,
      "approximate_counts": approximateCounts,
      "page_size": pageSize,
      "row_numbers": rowNumbers.rawValue,
      "history_mode": historyMode.rawValue,
      "history_max_size": historyMaxSize,
      "keychain_passwords": keychainPasswords,
    ]
    let data = try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys])
    try Prefs.writePrivate(data, to: Prefs.path)
  }

  /// Like the TUI's platform_write_file_private(): a 0600 temp file in the
  /// same directory, synced, then renamed over the target, so a crash leaves
  /// the old file and nothing is ever briefly world-readable
  static func writePrivate(_ data: Data, to url: URL) throws {
    let tmp = url.deletingLastPathComponent()
      .appendingPathComponent(".\(url.lastPathComponent).\(getpid()).tmp")
    guard
      FileManager.default.createFile(
        atPath: tmp.path, contents: data, attributes: [.posixPermissions: 0o600])
    else {
      throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: tmp.path])
    }
    if let fh = try? FileHandle(forWritingTo: tmp) {
      try? fh.synchronize()
      try? fh.close()
    }
    if rename(tmp.path, url.path) != 0 {
      let code = errno
      unlink(tmp.path)
      throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
    }
  }

  /// Whether a grid over this schema shows a row number column
  func rowNumbers(for schema: Schema?) -> Bool {
    switch rowNumbers {
    case .never: return false
    case .always: return true
    case .auto:
      guard let schema else { return true }
      let keys = schema.primaryKey
      let autoInt = keys.count == 1 && keys[0].autoIncrement && keys[0].type == .int
      return !autoInt
    }
  }
}
