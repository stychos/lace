//
// Lace macOS frontend
// Connection: one open database connection and its worker
//
// The daemon runs one statement at a time per database connection, so every
// liblace call for a connection goes through its own serial queue, in order,
// and callers await the result. The worker mirrors the GTK frontend's
// LaceConnection: before every job it declares the daemon generation its
// conn_id belongs to; when the daemon was replaced (LACE_ERR_CONNECTION_LOST
// with a new generation) it reconnects from the stored connection string and
// retries a read once; a transaction open at that moment is lost, and only a
// ROLLBACK (acknowledged, not sent) gets through until the user runs one.
// Reads inside an open PostgreSQL transaction run under SAVEPOINT lace_read,
// so a failed or cancelled page does not abort the user's transaction.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class Connection: @unchecked Sendable {
  /// Transaction state as shown in the UI (the TUI's [TX] / [TX FAILED] / [TX LOST])
  public enum TxState: Int32, Sendable {
    case none = 0
    case open = 1
    /// PostgreSQL: in error, only ROLLBACK works
    case failed = 2
    /// The daemon was replaced while one was open
    case lost = 3
  }

  public enum Event: Sendable, Equatable {
    case transactionChanged(TxState)
    /// Something the user should see (the daemon was replaced)
    case notice(String)
    /// A statement the user asked for started (true) or the last one ended
    /// (false): the lock banner (see userStatement)
    case userBusyChanged(Bool)
    case closed
  }

  /// What a statement the user asked for looks like while queued or running
  public struct UserStatement: Sendable, Equatable {
    /// The SQL, or a word for a write ("UPDATE")
    public let what: String
    public let since: Date
  }

  public let client: LaceClient
  /// May hold passwords; never shown (see displayName)
  public let connectionString: String
  public let driver: Driver
  /// Transaction changes, notices and the close, in order; ends after close().
  /// One consumer; every other one takes its own stream from subscribe().
  public let events: AsyncStream<Event>

  private let eventSink: AsyncStream<Event>.Continuation
  /// The streams subscribe() handed out (a connection shown in several windows)
  private var subscribers: [UUID: AsyncStream<Event>.Continuation] = [:]
  private let queue: DispatchQueue
  private let lock = NSLock()
  private var connID: Int32
  private var generation: UInt64
  private var txState: TxState = .none
  private var reportedTx: TxState = .none
  private var pending = 0
  /// The user's statements queued or running, oldest first (the banner's)
  private var userJobs: [(serial: Int, what: String)] = []
  private var userSince = Date()
  private var userSerial = 0
  private var closing = false
  private var closed = false

  /// Takes a connection the client already opened (see open())
  public init(client: LaceClient, connectionID: Int32, connectionString: String) {
    self.client = client
    self.connectionString = connectionString
    connID = connectionID
    generation = client.generation
    driver = Driver(connectionString: connectionString) ?? .sqlite
    queue = DispatchQueue(label: "dev.hexed.lace.connection", qos: .userInitiated)
    // Bounded: an app that reads only subscribe() streams leaves this one
    // unread for the connection's life
    let stream = AsyncStream<Event>.makeStream(bufferingPolicy: .bufferingNewest(256))
    events = stream.stream
    eventSink = stream.continuation
  }

  /// Open a database (off the caller's thread: a connect can take a minute
  /// through an SSH tunnel). password replaces the one in the connection
  /// string (a password prompt's answer).
  public static func open(client: LaceClient, connectionString: String, password: String? = nil)
    async throws -> Connection
  {
    let id: Int32 = try await withCheckedThrowingContinuation { cont in
      DispatchQueue.global(qos: .userInitiated).async {
        cont.resume(with: Result { try client.connect(connectionString, password: password) })
      }
    }
    return Connection(client: client, connectionID: id, connectionString: connectionString)
  }

  /// The daemon's conn_id (changes after a reconnect)
  public var id: Int32 { lock.withLock { connID } }
  public var transactionState: TxState { lock.withLock { txState } }
  /// Jobs queued or running
  public var isBusy: Bool { lock.withLock { pending > 0 } }
  /// The oldest statement the user asked for that is queued or running, or nil
  public var userStatement: UserStatement? {
    lock.withLock { userJobs.first.map { UserStatement(what: $0.what, since: userSince) } }
  }
  public var isClosed: Bool { lock.withLock { closed || closing } }

  /// A stream of its own for one more consumer: the events from now on, in
  /// order, ending after close() (at once when already closed)
  public func subscribe() -> AsyncStream<Event> {
    let (stream, cont) = AsyncStream<Event>.makeStream()
    let id = UUID()
    let added: Bool = lock.withLock {
      if closed { return false }
      subscribers[id] = cont
      return true
    }
    if !added {
      cont.yield(.closed)
      cont.finish()
      return stream
    }
    cont.onTermination = { [weak self] _ in
      guard let self else { return }
      _ = lock.withLock { self.subscribers.removeValue(forKey: id) }
    }
    return stream
  }

  /// To every consumer (never under the lock: a finished stream's
  /// onTermination takes it)
  private func emit(_ event: Event) {
    eventSink.yield(event)
    for sink in lock.withLock({ Array(subscribers.values) }) {
      sink.yield(event)
    }
  }

  // MARK: Jobs

  /// A statement the user wrote: run once as written, never retried. user:
  /// the user asked for it (the lock banner shows); false for the BEGIN,
  /// COMMIT and ROLLBACK around a script.
  public func exec(_ sql: String, user: Bool = true) async throws -> ResultSet {
    var job = Job<ResultSet> { [client] id in try Connection.execSQL(client, id, sql) }
    if SQL.isRollback(sql) {
      job.rollbackAck = { ResultSet.empty() }
    }
    job.user = user ? sql : nil
    return try await submit(job)
  }

  /// A read run on the user's behalf (a page, a count): retried once after a
  /// reconnect, under a savepoint inside an open PostgreSQL transaction.
  /// user: the first page of a statement the user ran (the lock banner).
  public func read(_ sql: String, user: Bool = false) async throws -> ResultSet {
    var job = Job(readOnly: true, savepoint: true) { [client] id in
      try Connection.execSQL(client, id, sql)
    }
    job.user = user ? sql : nil
    return try await submit(job)
  }

  public func tables() async throws -> [String] {
    try await submit(
      Job(readOnly: true, savepoint: true) { [client] id in
        var names: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>? = nil
        var count = 0
        try client.check(lace_list_tables(client.raw, id, &names, &count))
        defer { lace_tables_free(names, count) }
        return (0..<count).compactMap { names?[$0].map { String(cString: $0) } }
      })
  }

  public func schema(of table: String) async throws -> Schema {
    try await submit(
      Job(readOnly: true, savepoint: true) { [client] id in
        var s: UnsafeMutablePointer<LaceSchema>? = nil
        try client.check(lace_get_schema(client.raw, id, table, &s))
        guard let s else {
          throw LaceError(code: LACE_ERR_TABLE_NOT_FOUND, message: "No schema for \(table)")
        }
        return Schema(owning: s)
      })
  }

  /// COUNT(*) of a table with an optional WHERE body. With approximate and no
  /// WHERE, a large PostgreSQL/MySQL table may answer with a catalog estimate.
  /// user: asked for by the user (Information's exact count: the lock banner).
  public func count(table: String, where whereBody: String? = nil, approximate: Bool = false, user: Bool = false)
    async throws -> (count: Int64, approximate: Bool)
  {
    let driver = self.driver
    var job = Job(readOnly: true, savepoint: true) { [client] id -> (count: Int64, approximate: Bool) in
        if whereBody == nil && approximate {
          var n = 0
          var approx = true
          try client.check(lace_count(client.raw, id, table, &n, &approx))
          return (Int64(n), approx)
        }
        guard let sql = SQL.tableCount(driver: driver, table: table, where: whereBody) else {
          throw LaceError(code: LACE_ERR_INVALID_PARAMS, message: "Bad table reference: \(table)")
        }
        return (Connection.firstInt(try Connection.execSQL(client, id, sql)), false)
      }
    job.user = user ? "COUNT" : nil
    return try await submit(job)
  }

  /// A SELECT COUNT(*) ... the caller built (SQL.count(of:))
  public func count(sql: String) async throws -> Int64 {
    try await submit(
      Job(readOnly: true, savepoint: true) { [client] id in
        Connection.firstInt(try Connection.execSQL(client, id, sql))
      })
  }

  /// Verify the row (re-read by key, compared as exact edit text), then
  /// UPDATE one cell. Returns the SQL liblace sent, for history.
  public func update(
    table: String, key: RowKey, snapshot: RowSnapshot?, column: String, value: Value
  ) async throws -> String? {
    var job = Job { [client] id -> String? in
      try Connection.verify(client, id, table, key, snapshot)
      let pk = lace_row_key_to_pk(&key.raw)
      defer { free(pk) }
      try value.withLaceValue { v in
        try client.check(lace_update(client.raw, id, table, pk, key.raw.count, column, v))
      }
      return borrowCString(lace_last_sql())
    }
    job.user = "UPDATE"
    return try await submit(job)
  }

  /// Verify the row, then DELETE it. Returns the SQL liblace sent.
  public func delete(table: String, key: RowKey, snapshot: RowSnapshot?) async throws -> String? {
    var job = Job { [client] id -> String? in
      try Connection.verify(client, id, table, key, snapshot)
      let pk = lace_row_key_to_pk(&key.raw)
      defer { free(pk) }
      try client.check(lace_delete(client.raw, id, table, pk, key.raw.count))
      return borrowCString(lace_last_sql())
    }
    job.user = "DELETE"
    return try await submit(job)
  }

  /// INSERT a row; no columns inserts a row of defaults. Returns the SQL sent.
  public func insert(into table: String, columns: [String], values: [Value]) async throws
    -> String?
  {
    precondition(columns.count == values.count, "one value per column")
    var job = Job { [client] id -> String? in
        var cvalues = values.map { $0.makeLaceValue() }
        defer {
          for i in cvalues.indices {
            lace_value_free(&cvalues[i])
          }
        }
        let cstrings = columns.map { strdup($0) }
        defer {
          for s in cstrings {
            free(s)
          }
        }
        var names: [UnsafePointer<CChar>?] = cstrings.map { UnsafePointer($0) }
        // liblace wants arrays even for a row of defaults
        if names.isEmpty {
          names = [nil]
          cvalues = [lace_value_null()]
        }
        try names.withUnsafeMutableBufferPointer { n in
          try cvalues.withUnsafeBufferPointer { v in
            try client.check(
              lace_insert(client.raw, id, table, n.baseAddress, v.baseAddress, columns.count))
          }
        }
        return borrowCString(lace_last_sql())
      }
    job.user = "INSERT"
    return try await submit(job)
  }

  /// A catalog read (Catalog.swift): retried once after a reconnect, under a
  /// savepoint inside an open PostgreSQL transaction
  func submitRead<T: Sendable>(_ body: @escaping @Sendable (Int32) throws -> T) async throws -> T {
    try await submit(Job(readOnly: true, savepoint: true, body: body))
  }

  /// Cancel the statement running now (best effort; from any thread)
  @discardableResult
  public func cancel() -> Bool {
    guard isBusy, !isClosed else { return false }
    return client.cancelQuery(connectionID: id)
  }

  /// Disconnect: queued jobs still run, then the database connection is
  /// closed, the worker stops and `events` ends. Later jobs fail with
  /// LACE_ERR_CONNECTION_CLOSED.
  public func close() async throws {
    let first = lock.withLock { () -> Bool in
      if closing || closed { return false }
      closing = true
      return true
    }
    guard first else { return }
    defer {
      let sinks: [AsyncStream<Event>.Continuation] = lock.withLock {
        closed = true
        let all = Array(subscribers.values)
        subscribers.removeAll()
        return all
      }
      eventSink.yield(.closed)
      eventSink.finish()
      for sink in sinks {
        sink.yield(.closed)
        sink.finish()
      }
    }
    try await submit(
      Job(disconnect: true) { [client] id in
        try client.check(lace_disconnect(client.raw, id))
      })
  }

  // MARK: Names

  /// "sqlite: /path/db" or "postgres://user@host:5432/db via ssh user@host":
  /// never a password, never the tunnel socket
  public var displayName: String {
    guard let cs = connstr_parse(connectionString, nil) else { return "database" }
    defer { connstr_free(cs) }
    let c = cs.pointee
    if borrowCString(c.driver) == "sqlite" {
      return "sqlite: \(borrowCString(c.database) ?? "")"
    }
    var out = "\(borrowCString(c.driver) ?? "db")://"
    if let user = borrowCString(c.user) {
      out += "\(user)@"
    }
    out += borrowCString(c.host) ?? "localhost"
    if c.port > 0 {
      out += ":\(c.port)"
    }
    out += "/\(borrowCString(c.database) ?? "")"
    if c.ssh, let host = borrowCString(c.ssh_host) {
      out += " via ssh "
      if let user = borrowCString(c.ssh_user) {
        out += "\(user)@"
      }
      out += host
    }
    return out
  }

  /// "data.sqlite" or "db on host"
  public var shortName: String {
    guard let cs = connstr_parse(connectionString, nil) else { return "database" }
    defer { connstr_free(cs) }
    let c = cs.pointee
    if borrowCString(c.driver) == "sqlite" {
      let db = borrowCString(c.database) ?? ""
      let file = (db as NSString).lastPathComponent
      return file.isEmpty ? db : file
    }
    return "\(borrowCString(c.database) ?? "db") on \(borrowCString(c.host) ?? "localhost")"
  }

  /// The connection string without its passwords (session files, recents)
  public var passwordlessConnectionString: String? {
    guard let sc = lace_saved_conn_from_connstr(connectionString, nil) else {
      return takeCString(connstr_mask_password(connectionString))
    }
    defer { lace_saved_conn_free(sc) }
    sc.pointee.save_password = false
    sc.pointee.save_ssh_password = false
    return takeCString(lace_saved_conn_build_connstr(sc, nil, nil))
  }

  // MARK: Worker

  private struct Job<T>: Sendable {
    /// May be retried once after a reconnect
    var readOnly = false
    /// SAVEPOINT inside an open PostgreSQL transaction
    var savepoint = false
    var disconnect = false
    /// For a ROLLBACK: the answer that acknowledges a lost transaction
    var rollbackAck: (@Sendable () -> T)? = nil
    /// A statement the user asked for: what the lock banner shows
    var user: String? = nil
    let body: @Sendable (Int32) throws -> T
  }

  private func submit<T>(_ job: Job<T>) async throws -> T {
    let (accepted, userSerial, firstUser) = lock.withLock { () -> (Bool, Int, Bool) in
      if closed || (closing && !job.disconnect) { return (false, 0, false) }
      pending += 1
      guard let what = job.user else { return (true, 0, false) }
      self.userSerial += 1
      let first = userJobs.isEmpty
      if first { userSince = Date() }
      userJobs.append((self.userSerial, what))
      return (true, self.userSerial, first)
    }
    guard accepted else {
      throw LaceError(code: LACE_ERR_CONNECTION_CLOSED, message: "The connection is closed")
    }
    if firstUser {
      emit(.userBusyChanged(true))
    }
    return try await withCheckedThrowingContinuation { cont in
      queue.async { [self] in
        let outcome = Result { try self.run(job) }
        let lastUser: Bool = self.lock.withLock {
          self.pending -= 1
          guard job.user != nil, let i = self.userJobs.firstIndex(where: { $0.serial == userSerial }) else {
            return false
          }
          self.userJobs.remove(at: i)
          self.userSince = Date()  // the next one starts about now
          return self.userJobs.isEmpty
        }
        if lastUser {
          self.emit(.userBusyChanged(false))
        }
        cont.resume(with: outcome)
      }
    }
  }

  /// One job, on the queue: the generation check, the lost-transaction gate,
  /// the savepoint, the call, one reconnect-and-retry, the transaction state
  private func run<T>(_ job: Job<T>) throws -> T {
    let (id, gen, tx) = lock.withLock { (connID, generation, txState) }
    client.expectGeneration(gen)

    if tx == .lost && !job.disconnect {
      if let ack = job.rollbackAck {
        setTx(.none)
        return ack()
      }
      throw LaceError(
        code: LACE_ERR_CONNECTION_LOST,
        message: "The transaction was lost when the daemon was replaced. Run ROLLBACK to acknowledge.")
    }

    var savepoint = job.savepoint && driver == .postgres && tx == .open
    if savepoint && lace_exec(client.raw, id, "SAVEPOINT lace_read", nil) != LACE_OK {
      savepoint = false
    }

    var outcome = Result { try job.body(id) }
    if case .failure(let error as LaceError) = outcome, error.isConnectionLost,
      client.generation != gen
    {
      savepoint = false  // the old daemon's savepoint went with it
      outcome = Result {
        try reconnect(readOnly: job.readOnly)
        return try job.body(self.id)
      }
    }

    if !job.disconnect {
      noteTx()
    }
    if savepoint {
      var ok = false
      if case .success = outcome { ok = true }
      let rc = lace_exec(
        client.raw, self.id,
        ok ? "RELEASE SAVEPOINT lace_read" : "ROLLBACK TO SAVEPOINT lace_read", nil)
      if rc == LACE_OK {
        noteTx()  // the failed read had marked it failed; live again
      }
    }
    return try outcome.get()
  }

  /// The daemon was replaced: open the database again. Throws unless the
  /// job may be retried (a read, with no transaction lost).
  private func reconnect(readOnly: Bool) throws {
    var newID: Int32 = -1
    guard lace_connect(client.raw, connectionString, nil, &newID) == LACE_OK else {
      throw LaceError(
        code: LACE_ERR_CONNECTION_LOST,
        message: "The daemon was replaced and reconnecting failed: \(client.lastError)")
    }
    let txWasOpen: Bool = lock.withLock {
      connID = newID
      generation = client.generation
      let open = txState == .open || txState == .failed
      txState = open ? .lost : .none
      return open
    }
    client.expectGeneration(lock.withLock { generation })
    emit(
      .notice(
        txWasOpen
          ? "The daemon was replaced. The open transaction is lost."
          : "The daemon was replaced; the connection was reopened."))
    reportTxIfChanged()
    if txWasOpen {
      throw LaceError(
        code: LACE_ERR_CONNECTION_LOST,
        message:
          "The daemon was replaced while a transaction was open. Its work is lost: run ROLLBACK to acknowledge."
      )
    }
    if !readOnly {
      throw LaceError(
        code: LACE_ERR_CONNECTION_LOST, message: "The daemon was replaced; reconnected. Please retry.")
    }
  }

  /// Record the transaction state liblace saw after the last statement
  private func noteTx() {
    let state = lace_last_transaction_state()
    guard state >= 0, let tx = TxState(rawValue: state) else { return }
    lock.withLock {
      if txState != .lost {
        txState = tx
      }
    }
    reportTxIfChanged()
  }

  private func setTx(_ tx: TxState) {
    lock.withLock { txState = tx }
    reportTxIfChanged()
  }

  private func reportTxIfChanged() {
    let change: TxState? = lock.withLock {
      if txState == reportedTx { return nil }
      reportedTx = txState
      return txState
    }
    if let change {
      emit(.transactionChanged(change))
    }
  }

  // MARK: liblace calls (worker thread)

  static func execSQL(_ client: LaceClient, _ id: Int32, _ sql: String) throws -> ResultSet {
    var r: UnsafeMutablePointer<LaceResult>? = nil
    try client.check(lace_exec(client.raw, id, sql, &r))
    return r.map { ResultSet(owning: $0) } ?? ResultSet.empty()
  }

  static func firstInt(_ r: ResultSet) -> Int64 {
    guard r.rowCount > 0, !r.columns.isEmpty else { return -1 }
    switch r.value(row: 0, column: 0) {
    case .int(let i): return i
    case .decimal(let s), .text(let s): return Int64(s) ?? -1
    case .float(let d): return Int64(d)
    default: return -1
    }
  }

  /// The row must still be the one on screen: exactly one row by key, its
  /// snapshot columns equal as exact edit text
  static func verify(
    _ client: LaceClient, _ id: Int32, _ table: String, _ key: RowKey, _ snapshot: RowSnapshot?
  ) throws {
    var why: UnsafeMutablePointer<CChar>? = nil
    let ok = withUnsafePointer(to: &key.raw) { k -> Bool in
      if let snapshot {
        return withUnsafePointer(to: &snapshot.raw) { s in
          lace_row_key_verify(client.raw, id, table, k, s, &why)
        }
      }
      return lace_row_key_verify(client.raw, id, table, k, nil, &why)
    }
    if !ok {
      throw LaceError(code: LACE_ERR_ROW_NOT_FOUND, message: takeCString(why) ?? "row check failed")
    }
  }
}

/// Whether two connections are the same database (the TUIs' conn_key_matches):
/// by saved id when either has one, else by the connection string without
/// its passwords
public enum ConnectionKey {
  public static func matches(savedA: String?, _ a: String?, savedB: String?, _ b: String?) -> Bool {
    conn_key_matches(savedA, a, savedB, b)
  }

  /// The connection string without its passwords (the key of a typed URL)
  public static func passwordless(_ connectionString: String) -> String {
    RecentConnections.stripPasswords(connectionString) ?? connectionString
  }
}
