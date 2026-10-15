//
// Lace macOS frontend
// Checks: query history (liblace's entries, the TUI's history/<id>.json)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkHistory(tmp: URL) {
  let id = SavedConnection.newID()
  let h = History(connectionID: id, mode: .persistent, maxSize: 10)
  Checks.expect(h.lastError == nil, "fresh history: \(h.lastError ?? "")")
  Checks.expect(h.isPersistent, "persistent with an id")
  Checks.equal(h.count, 0)
  var changes = 0
  h.onChanged = { changes += 1 }

  // Kinds are detected from the SQL
  h.record("SELECT * FROM t")
  h.record("  INSERT INTO t VALUES (1)  ")
  h.record("UPDATE t SET a = 1 WHERE id = 1")
  h.record("DELETE FROM t WHERE id = 1")
  h.record("CREATE TABLE u (id INT)")
  h.record("PRAGMA table_info(t)")
  h.record("")
  Checks.equal(h.count, 6, "empty text is not recorded")
  Checks.equal(h.entries.map(\.kind), [.select, .insert, .update, .delete, .ddl, .query], "kinds")
  Checks.equal(h.entries[1].sql, "INSERT INTO t VALUES (1)", "trimmed")
  Checks.expect(abs(h.entries[0].date.timeIntervalSinceNow) < 60, "timestamped now")
  Checks.equal(changes, 6, "a change per record")
  Checks.equal(History.Kind.select.tag, "SEL")

  // The file is under the private HOME's data directory and loads back
  guard let path = h.filePath else {
    Checks.fail("no history file path")
    return
  }
  Checks.expect(path.hasPrefix(tmp.path), "history file under the private HOME: \(path)")
  Checks.expect(path.hasSuffix("/history/\(id).json"), "named by the id: \(path)")
  Checks.expect(FileManager.default.fileExists(atPath: path), "history file written")
  let again = History(connectionID: id, mode: .persistent, maxSize: 10)
  Checks.equal(again.count, 6, "reloaded")
  Checks.equal(again.entries.map(\.sql), h.entries.map(\.sql), "same statements")
  Checks.equal(again.entries.map(\.kind), h.entries.map(\.kind), "same kinds")

  // The size limit trims the oldest; remove and clear persist
  for i in 0..<10 {
    h.record("SELECT \(i)")
  }
  Checks.equal(h.count, 10, "trimmed to the maximum")
  Checks.equal(h.entries.first?.sql, "SELECT 0", "oldest dropped first")
  h.remove(at: 0)
  Checks.equal(h.entries.first?.sql, "SELECT 1", "remove")
  Checks.equal(History(connectionID: id, mode: .persistent, maxSize: 10).count, 9, "remove persisted")
  h.clear()
  Checks.equal(h.count, 0, "clear")
  Checks.equal(History(connectionID: id, mode: .persistent, maxSize: 10).count, 0, "clear persisted")

  // Session mode keeps entries in memory only; off records nothing
  let session = History(connectionID: id, mode: .session, maxSize: 10)
  Checks.expect(!session.isPersistent, "session mode is not persistent")
  session.record("SELECT 1")
  Checks.equal(session.count, 1)
  Checks.equal(History(connectionID: id, mode: .persistent, maxSize: 10).count, 0, "session mode wrote no file")
  let off = History(connectionID: id, mode: .off)
  off.record("SELECT 1")
  Checks.equal(off.count, 0, "off records nothing")
  let unsaved = History(connectionID: nil, mode: .persistent)
  Checks.expect(!unsaved.isPersistent, "no id: session only")
  unsaved.record("SELECT 1")
  Checks.equal(unsaved.count, 1)
  Checks.expect(unsaved.filePath == nil, "no file without an id")
  let badID = History(connectionID: "../etc/passwd", mode: .persistent)
  Checks.expect(badID.connectionID == nil && !badID.isPersistent, "an unsafe id is dropped")
}
