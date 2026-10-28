//
// Lace macOS frontend
// ConnectionRegistry: the database connections open in the app and who
// holds them. One Connection per saved connection (or per password-less
// connection string for a typed URL) is shared by every strip entry that
// shows it, in any window; each entry holds it, and the last release
// closes it (the daemon rolls an open transaction back). The GTK
// frontend's conn_registry.c, in Swift.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
final class ConnectionRegistry {
  private struct Entry {
    let connection: Connection
    /// The saved connection it was opened from
    let savedID: String?
    var holders: Int
  }

  private var entries: [Entry] = []
  /// Opens in flight, by key: a second open of the same database waits for the first
  private var opening: [String: Task<Connection?, Never>] = [:]

  /// The open connections
  var count: Int { entries.count }

  /// An open one for this target: with a saved id, the one opened from it
  /// whose string still points where this one does (an edited entry is
  /// connected anew); without, one with no saved id and the same
  /// password-less string. nil if none.
  func find(savedID: String?, connectionString: String) -> Connection? {
    let byID = !(savedID ?? "").isEmpty
    return entries.first { e in
      guard !e.connection.isClosed else { return false }
      if byID {
        return e.savedID == savedID
          && ConnectionKey.matches(savedA: nil, connectionString, savedB: nil, e.connection.connectionString)
      }
      return ConnectionKey.matches(savedA: nil, connectionString, savedB: e.savedID, e.connection.connectionString)
    }?.connection
  }

  func savedID(of c: Connection) -> String? {
    entries.first { $0.connection === c }?.savedID
  }

  func holders(_ c: Connection) -> Int {
    entries.first { $0.connection === c }?.holders ?? 0
  }

  func hold(_ c: Connection) {
    if let i = entries.firstIndex(where: { $0.connection === c }) {
      entries[i].holders += 1
    }
  }

  /// A connection that found no place (its window went while it opened)
  /// is closed; one somebody holds is left alone
  func releaseIfUnheld(_ c: Connection) {
    // A turn later: another caller waiting on the same open may hold it first
    DispatchQueue.main.async { [self] in
      if let e = entries.first(where: { $0.connection === c }), e.holders == 0 {
        release(c)
      }
    }
  }

  /// The last release closes it and drops it; true then
  @discardableResult
  func release(_ c: Connection) -> Bool {
    guard let i = entries.firstIndex(where: { $0.connection === c }) else { return false }
    if entries[i].holders > 1 {
      entries[i].holders -= 1
      return false
    }
    entries.remove(at: i)
    Task { try? await c.close() }
    return true
  }

  /// The open connection for this target, else a new one: connecting asks
  /// for a password when the server (or the SSH host) refuses one and
  /// retries with the string rebuilt around it, so a tunnel is kept. A new
  /// connection joins with no holder: the caller holds it. nil when it
  /// failed or was cancelled (the error went to onError, else an alert
  /// over window).
  func open(
    connectionString: String, saved: SavedConnection?, window: NSWindow?,
    onError: ((String) -> Void)? = nil
  ) async -> Connection? {
    if let open = find(savedID: saved?.id, connectionString: connectionString) {
      if saved == nil {
        LaceApp.shared.recents.add(connectionString)
      }
      return open
    }
    let key = saved.map { "id:\($0.id)" } ?? "cs:\(ConnectionKey.passwordless(connectionString))"
    if let inFlight = opening[key] {
      return await inFlight.value
    }
    let task = Task {
      await connect(connectionString: connectionString, saved: saved, window: window, onError: onError)
    }
    opening[key] = task
    let c = await task.value
    opening[key] = nil
    return c
  }

  private func connect(
    connectionString: String, saved: SavedConnection?, window: NSWindow?, onError: ((String) -> Void)?
  ) async -> Connection? {
    func fail(_ title: String, _ message: String) {
      if let onError {
        onError(message)
        Alerts.logger?(title, message)
      } else {
        Alerts.error(title, message, window: window)
      }
    }
    let client = await LaceApp.shared.client()
    guard client.isConnected else {
      fail("No daemon", "The daemon did not answer: \(client.lastError)")
      return nil
    }
    var cs = connectionString
    var attempts = 0
    // What the prompts were answered with, both kept (a tunnel may need both)
    var typedPassword: String?
    var typedSSHPassword: String?
    while true {
      do {
        let conn = try await Connection.open(client: client, connectionString: cs)
        // Another open of the same database landed first: that one is used
        if let first = find(savedID: saved?.id, connectionString: conn.connectionString) {
          Task { try? await conn.close() }
          return first
        }
        entries.append(Entry(connection: conn, savedID: saved?.id, holders: 0))
        if saved == nil, let url = conn.passwordlessConnectionString {
          LaceApp.shared.recents.add(url)
        }
        return conn
      } catch let error as LaceError where attempts < 4 && (error.isAuthError || Self.looksLikeSSHAuth(error.message)) {
        attempts += 1
        let ssh = !error.isAuthError
        guard
          let typed = await Alerts.prompt(
            ssh ? "SSH password" : "Password", error.message, secure: true, action: "Connect", window: window)
        else { return nil }
        guard let model = saved ?? SavedConnection(connectionString: cs) else {
          fail("Could not connect", "The connection string could not be parsed")
          return nil
        }
        if ssh {
          typedSSHPassword = typed
        } else {
          typedPassword = typed
        }
        cs = model.connectionString(password: typedPassword, sshPassword: typedSSHPassword)
      } catch {
        fail("Could not connect", "\(error)")
        return nil
      }
    }
  }

  /// The daemon passes ssh's own line ("Permission denied") for a refused tunnel login
  private static func looksLikeSSHAuth(_ message: String) -> Bool {
    let l = message.lowercased()
    return l.contains("permission denied") || l.contains("ssh")
  }
}
