//
// Lace macOS frontend
// Checks: a Connection against a private stdio daemon and a temporary
// SQLite database: reads, schema, verified writes, transaction state,
// cancel, close
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkConnection(laced: String, tmp: URL) async {
  var config = LaceClient.Config(spawnMode: .stdio, daemonPath: laced)
  config.mode = .auto
  let client = LaceClient(config: config)
  Checks.expect(client.isConnected, "stdio daemon answers: \(client.lastError)")
  guard client.isConnected else { return }
  Checks.expect(!((try? client.version()) ?? "").isEmpty, "version")
  Checks.expect((try? client.ping()) != nil, "ping")

  let dbPath = tmp.appendingPathComponent("checks.sqlite").path
  let url = "sqlite://\(dbPath)?create=1"
  do {
    try await roundTrip(client: client, url: url)
  } catch {
    Checks.fail("round trip: unexpected error \(error)")
  }
  do {
    try await cancelAndRecover(client: client, url: url)
  } catch {
    Checks.fail("cancel: unexpected error \(error)")
  }
  do {
    try await subscribers(client: client, url: url)
  } catch {
    Checks.fail("subscribers: unexpected error \(error)")
  }
}

/// A connection shown in two windows: each consumer has its own stream with
/// every event in order, both end at the close; a late one ends at once
private func subscribers(client: LaceClient, url: String) async throws {
  let conn = try await Connection.open(client: client, connectionString: url)
  let a = conn.subscribe()
  let b = conn.subscribe()
  _ = try await conn.exec("BEGIN")
  _ = try await conn.exec("ROLLBACK")
  try await conn.close()
  func collect(_ s: AsyncStream<Connection.Event>) async -> [Connection.Event] {
    var out: [Connection.Event] = []
    for await e in s {
      if case .userBusyChanged = e { continue }
      out.append(e)
    }
    return out
  }
  let want: [Connection.Event] = [.transactionChanged(.open), .transactionChanged(.none), .closed]
  Checks.equal(await collect(a), want, "first subscriber")
  Checks.equal(await collect(b), want, "second subscriber")
  Checks.equal(await collect(conn.subscribe()), [.closed], "a subscriber after the close")
}

