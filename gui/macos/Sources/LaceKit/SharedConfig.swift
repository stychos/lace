//
// Lace macOS frontend
// SharedConfig: the config.json every frontend shares, read and written by
// the TUI's own config.c (compiled into the app, see the Makefile). This app
// owns "general" (the settings common to the frontends) and its "macos"
// section; a save reads the file again and keeps everything else (the TUIs'
// hotkeys, the notcurses and gtk sections, general keys this app does not
// show). A file that does not parse loads defaults and is never written.
// macos.json, the app's old preferences file, is migrated once: its values
// for the keys config.json lacks, then it is renamed macos.json.migrated.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class SharedConfig {
  /// general.on_last_workspace_close
  public enum LastWorkspaceClose: Int32, Sendable {
    /// The app quits
    case quit = 0
    /// A new window with a connection form
    case new = 1
  }

  private let config: UnsafeMutablePointer<Config>
  private let path: String?
  /// Why the file could not be read (it is then never written)
  public private(set) var loadError: String?

  /// path nil: the default config.json; legacyMacOS: macos.json to migrate
  public init(path: URL? = nil, legacyMacOS: URL? = nil) {
    self.path = path?.path
    var err: UnsafeMutablePointer<CChar>? = nil
    let loaded = SharedConfig.withOptionalCString(self.path) { config_load_file($0, &err) }
    config = loaded ?? config_get_defaults()
    loadError = takeCString(err)
    // A GUI owns no hotkeys: the file's are kept as they are, and the TUIs'
    // bindings are not checked for conflicts on this app's saves
    config.pointee.hotkeys_foreign = true
    config.pointee.frontend = 0
    if let legacyMacOS {
      migrate(from: legacyMacOS)
    }
  }

  deinit {
    config_free(config)
  }

  public struct SaveError: Error, LocalizedError {
    public let message: String
    public var errorDescription: String? { message }
  }

  /// Write the file: "general" and "macos" from here, the rest as the file has it
  public func save() throws {
    var err: UnsafeMutablePointer<CChar>? = nil
    let ok = SharedConfig.withOptionalCString(path) { config_save_file(config, $0, &err) }
    let message = takeCString(err)
    if !ok {
      throw SaveError(message: message ?? "config.json could not be written")
    }
  }

  /// Where the file is (the default path when none was given)
  public var filePath: String {
    path ?? takeCString(config_get_path()) ?? "config.json"
  }

  // MARK: general

  public var deleteConfirmation: Bool {
    get { config.pointee.general.delete_confirmation }
    set { config.pointee.general.delete_confirmation = newValue }
  }
  public var restoreSession: Bool {
    get { config.pointee.general.restore_session }
    set { config.pointee.general.restore_session = newValue }
  }
  public var approximateCounts: Bool {
    get { config.pointee.general.approximate_counts }
    set { config.pointee.general.approximate_counts = newValue }
  }
  /// 0 off, 1 this session, 2 kept per saved connection
  public var historyMode: Int {
    get { Int(config.pointee.general.history_mode) }
    set { config.pointee.general.history_mode = Int32(newValue) }
  }
  public var historyMaxSize: Int {
    get { Int(config.pointee.general.history_max_size) }
    set { config.pointee.general.history_max_size = Int32(newValue) }
  }
  /// LaceClient.SpawnMode's raw value
  public var daemonSpawnMode: Int32 {
    get { config.pointee.general.daemon_spawn_mode }
    set { config.pointee.general.daemon_spawn_mode = newValue }
  }
  public var daemonIdleTimeout: Int32 {
    get { config.pointee.general.daemon_idle_timeout }
    set { config.pointee.general.daemon_idle_timeout = newValue }
  }
  public var pageSize: Int {
    get { Int(config.pointee.general.page_size) }
    set { config.pointee.general.page_size = Int32(newValue) }
  }
  /// 0 never, 1 unless the key numbers the rows, 2 always
  public var rowNumbers: Int {
    get { Int(config.pointee.general.row_numbers) }
    set { config.pointee.general.row_numbers = Int32(newValue) }
  }
  /// Closing a connection's last tab leaves a connection form in its place
  public var keepConnectionTab: Bool {
    get { config.pointee.general.keep_connection_tab }
    set { config.pointee.general.keep_connection_tab = newValue }
  }
  public var lastWorkspaceClose: LastWorkspaceClose {
    get { LastWorkspaceClose(rawValue: config.pointee.general.last_workspace_close) ?? .quit }
    set { config.pointee.general.last_workspace_close = newValue.rawValue }
  }
  /// A connection placed in a window opens its first table
  public var autoOpenFirstTable: Bool {
    get { config.pointee.general.auto_open_first_table }
    set { config.pointee.general.auto_open_first_table = newValue }
  }

  // MARK: macos

  /// Saved passwords go to the Keychain instead of connections.json
  public var keychainPasswords: Bool {
    get { config_section_get_bool(config, "macos", "keychain_passwords", true) }
    set { config_section_set_bool(config, "macos", "keychain_passwords", newValue) }
  }

  // MARK: Migration

  private static let pageSizes = Int(CONFIG_PAGE_SIZE_MIN)...Int(CONFIG_PAGE_SIZE_MAX)

  /// macos.json's values for the keys config.json lacks, written, then the
  /// old file renamed; nothing when config.json is there but does not parse
  private func migrate(from legacy: URL) {
    // A config.json that does not parse is never written; an empty one is no file
    let empty = ((try? FileManager.default.attributesOfItem(atPath: filePath)[.size]) as? Int) == 0
    guard FileManager.default.fileExists(atPath: legacy.path), loadError == nil || empty else { return }
    var general: [String: Any] = [:]
    var macos: [String: Any] = [:]
    let file = URL(fileURLWithPath: filePath)
    if let data = try? Data(contentsOf: file), !data.isEmpty {
      guard let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return }
      general = root["general"] as? [String: Any] ?? [:]
      macos = root["macos"] as? [String: Any] ?? [:]
    }
    guard let data = try? Data(contentsOf: legacy),
      let old = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    else { return }
    func int(_ key: String, _ range: ClosedRange<Int>) -> Int? {
      guard let v = old[key] as? Int, range.contains(v) else { return nil }
      return v
    }
    func lacks(_ key: String) -> Bool { general[key] == nil }
    if lacks("daemon_spawn_mode"), let v = int("daemon_spawn_mode", 0...3) { daemonSpawnMode = Int32(v) }
    if lacks("daemon_idle_timeout"), let v = int("daemon_idle_timeout", 0...86400) { daemonIdleTimeout = Int32(v) }
    if lacks("delete_confirmation"), let v = old["confirm_delete"] as? Bool { deleteConfirmation = v }
    if lacks("restore_session"), let v = old["restore_session"] as? Bool { restoreSession = v }
    if lacks("approximate_counts"), let v = old["approximate_counts"] as? Bool { approximateCounts = v }
    if lacks("page_size"), let v = int("page_size", SharedConfig.pageSizes) { pageSize = v }
    if lacks("row_numbers"), let v = int("row_numbers", 0...2) { rowNumbers = v }
    if lacks("history_mode"), let v = int("history_mode", 0...2) { historyMode = v }
    if lacks("history_max_size"), let v = int("history_max_size", History.sizeRange) { historyMaxSize = v }
    if macos["keychain_passwords"] == nil, let v = old["keychain_passwords"] as? Bool { keychainPasswords = v }
    guard (try? save()) != nil else { return }
    _ = rename(legacy.path, legacy.path + ".migrated")
  }

  private static func withOptionalCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
    guard let s else { return body(nil) }
    return s.withCString { body($0) }
  }
}
