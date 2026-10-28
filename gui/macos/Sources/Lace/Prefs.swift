//
// Lace macOS frontend
// Preferences: a view over the shared config.json (LaceKit's SharedConfig,
// the TUI's config.c): "general" for the settings every frontend has, the
// "macos" section for the Keychain switch. macos.json is migrated into it
// once. Files live next to the TUI's, written owner-only through a temp
// file and a rename.
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
  /// Off, this session only, or history/<saved id>.json
  var historyMode: History.Mode = .persistent
  var historyMaxSize = History.defaultSize
  /// Saved passwords go to the Keychain instead of connections.json
  var keychainPasswords = true
  /// Closing a connection's last tab leaves a connection form in its place
  var keepConnectionTab = true
  /// The last window closing quits, or opens a new window with a form
  var lastWorkspaceClose: SharedConfig.LastWorkspaceClose = .quit
  /// A connection placed in a window opens its first table
  var autoOpenFirstTable = false

  /// The app's old preferences file, migrated into config.json once
  static let legacyFileName = "macos.json"

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

  static var legacyPath: URL { configDirectory.appendingPathComponent(legacyFileName) }

  /// config.json's values (defaults for what it lacks; out-of-range values
  /// were already clamped by config.c)
  static func load(_ config: SharedConfig) -> Prefs {
    var p = Prefs()
    p.daemonSpawnMode = LaceClient.SpawnMode(rawValue: config.daemonSpawnMode) ?? .unix
    p.daemonIdleTimeout = (0...86400).contains(config.daemonIdleTimeout) ? config.daemonIdleTimeout : 0
    p.confirmDelete = config.deleteConfirmation
    p.restoreSession = config.restoreSession
    p.approximateCounts = config.approximateCounts
    p.pageSize = config.pageSize
    p.rowNumbers = RowNumbers(rawValue: config.rowNumbers) ?? .auto
    p.historyMode = History.Mode(rawValue: config.historyMode) ?? .persistent
    p.historyMaxSize = History.sizeRange.contains(config.historyMaxSize) ? config.historyMaxSize : History.defaultSize
    p.keychainPasswords = config.keychainPasswords
    p.keepConnectionTab = config.keepConnectionTab
    p.lastWorkspaceClose = config.lastWorkspaceClose
    p.autoOpenFirstTable = config.autoOpenFirstTable
    return p
  }

  /// Into config.json: general and macos replaced, everything else kept
  func save(to config: SharedConfig) throws {
    config.daemonSpawnMode = daemonSpawnMode.rawValue
    config.daemonIdleTimeout = daemonIdleTimeout
    config.deleteConfirmation = confirmDelete
    config.restoreSession = restoreSession
    config.approximateCounts = approximateCounts
    config.pageSize = pageSize
    config.rowNumbers = rowNumbers.rawValue
    config.historyMode = historyMode.rawValue
    config.historyMaxSize = historyMaxSize
    config.keychainPasswords = keychainPasswords
    config.keepConnectionTab = keepConnectionTab
    config.lastWorkspaceClose = lastWorkspaceClose
    config.autoOpenFirstTable = autoOpenFirstTable
    try config.save()
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
