//
// Lace macOS frontend
// Saved connections: connections.json through the TUI's ConnectionManager
// (compiled into the app, see the Makefile), so the TUI, the GTK frontend
// and this app share one file. SavedConnection is a value copied out of the
// C tree; SavedConnectionStore owns the tree and writes every change at once.
//
// Connection strings are built only with lace_saved_conn_build_connstr():
// it URL-encodes users and passwords, emits the +ssh form for a tunnel, and
// takes the passwords a prompt supplied, so a retry keeps the tunnel.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public struct SavedConnection: Sendable, Hashable, Identifiable {
  /// A UUID; names history/<id>.json too
  public var id: String
  public var name: String
  /// "sqlite", "postgres", "mysql" or "mariadb"
  public var driver: String
  public var host: String
  /// 0: the driver's default
  public var port: Int
  /// The file for SQLite
  public var database: String
  public var user: String
  /// Written to the file only while savePassword is set
  public var password: String?
  public var savePassword: Bool
  public var ssh: Bool
  public var sshHost: String
  public var sshPort: Int
  public var sshUser: String
  public var sshPassword: String?
  public var saveSSHPassword: Bool
  /// A private key file for ssh -i; empty: ssh's own choice
  public var sshKey: String

  public init(
    id: String = SavedConnection.newID(), name: String = "", driver: String = "sqlite",
    host: String = "", port: Int = 0, database: String = "", user: String = "",
    password: String? = nil, savePassword: Bool = false, ssh: Bool = false, sshHost: String = "",
    sshPort: Int = 0, sshUser: String = "", sshPassword: String? = nil, saveSSHPassword: Bool = false,
    sshKey: String = ""
  ) {
    self.id = id
    self.name = name
    self.driver = driver
    self.host = host
    self.port = port
    self.database = database
    self.user = user
    self.password = password
    self.savePassword = savePassword
    self.ssh = ssh
    self.sshHost = sshHost
    self.sshPort = sshPort
    self.sshUser = sshUser
    self.sshPassword = sshPassword
    self.saveSSHPassword = saveSSHPassword
    self.sshKey = sshKey
  }

  /// liblace's UUID v4
  public static func newID() -> String {
    takeCString(lace_generate_uuid()) ?? UUID().uuidString.lowercased()
  }

  /// From a connection string (typed, or a recent one); nil when it does
  /// not parse. Passwords in the string are kept (not marked saved).
  public init?(connectionString: String) {
    var err: UnsafeMutablePointer<CChar>? = nil
    guard let c = lace_saved_conn_from_connstr(connectionString, &err) else {
      free(err)
      return nil
    }
    defer { lace_saved_conn_free(c) }
    self.init(c.pointee)
  }

  init(_ c: LaceSavedConnection) {
    self.init(
      id: borrowCString(c.id) ?? SavedConnection.newID(), name: borrowCString(c.name) ?? "",
      driver: borrowCString(c.driver).map { borrowCString(lace_driver_normalize($0)) ?? $0 } ?? "sqlite",
      host: borrowCString(c.host) ?? "", port: Int(c.port), database: borrowCString(c.database) ?? "",
      user: borrowCString(c.user) ?? "", password: borrowCString(c.password),
      savePassword: c.save_password, ssh: c.ssh, sshHost: borrowCString(c.ssh_host) ?? "",
      sshPort: Int(c.ssh_port), sshUser: borrowCString(c.ssh_user) ?? "",
      sshPassword: borrowCString(c.ssh_password), saveSSHPassword: c.save_ssh_password,
      sshKey: borrowCString(c.ssh_key) ?? "")
  }

  /// Copy every field into a C struct the caller owns (its old strings freed)
  func apply(to p: UnsafeMutablePointer<LaceSavedConnection>) {
    func set(_ field: inout UnsafeMutablePointer<CChar>?, _ value: String?) {
      if let field { str_secure_free(field) }
      field = value.flatMap { $0.isEmpty ? nil : strdup($0) }
    }
    set(&p.pointee.id, id)
    set(&p.pointee.name, name)
    set(&p.pointee.driver, driver)
    set(&p.pointee.host, host)
    set(&p.pointee.database, database)
    set(&p.pointee.user, user)
    set(&p.pointee.password, password)
    p.pointee.port = Int32(port)
    p.pointee.save_password = savePassword
    p.pointee.ssh = ssh
    set(&p.pointee.ssh_host, sshHost)
    set(&p.pointee.ssh_user, sshUser)
    set(&p.pointee.ssh_password, sshPassword)
    p.pointee.ssh_port = Int32(sshPort)
    p.pointee.save_ssh_password = saveSSHPassword
    set(&p.pointee.ssh_key, sshKey)
  }

  /// A temporary C struct holding this value
  func withCStruct<R>(_ body: (UnsafeMutablePointer<LaceSavedConnection>) throws -> R) rethrows -> R {
    guard let p = lace_saved_conn_new() else { fatalError("out of memory") }
    defer { lace_saved_conn_free(p) }
    apply(to: p)
    return try body(p)
  }

  /// The connection string: saved passwords included when their flag is
  /// set, or the ones given here (a prompt's answer); the tunnel is kept.
  /// May hold passwords: never show or log it.
  public func connectionString(password: String? = nil, sshPassword: String? = nil) -> String {
    withCStruct { p in
      guard let s = lace_saved_conn_build_connstr(p, password, sshPassword) else { return "" }
      defer { str_secure_free(s) }
      return String(cString: s)
    }
  }

  public var driverKind: Driver {
    switch driver {
    case "postgres", "postgresql": .postgres
    case "mysql": .mysql
    case "mariadb": .mariadb
    default: .sqlite
    }
  }

  public var isSQLite: Bool { driverKind == .sqlite }

  /// The name, else what it connects to
  public var displayName: String {
    name.isEmpty ? summary : name
  }

  /// "data.sqlite", "user@host:5432/db via ssh host": never a password
  public var summary: String {
    if isSQLite {
      let file = (database as NSString).lastPathComponent
      return file.isEmpty ? "SQLite" : file
    }
    var s = ""
    if !user.isEmpty {
      s += "\(user)@"
    }
    s += host.isEmpty ? "localhost" : host
    if port > 0 {
      s += ":\(port)"
    }
    s += "/\(database)"
    if ssh {
      s += " via ssh \(sshUser.isEmpty ? "" : sshUser + "@")\(sshHost)"
    }
    return s
  }

  /// The default port of a driver (0 for SQLite)
  public static func defaultPort(driver: String) -> Int {
    Int(lace_driver_default_port(driver))
  }
}