private func roundTrip(client: LaceClient, url: String) async throws {
  let conn = try await Connection.open(client: client, connectionString: url)
  Checks.equal(conn.driver, .sqlite)
  Checks.equal(conn.shortName, "checks.sqlite")
  Checks.expect(conn.displayName.hasPrefix("sqlite: /"), "display name: \(conn.displayName)")
  Checks.expect(conn.passwordlessConnectionString?.hasPrefix("sqlite://") == true, "passwordless connstr")

  // Writes answer with a count, reads with rows
  let created = try await conn.exec(
    "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL, data BLOB)")
  Checks.expect(!created.hasRows, "CREATE has no rows")
  let inserted = try await conn.exec(
    "INSERT INTO t (name, score, data) VALUES ('alice', 1.5, X'DEAD'), ('bob', NULL, NULL)")
  Checks.equal(inserted.rowsAffected, 2, "rows affected")

  Checks.equal(try await conn.tables(), ["t"])
  let schema = try await conn.schema(of: "t")
  Checks.equal(schema.columns.map(\.name), ["id", "name", "score", "data"])
  Checks.expect(schema.columns[0].primaryKey && schema.columns[0].type == .int, "id is the INT key")
  Checks.expect(schema.columns[0].autoIncrement, "sole INTEGER PRIMARY KEY auto-increments")
  Checks.expect(!schema.columns[1].nullable, "name NOT NULL")

  let rows = try await conn.read("SELECT * FROM t ORDER BY id")
  Checks.expect(rows.hasRows && rows.rowCount == 2, "two rows")
  Checks.equal(rows.columns.map(\.name), ["id", "name", "score", "data"])
  Checks.equal(rows.value(row: 0, column: 0), .int(1))
  Checks.equal(rows.value(row: 0, column: 1), .text("alice"))
  Checks.equal(rows.value(row: 0, column: 2), .float(1.5))
  Checks.equal(rows.value(row: 0, column: 3), .blob(Data([0xde, 0xad])))
  Checks.equal(rows.value(row: 1, column: 2), .null)
  Checks.equal(rows.row(1)[1], .text("bob"))
  Checks.expect(rows.hasRows && rows.rowsAffected <= 0, "a SELECT is rows, not a count")

  let (n, approx) = try await conn.count(table: "t", approximate: true)
  Checks.equal(n, 2)
  Checks.expect(!approx, "SQLite counts exactly")
  let filtered = try await conn.count(table: "t", where: "score IS NULL")
  Checks.expect(filtered.count == 1 && !filtered.approximate, "filtered count: \(filtered)")
  Checks.equal(try await conn.count(sql: SQL.count(of: "SELECT * FROM t")), 2)

  // A verified update by key: the key is found by column name
  let key = try RowKey.build(for: 0, in: rows, schema: schema, uniqueNames: true)
  Checks.equal(key.columnNames, ["id"])
  Checks.equal(key.values, [.int(1)])
  Checks.equal(key.copy().values, [.int(1)])
  let snapshot = RowSnapshot(result: rows, row: 0, column: 1, schema: schema)
  Checks.equal(snapshot.count, 1)
  let updateSQL = try await conn.update(
    table: "t", key: key, snapshot: snapshot, column: "name", value: .text("alicia"))
  Checks.expect(updateSQL?.hasPrefix("UPDATE") == true, "update SQL recorded: \(updateSQL ?? "nil")")
  Checks.equal(try await conn.read("SELECT name FROM t WHERE id = 1").value(row: 0, column: 0), .text("alicia"))

  // The snapshot is stale now: the write is refused, nothing changes
  await Checks.throwsLaceError(
    "stale snapshot",
    {
      _ = try await conn.update(table: "t", key: key, snapshot: snapshot, column: "name", value: .text("x"))
    }, matching: { $0.isRowNotFound && $0.message.contains("changed") })
  Checks.equal(try await conn.read("SELECT name FROM t WHERE id = 1").value(row: 0, column: 0), .text("alicia"))

  // Delete, verified over every table column
  let fresh = try await conn.read("SELECT * FROM t ORDER BY id")
  let bobKey = try RowKey.build(for: 1, in: fresh, schema: schema, uniqueNames: true)
  let all = RowSnapshot(result: fresh, row: 1, column: nil, schema: schema)
  Checks.equal(all.count, 4)
  let deleteSQL = try await conn.delete(table: "t", key: bobKey, snapshot: all)
  Checks.expect(deleteSQL?.hasPrefix("DELETE") == true, "delete SQL recorded")
  Checks.equal(try await conn.count(sql: "SELECT COUNT(*) FROM t"), 1)
  await Checks.throwsLaceError(
    "deleting a row that is gone",
    { _ = try await conn.delete(table: "t", key: bobKey, snapshot: nil) },
    matching: { $0.isRowNotFound })

  // Insert, including a BLOB and an explicit NULL; then a row of defaults fails
  // (name is NOT NULL), and the error carries SQLite's message
  _ = try await conn.insert(into: "t", columns: ["name", "score", "data"], values: [.text("carol"), .null, .blob(Data([1, 2, 3]))])
  let carol = try await conn.read("SELECT score, data FROM t WHERE name = 'carol'")
  Checks.equal(carol.value(row: 0, column: 0), .null)
  Checks.equal(carol.value(row: 0, column: 1), .blob(Data([1, 2, 3])))
  await Checks.throwsLaceError(
    "defaults insert violates NOT NULL", { _ = try await conn.insert(into: "t", columns: [], values: []) },
    matching: { $0.message.localizedCaseInsensitiveContains("not null") })

  // Transaction state follows the replies
  Checks.equal(conn.transactionState, .none)
  _ = try await conn.exec("BEGIN")
  Checks.equal(conn.transactionState, .open)
  let inTx = try await conn.exec("UPDATE t SET score = 9 WHERE name = 'carol'")
  Checks.expect(inTx.inTransaction, "reply says a transaction is open")
  _ = try await conn.exec("ROLLBACK")
  Checks.equal(conn.transactionState, .none)
  Checks.equal(try await conn.read("SELECT score FROM t WHERE name = 'carol'").value(row: 0, column: 0), .null)

  // Keys that cannot address a row
  _ = try await conn.exec("CREATE TABLE nokey (a TEXT); INSERT INTO nokey VALUES ('x')")
  let nokeySchema = try await conn.schema(of: "nokey")
  let nokeyRows = try await conn.read("SELECT * FROM nokey")
  do {
    _ = try RowKey.build(for: 0, in: nokeyRows, schema: nokeySchema, uniqueNames: true)
    Checks.fail("a table without a key gave a key")
  } catch RowKeyProblem.none {
  } catch {
    Checks.fail("no key: wrong error \(error)")
  }
  _ = try await conn.exec("CREATE TABLE nullkey (k TEXT PRIMARY KEY, v INT); INSERT INTO nullkey VALUES (NULL, 1)")
  let nullkeySchema = try await conn.schema(of: "nullkey")
  let nullkeyRows = try await conn.read("SELECT * FROM nullkey")
  do {
    _ = try RowKey.build(for: 0, in: nullkeyRows, schema: nullkeySchema, uniqueNames: true)
    Checks.fail("a NULL key part gave a key")
  } catch RowKeyProblem.unusable(let reason) {
    Checks.expect(reason.contains("NULL"), "NULL key reason: \(reason)")
  } catch {
    Checks.fail("NULL key: wrong error \(error)")
  }
  // A key column that appears twice is refused with uniqueNames, taken without
  let twice = try await conn.read("SELECT id, id, name FROM t")
  Checks.expect((try? RowKey.build(for: 0, in: twice, schema: schema, uniqueNames: true)) == nil, "duplicate key column refused")
  Checks.expect((try? RowKey.build(for: 0, in: twice, schema: schema, uniqueNames: false)) != nil, "duplicate key column taken")

  // A failing statement is an error with the database's message, and the
  // connection stays usable
  await Checks.throwsLaceError(
    "syntax error", { _ = try await conn.exec("SELEC 1") },
    matching: { $0.message.localizedCaseInsensitiveContains("syntax") })
  Checks.equal(try await conn.read("SELECT 1").value(row: 0, column: 0), .int(1))

  // Close: later jobs are refused, the event stream ends with the tx changes
  Checks.expect(!conn.isClosed, "open before close")
  try await conn.close()
  Checks.expect(conn.isClosed, "closed after close")
  await Checks.throwsLaceError(
    "job after close", { _ = try await conn.exec("SELECT 1") }, matching: { $0.isConnectionClosed })
  var events: [Connection.Event] = []
  for await e in conn.events {
    events.append(e)
  }
  // The user's statements (exec, the writes) start and end the lock banner's
  // state in pairs; the reads (pages, counts, schema) raise none
  let busy = events.compactMap { e -> Bool? in
    if case .userBusyChanged(let on) = e { return on }
    return nil
  }
  Checks.expect(!busy.isEmpty && busy.first == true && busy.last == false, "user statements start and end: \(busy)")
  Checks.equal(busy.filter { $0 }.count, busy.filter { !$0 }.count, "every start has its end")
  Checks.expect(conn.userStatement == nil, "no user statement after close")
  let rest = events.filter { e in
    if case .userBusyChanged = e { return false }
    return true
  }
  Checks.equal(rest, [.transactionChanged(.open), .transactionChanged(.none), .closed])
}

private func cancelAndRecover(client: LaceClient, url: String) async throws {
  let conn = try await Connection.open(client: client, connectionString: url)
  defer { Task { try? await conn.close() } }
  // About 200 M steps: tens of seconds unless cancelled
  let slow =
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 200000000) SELECT count(*) FROM c"
  let running = Task { try await conn.exec(slow) }
  try await Task.sleep(for: .milliseconds(400))
  Checks.expect(conn.isBusy, "busy while the slow statement runs")
  Checks.expect(conn.cancel(), "cancel sent")
  do {
    _ = try await running.value
    Checks.fail("the slow statement was not cancelled")
  } catch let e as LaceError {
    Checks.expect(e.isCancelled, "cancelled: \(e.code) \(e.message)")
  }
  Checks.expect(!conn.isBusy, "idle after the cancel")
  Checks.equal(try await conn.read("SELECT 2").value(row: 0, column: 0), .int(2), "usable after a cancel")
  Checks.expect(!conn.cancel(), "nothing to cancel when idle")
}
