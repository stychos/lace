//
// Lace macOS frontend
// Application state: the preferences, the saved connections (and where
// their passwords live), the one daemon client, the workspaces
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
final class LaceApp {
  static let shared = LaceApp()

  var prefs = Prefs.load()
  /// Set once the app is quitting: windows closing then are not the user's doing
  var isTerminating = false
  /// connections.json and recent.json, shared with the TUI and the GTK frontend
  let saved = SavedConnectionStore()
  let recents = RecentConnections()
  private(set) var workspaces: [WorkspaceWindowController] = []
  private var workspaceCounter = 0
  private var clientTask: Task<LaceClient, Never>?
  private var preferences: PreferencesWindowController?

  func showPreferences() {
    if preferences == nil {
      preferences = PreferencesWindowController()
    }
    preferences?.showWindow(nil)
    preferences?.window?.makeKeyAndOrderFront(nil)
  }

  /// The process's client, created on first use off the main thread (it
  /// attaches to a running daemon or starts one, which can take a moment).
  /// Always answers; `isConnected` says whether a daemon is there.
  func client() async -> LaceClient {
    if let clientTask {
      return await clientTask.value
    }
    var config = LaceClient.Config()
    config.mode = .auto
    config.spawnMode = prefs.daemonSpawnMode
    config.idleTimeout = prefs.daemonIdleTimeout
    let task = Task.detached(priority: .userInitiated) { LaceClient(config: config) }
    clientTask = task
    return await task.value
  }

  @discardableResult
  func newWorkspace() -> WorkspaceWindowController {
    workspaceCounter += 1
    let name = workspaceCounter == 1 ? "Workspace" : "Workspace \(workspaceCounter)"
    let wc = WorkspaceWindowController(name: name)
    workspaces.append(wc)
    wc.showWindow(nil)
    return wc
  }

  func workspaceClosed(_ wc: WorkspaceWindowController) {
    workspaces.removeAll { $0 === wc }
  }

  // MARK: Saved connections and their passwords

  /// Where a saved password lives: the file, or the Keychain (preference)
  private static func sshAccount(_ id: String) -> String { "\(id)/ssh" }

  /// A saved connection with its passwords, from the Keychain when the
  /// file holds none (a connection saved by this app) and the flag is set
  func savedConnection(id: String) -> SavedConnection? {
    guard var c = saved.connection(id: id) else { return nil }
    if prefs.keychainPasswords {
      if c.savePassword, c.password == nil {
        c.password = Keychain.password(account: c.id)
      }
      if c.ssh, c.saveSSHPassword, c.sshPassword == nil {
        c.sshPassword = Keychain.password(account: LaceApp.sshAccount(c.id))
      }
    }
    return c
  }

  /// Add or update a saved connection. With the Keychain preference on, the
  /// passwords go there and the file keeps only the flags; off, the file
  /// holds them as the TUI writes them. Passwords not marked saved are
  /// dropped from both.
  func store(_ connection: SavedConnection, inFolder folder: [Int]? = nil) throws {
    var c = connection
    if !c.savePassword {
      c.password = nil
    }
    if !c.saveSSHPassword {
      c.sshPassword = nil
    }
    if prefs.keychainPasswords {
      if let why = Keychain.set(c.password, account: c.id) {
        throw SavedConnectionStore.StoreError.save("Keychain: \(why)")
      }
      Keychain.set(c.sshPassword, account: LaceApp.sshAccount(c.id))
      c.password = nil
      c.sshPassword = nil
    } else {
      Keychain.remove(account: c.id)
      Keychain.remove(account: LaceApp.sshAccount(c.id))
    }
    if saved.connection(id: c.id) != nil {
      try saved.update(c)
      if let folder, saved.entry(id: c.id)?.folderPath != folder {
        try saved.move(id: c.id, toFolder: folder)
      }
    } else {
      try saved.add(c, inFolder: folder ?? [])
    }
  }

  func removeSaved(id: String) throws {
    try saved.remove(id: id)
    Keychain.remove(account: id)
    Keychain.remove(account: LaceApp.sshAccount(id))
  }

  /// The workspace to act on for app-level requests (a file to open)
  var frontWorkspace: WorkspaceWindowController? {
    (NSApp.keyWindow?.windowController as? WorkspaceWindowController)
      ?? (NSApp.mainWindow?.windowController as? WorkspaceWindowController)
      ?? workspaces.last
  }
}