/// The tree of connections.json: folders and connections. Main thread only.
public final class SavedConnectionStore {
  public struct Folder: Hashable, Sendable {
    /// Indices from the root
    public let path: [Int]
    public let name: String
    public var depth: Int { path.count }
    public var isRoot: Bool { path.isEmpty }
  }

  public struct Entry: Hashable, Sendable, Identifiable {
    public let connection: SavedConnection
    /// The folder it is in ([] for the root)
    public let folderPath: [Int]
    public var id: String { connection.id }
    public var depth: Int { folderPath.count }
  }

  public enum StoreError: Error, LocalizedError, Equatable {
    case readOnly(String)
    case notFound
    case folderNotEmpty
    case save(String)

    public var errorDescription: String? {
      switch self {
      case .readOnly(let why): "connections.json is read only: \(why)"
      case .notFound: "The connection is not in the list any more"
      case .folderNotEmpty: "The folder is not empty"
      case .save(let why): why
      }
    }
  }

  private let mgr: UnsafeMutablePointer<ConnectionManager>
  /// Why the file could not be loaded (the store is then read only)
  public private(set) var loadError: String?
  /// Every folder in tree order, the root first
  public private(set) var folders: [Folder] = []
  /// Every connection in tree order
  public private(set) var entries: [Entry] = []
  /// After every change (also posted as didChange, for every connection form)
  public var onChanged: (() -> Void)?
  public static let didChange = Notification.Name("LaceSavedConnectionsDidChange")

  /// connections.json in the config directory (the TUI's path rules)
  public init() {
    var err: UnsafeMutablePointer<CChar>? = nil
    if let m = connmgr_load(&err) {
      mgr = m
    } else {
      mgr = connmgr_new()
    }
    loadError = takeCString(err)
    snapshot()
  }

  /// A given file (tests)
  public init(path: String) {
    var err: UnsafeMutablePointer<CChar>? = nil
    if let m = connmgr_load_file(path, &err) {
      mgr = m
    } else {
      mgr = connmgr_new()
    }
    loadError = takeCString(err)
    snapshot()
  }

  deinit {
    connmgr_free(mgr)
  }

  public var isReadOnly: Bool { mgr.pointee.read_only }
  public var path: String? { borrowCString(mgr.pointee.file_path) }
  public var isEmpty: Bool { entries.isEmpty && folders.count <= 1 }

  public func connection(id: String) -> SavedConnection? {
    entries.first { $0.connection.id == id }?.connection
  }

  public func entry(id: String) -> Entry? {
    entries.first { $0.connection.id == id }
  }

  public func folder(at path: [Int]) -> Folder? {
    folders.first { $0.path == path }
  }

