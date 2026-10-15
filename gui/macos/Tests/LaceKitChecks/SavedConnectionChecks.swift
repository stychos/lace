//
// Lace macOS frontend
// Checks: saved connections through the TUI's ConnectionManager (a private
// connections.json): the tree, every change written at once, reloading,
// connection strings with and without saved passwords, the +ssh form
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkSavedConnections(tmp: URL) {
  let path = tmp.appendingPathComponent("connections.json").path

  // A store over a file that does not exist yet is empty and writable
  let store = SavedConnectionStore(path: path)
  Checks.expect(store.loadError == nil, "fresh store: \(store.loadError ?? "")")
  Checks.expect(!store.isReadOnly, "fresh store writable")
  Checks.expect(store.isEmpty, "fresh store empty")
  Checks.equal(store.folders.count, 1, "the root folder")
  var changes = 0
  store.onChanged = { changes += 1 }

  var pg = SavedConnection(
    name: "Prod", driver: "postgres", host: "db.example.com", port: 5433, database: "shop", user: "app",
    password: "s3cret", savePassword: true)
  let lite = SavedConnection(name: "Local", driver: "sqlite", database: "/tmp/x.sqlite")
  do {
    try store.addFolder(named: "Work")
    Checks.equal(store.folders.map(\.name), ["", "Work"], "folders after add")
    let work = store.folders[1].path
    Checks.equal(work, [0], "Work's path")
    try store.add(pg, inFolder: work)
    try store.add(lite)
    Checks.equal(store.entries.map(\.connection.name), ["Prod", "Local"], "tree order")
    Checks.equal(store.entry(id: pg.id)?.folderPath, [0], "Prod in Work")
    Checks.equal(store.entry(id: lite.id)?.folderPath, [], "Local at the root")
    Checks.equal(store.entries(inFolder: [0]).count, 1, "one in Work")
    Checks.equal(changes, 3, "a change per write")

    // The file exists, owner-only, and loads back the same
    let attrs = try FileManager.default.attributesOfItem(atPath: path)
    Checks.equal((attrs[.posixPermissions] as? Int) ?? 0 & 0o777, 0o600, "connections.json is 0600")
    let again = SavedConnectionStore(path: path)
    Checks.expect(again.loadError == nil, "reload: \(again.loadError ?? "")")
    Checks.equal(again.entries.map(\.connection.name), ["Prod", "Local"], "reload order")
    Checks.equal(again.folders.map(\.name), ["", "Work"], "reload folders")
    let loaded = again.connection(id: pg.id)
    Checks.equal(loaded?.host, "db.example.com")
    Checks.equal(loaded?.port, 5433)
    Checks.equal(loaded?.password, "s3cret", "a saved password is in the file")
    Checks.equal(loaded?.savePassword, true)

    // Update by id, move between folders, remove
    pg.name = "Production"
    pg.host = "db2.example.com"
    try store.update(pg)
    Checks.equal(store.connection(id: pg.id)?.host, "db2.example.com", "update")
    try store.move(id: pg.id, toFolder: [])
    Checks.equal(store.entry(id: pg.id)?.folderPath, [], "moved to the root")
    Checks.equal(store.entries.map(\.connection.name), ["Local", "Production"], "moved to the end")
    try store.renameFolder(at: [0], to: "Office")
    Checks.equal(store.folders[1].name, "Office", "rename folder")
    try store.removeFolder(at: [0])
    Checks.equal(store.folders.count, 1, "empty folder removed")
    try store.addFolder(named: "Keep")
    let keep = store.folders.last!.path
    Checks.equal(keep, [2], "a new folder goes after the root's connections")
    try store.move(id: lite.id, toFolder: keep)
    // Paths are positions: taking Local out of the root moved Keep up
    let keepNow = store.folders.last!.path
    Checks.equal(keepNow, [1], "folder paths shift with the tree")
    Checks.equal(store.entry(id: lite.id)?.folderPath, keepNow, "moved into Keep")
    do {
      try store.removeFolder(at: keepNow)
      Checks.fail("a folder with a connection was removed")
    } catch let e as SavedConnectionStore.StoreError {
      Checks.equal(e, .folderNotEmpty, "non-empty folder refused")
    }
    try store.remove(id: lite.id)
    Checks.equal(store.entries.count, 1, "removed")
    Checks.expect(store.connection(id: lite.id) == nil, "gone")
    do {
      try store.update(lite)
      Checks.fail("updating a removed connection succeeded")
    } catch let e as SavedConnectionStore.StoreError {
      Checks.equal(e, .notFound, "update of a removed connection")
    }
  } catch {
    Checks.fail("saved connections: unexpected \(error)")
  }

  // Connection strings: lace_saved_conn_build_connstr's rules
  Checks.equal(pg.connectionString(), "postgres://app:s3cret@db2.example.com:5433/shop", "saved password in")
  pg.savePassword = false
  Checks.equal(pg.connectionString(), "postgres://app@db2.example.com:5433/shop", "password left out")
  Checks.equal(
    pg.connectionString(password: "typed"), "postgres://app:typed@db2.example.com:5433/shop", "a prompt's password")
  Checks.equal(pg.summary, "app@db2.example.com:5433/shop")
  Checks.equal(pg.displayName, "Production")
  Checks.equal(pg.driverKind, .postgres)
  var tunnel = pg
  tunnel.ssh = true
  tunnel.sshHost = "bastion"
  tunnel.sshUser = "ops"
  tunnel.sshPort = 2222
  let viaSSH = tunnel.connectionString(password: "p@ss")
  Checks.expect(viaSSH.hasPrefix("postgres+ssh://ops@bastion:2222/"), "the +ssh form: \(viaSSH)")
  Checks.expect(viaSSH.contains("app:p%40ss@db2.example.com:5433/shop"), "encoded database password: \(viaSSH)")
  Checks.expect(tunnel.summary.hasSuffix(" via ssh ops@bastion"), "summary names the tunnel: \(tunnel.summary)")
  Checks.equal(lite.connectionString(), "sqlite:///tmp/x.sqlite", "sqlite path")
  Checks.equal(lite.summary, "x.sqlite")
  Checks.equal(lite.driverKind, .sqlite)
  Checks.expect(lite.isSQLite, "sqlite kind")

  // Parsed from a string (what the user typed): fields, no saved flag
  let parsed = SavedConnection(connectionString: "mysql://root:pw@localhost:3307/inventory")
  Checks.equal(parsed?.driver, "mysql")
  Checks.equal(parsed?.host, "localhost")
  Checks.equal(parsed?.port, 3307)
  Checks.equal(parsed?.database, "inventory")
  Checks.equal(parsed?.user, "root")
  Checks.equal(parsed?.password, "pw", "password kept")
  // liblace marks a password in the string as saved; the app asks before
  // storing one (as the TUI does)
  Checks.equal(parsed?.savePassword, true, "a password in the string counts as saved")
  Checks.equal(parsed?.driverKind, .mysql)
  Checks.expect(SavedConnection(connectionString: "nonsense") == nil, "garbage does not parse")
  Checks.equal(SavedConnection.defaultPort(driver: "postgres"), 5432)
  Checks.equal(SavedConnection.defaultPort(driver: "sqlite"), 0)
  Checks.expect(SavedConnection.newID().count == 36, "a uuid")
}
