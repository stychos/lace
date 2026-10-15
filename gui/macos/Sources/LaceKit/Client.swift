//
// Lace macOS frontend
// LaceClient: the process's link to the daemon
//
// One per app, shared by every Connection, as the TUI and the GTK frontend
// do. liblace borrows a link to the daemon per call, so calls from different
// threads run concurrently; its error message is per thread, which is why
// `check` reads it right on the thread that made the failing call.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class LaceClient: @unchecked Sendable {
  /// How to reach the daemon
  public enum Mode: Sendable {
    /// Attach to a running daemon, else start one in the spawn mode (the default)
    case auto
    /// A Unix socket; nil is the default per-user path
    case unix(socketPath: String?)
    /// A TCP daemon (token handshake); nil host is localhost, port 0 the default
    case tcp(host: String?, port: Int32)
  }

  /// How a daemon is started when none answers (config.json's daemon_spawn_mode)
  public enum SpawnMode: Int32, Sendable {
    /// A shared Unix-socket daemon that outlives us (default)
    case unix = 0
    /// A private daemon over pipes that exits with us
    case stdio = 1
    /// A shared TCP daemon
    case tcp = 2
    /// A running daemon if there is one, else a private stdio daemon: never
    /// leaves a daemon behind
    case detect = 3

    var c: LaceSpawnMode { LaceSpawnMode(rawValue: UInt32(rawValue)) }
  }

  public struct Config: Sendable {
    public var mode: Mode = .auto
    public var spawnMode: SpawnMode = .unix
    /// laced to start; nil finds it next to the executable, then on PATH
    public var daemonPath: String? = nil
    /// A spawned socket daemon exits after this many idle seconds (0: never)
    public var idleTimeout: Int32 = 0
    /// Connection timeout to the daemon in ms (0: liblace's default, 5 s)
    public var connectTimeoutMs: Int32 = 0
    /// Unix mode: start a daemon when the default socket has none
    public var spawnIfMissing = true

    public init() {}

    public init(spawnMode: SpawnMode, daemonPath: String? = nil, idleTimeout: Int32 = 0) {
      self.spawnMode = spawnMode
      self.daemonPath = daemonPath
      self.idleTimeout = idleTimeout
    }
  }

  let raw: OpaquePointer

  /// Always succeeds in creating the handle; `isConnected` says whether a
  /// daemon answered and `lastError` why not.
  public init(config: Config = Config()) {
    var c = LaceClientConfig()
    var strings: [UnsafeMutablePointer<CChar>] = []
    func keep(_ s: String?) -> UnsafePointer<CChar>? {
      guard let s, let p = strdup(s) else { return nil }
      strings.append(p)
      return UnsafePointer(p)
    }
    switch config.mode {
    case .auto:
      c.mode = LACE_CONN_AUTO
    case .unix(let path):
      c.mode = LACE_CONN_UNIX
      c.socket_path = keep(path)
      c.spawn_if_missing = config.spawnIfMissing
    case .tcp(let host, let port):
      c.mode = LACE_CONN_TCP
      c.host = keep(host)
      c.port = port
    }
    c.spawn_mode = config.spawnMode.c
    c.daemon_path = keep(config.daemonPath)
    c.idle_timeout = config.idleTimeout
    c.connect_timeout_ms = config.connectTimeoutMs
    // Never NULL: failure is reported through lace_client_connected()
    raw = lace_client_create_with_config(&c)!
    for p in strings {
      free(p)
    }
  }

  deinit {
    // Never shuts a shared socket daemon down; a stdio daemon is ours and ends here
    lace_client_destroy(raw)
  }

  public var isConnected: Bool { lace_client_connected(raw) }

  /// Why the calling thread's last liblace call failed (per thread, like errno)
  public var lastError: String {
    borrowCString(lace_client_error(raw)) ?? "unknown error"
  }

  /// Bumped whenever liblace had to replace the daemon; connection ids of an
  /// older generation are dead
  public var generation: UInt64 { lace_client_generation(raw) }

  /// Which generation the calling thread's connection ids belong to: calls
  /// carrying an older id fail with LACE_ERR_CONNECTION_LOST instead of
  /// reaching a different database that reuses the id
  public func expectGeneration(_ generation: UInt64) {
    lace_expect_generation(raw, generation)
  }

  /// Reply timeout for every call but queries (ms; 0 waits forever)
  public var timeoutMs: Int32 {
    get { lace_get_timeout(raw) }
    set { lace_set_timeout(raw, newValue) }
  }

  /// Reply timeout for queries (ms; 0, the default, waits as long as they run)
  public func setQueryTimeout(ms: Int32) {
    lace_set_query_timeout(raw, ms)
  }

  public func ping() throws {
    try check(lace_ping(raw))
  }

  public func version() throws -> String {
    var p: UnsafeMutablePointer<CChar>? = nil
    try check(lace_version(raw, &p))
    return takeCString(p) ?? ""
  }

  /// Open a database connection (blocks: up to a minute for SSH tunnels).
  /// Prefer Connection.open(), which runs it off the caller's thread.
  public func connect(_ connectionString: String, password: String? = nil) throws -> Int32 {
    var id: Int32 = -1
    try check(lace_connect(raw, connectionString, password, &id))
    return id
  }

  public func disconnect(_ id: Int32) throws {
    try check(lace_disconnect(raw, id))
  }

  /// Best effort: the statement may still run for a moment. Safe from any
  /// thread while the connection's worker waits for the reply.
  @discardableResult
  public func cancelQuery(connectionID id: Int32) -> Bool {
    lace_cancel_query(raw, id) == LACE_OK
  }

  /// The canonical per-user socket path (not a search)
  public static var defaultSocketPath: String? {
    takeCString(lace_get_default_socket_path())
  }

  /// True if a daemon answers at the socket (nil: the default path)
  public static func daemonIsRunning(socketPath: String? = nil) -> Bool {
    lace_daemon_is_running(socketPath)
  }

  /// Turn a liblace return code into a thrown LaceError. Call it on the
  /// thread that made the call: the message is per thread.
  func check(_ rc: Int32) throws {
    if rc != LACE_OK {
      throw LaceError(code: rc, message: lastError)
    }
  }
}
