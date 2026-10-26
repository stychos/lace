//
// Lace macOS frontend
// LaceKit checks: values, SQL, the row model, saved connections and history
// (no daemon), then a Connection against a private stdio daemon and a
// temporary SQLite database. `make test` runs it;
// LACE_TEST_LACED names the daemon (default: laced/build/laced in the tree).
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

// A private HOME: default sockets, token files and logs never touch the
// user's; the daemon's log stays with the run for a failure
let tmp = FileManager.default.temporaryDirectory
  .appendingPathComponent("lace-checks-\(getpid())", isDirectory: true)
try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
setenv("HOME", tmp.path, 1)
setenv("XDG_RUNTIME_DIR", "", 1)
setenv("LACED_LOG", tmp.appendingPathComponent("laced.log").path, 1)

func findLaced() -> String? {
  if let env = ProcessInfo.processInfo.environment["LACE_TEST_LACED"], !env.isEmpty {
    return env
  }
  // gui/macos/Tests/LaceKitChecks/main.swift -> the tree's root
  let root = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
  let path = root.appendingPathComponent("laced/build/laced").path
  return FileManager.default.isExecutableFile(atPath: path) ? path : nil
}

checkValues()
checkSQL()
checkRowModel()
checkSavedConnections(tmp: tmp)
checkHistory(tmp: tmp)
checkRecents()
checkKeyset()
checkDdl()
if let laced = findLaced() {
  await checkConnection(laced: laced, tmp: tmp)
} else {
  Checks.fail("laced not found: `make -C laced`, or set LACE_TEST_LACED")
}

let status = Checks.report("lacekit")
if status == 0 {
  try? FileManager.default.removeItem(at: tmp)
} else {
  print("kept \(tmp.path) (laced.log inside)")
}
exit(status)