  /// The connections of a folder (not of its subfolders)
  public func entries(inFolder path: [Int]) -> [Entry] {
    entries.filter { $0.folderPath == path }
  }

  // MARK: Changes (each written to the file at once)

  public func add(_ connection: SavedConnection, inFolder path: [Int] = []) throws {
    try checkWritable()
    guard let folder = item(at: path), lace_item_folder(folder) != nil else { throw StoreError.notFound }
    guard let c = connmgr_new_connection() else { fatalError("out of memory") }
    connection.apply(to: c)
    guard connmgr_add_connection(folder, c) else {
      lace_saved_conn_free(c)
      throw StoreError.save("The connection could not be added")
    }
    try persist()
  }

  /// Replace the fields of the connection with this id
  public func update(_ connection: SavedConnection) throws {
    try checkWritable()
    guard let item = connmgr_find_by_id(mgr, connection.id), let c = lace_item_connection(item) else {
      throw StoreError.notFound
    }
    connection.apply(to: c)
    try persist()
  }

  public func remove(id: String) throws {
    try checkWritable()
    guard let item = connmgr_find_by_id(mgr, id) else { throw StoreError.notFound }
    connmgr_remove_item(mgr, item)
    try persist()
  }

  public func addFolder(named name: String, inFolder path: [Int] = []) throws {
    try checkWritable()
    guard let parent = item(at: path), lace_item_folder(parent) != nil else { throw StoreError.notFound }
    guard let f = connmgr_new_folder(name) else { fatalError("out of memory") }
    guard connmgr_add_folder(parent, f) else {
      connmgr_free_folder(f)
      free(f)
      throw StoreError.save("The folder could not be added")
    }
    try persist()
  }

  public func renameFolder(at path: [Int], to name: String) throws {
    try checkWritable()
    guard !path.isEmpty, let item = item(at: path), let f = lace_item_folder(item) else {
      throw StoreError.notFound
    }
    free(f.pointee.name)
    f.pointee.name = strdup(name)
    try persist()
  }

  /// Only an empty folder
  public func removeFolder(at path: [Int]) throws {
    try checkWritable()
    guard !path.isEmpty, let item = item(at: path), let f = lace_item_folder(item) else {
      throw StoreError.notFound
    }
    guard f.pointee.num_children == 0 else { throw StoreError.folderNotEmpty }
    connmgr_remove_item(mgr, item)
    try persist()
  }

  /// Move a connection to the end of another folder
  public func move(id: String, toFolder path: [Int]) throws {
    try checkWritable()
    guard let item = connmgr_find_by_id(mgr, id), let parent = self.item(at: path),
      let f = lace_item_folder(parent)
    else { throw StoreError.notFound }
    if item.pointee.parent == parent { return }
    let last = f.pointee.num_children > 0 ? lace_folder_child(f, f.pointee.num_children - 1) : nil
    guard connmgr_move_item(mgr, item, parent, last) else { throw StoreError.save("The connection could not be moved") }
    try persist()
  }

  // MARK: Internals

  private func checkWritable() throws {
    if isReadOnly {
      throw StoreError.readOnly(loadError ?? "it did not load")
    }
  }

  private func persist() throws {
    defer {
      snapshot()
      onChanged?()
      NotificationCenter.default.post(name: SavedConnectionStore.didChange, object: self)
    }
    var err: UnsafeMutablePointer<CChar>? = nil
    if !connmgr_save(mgr, &err) {
      throw StoreError.save(takeCString(err) ?? "connections.json could not be written")
    }
  }

  /// The item at an index path from the root (pointers are stale after any change)
  private func item(at path: [Int]) -> UnsafeMutablePointer<ConnectionItem>? {
    var item = lace_connmgr_root(mgr)
    for index in path {
      guard let folder = lace_item_folder(item), index >= 0,
        let child = lace_folder_child(folder, index)
      else { return nil }
      item = child
    }
    return item
  }

  private func snapshot() {
    var folders: [Folder] = []
    var entries: [Entry] = []
    func walk(_ item: UnsafeMutablePointer<ConnectionItem>?, _ path: [Int]) {
      guard let item, let folder = lace_item_folder(item) else { return }
      folders.append(Folder(path: path, name: path.isEmpty ? "" : borrowCString(folder.pointee.name) ?? "Folder"))
      for i in 0..<folder.pointee.num_children {
        guard let child = lace_folder_child(folder, i) else { continue }
        if let c = lace_item_connection(child) {
          entries.append(Entry(connection: SavedConnection(c.pointee), folderPath: path))
        } else {
          walk(child, path + [i])
        }
      }
    }
    walk(lace_connmgr_root(mgr), [])
    self.folders = folders
    self.entries = entries
  }
}
