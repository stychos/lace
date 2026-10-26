//
// Lace macOS frontend
// DdlRunner: the one run path of every structure change and table action.
// The statement liblace built is shown in a confirmation under a
// plain-words summary (and a note when a transaction is open), run on the
// table view's connection as a recorded user statement (the lock banner
// shows meanwhile), and on success the connection page reloads every tab
// of the object, renames or closes them, and reloads the sidebar
// (TableViewController.onDdlApplied). A refusal from liblace never reaches
// a confirmation; a refusal from the server is shown in full, in the
// editor's sheet when one is open.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

/// What follows a change that ran
enum DdlAfter {
  /// The object's tabs reload (schema, page 0, count, what the sub-views loaded)
  case schema
  /// The same, and the sidebar's lists
  case tables
  /// The object's tabs follow the new name; the sidebar reloads
  case rename(String)
  /// The object's tabs close; the sidebar reloads
  case drop
  /// The sidebar reloads and the table opens on Structure
  case createTable(String)
}

struct DdlAction {
  var title: String
  var summary: String
  var sql: String
  var destructive = false
  var after: DdlAfter = .schema
}

@MainActor
enum DdlRunner {
  /// Scripts answer every confirmation with yes (ScriptDriver sets it)
  static var autoConfirm = false
  /// Told the SQL of every change that was confirmed, and every refusal
  static var logger: ((String) -> Void)?

  /// Confirm, then run; true when the change ran. sheet: the editor to close
  /// on success, or to show the server's refusal in.
  @discardableResult
  static func perform(_ a: DdlAction, in vc: TableViewController, sheet: FormSheetController? = nil) async -> Bool {
    let connection = vc.connection
    var message = "\(a.summary)\n\n\(a.sql)"
    if connection.transactionState == .open {
      message +=
        connection.driver == .mysql || connection.driver == .mariadb
        ? "\n\nMySQL commits the open transaction first."
        : "\n\nRuns inside the open transaction: ROLLBACK undoes it."
    }
    logger?("ddl \(a.title): \(a.sql)")
    if !autoConfirm {
      let window = sheet?.view.window ?? vc.view.window
      guard await confirm(a.title, message, action: a.destructive ? a.title : "Run", destructive: a.destructive, window: window)
      else {
        vc.toast?("\(a.title): cancelled")
        return false
      }
    }
    sheet?.setBusy(true)
    do {
      _ = try await connection.exec(a.sql)
      sheet?.setBusy(false)
      sheet?.close()
      vc.record?(a.sql)
      vc.toast?("\(a.title): done")
      vc.onDdlApplied?(a.after)
      return true
    } catch {
      sheet?.setBusy(false)
      let why = "\(error)"
      logger?("ddl failed: \(why)")
      if let sheet {
        sheet.showError(why)
      } else {
        Alerts.error("\(a.title) failed", why, window: vc.view.window)
      }
      return false
    }
  }

  /// liblace cannot build the change for this driver: the reason, before any confirmation
  static func refused(_ error: Error, in vc: TableViewController, sheet: FormSheetController? = nil) {
    let why = "\(error)"
    logger?("ddl refused: \(why)")
    if let sheet {
      sheet.showError(why)
    } else {
      vc.toast?(why)
    }
  }

  /// A plain confirmation: Cancel is the default; a destructive action is drawn so
  static func confirm(_ title: String, _ message: String, action: String, destructive: Bool, window: NSWindow?) async
    -> Bool
  {
    if autoConfirm { return true }
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    alert.alertStyle = destructive ? .warning : .informational
    alert.addButton(withTitle: "Cancel")
    let go = alert.addButton(withTitle: action)
    go.hasDestructiveAction = destructive
    go.keyEquivalent = destructive ? "" : "\r"
    if destructive {
      alert.buttons.first?.keyEquivalent = "\r"
    }
    let response: NSApplication.ModalResponse
    if let window {
      response = await withCheckedContinuation { cont in
        alert.beginSheetModal(for: window) { cont.resume(returning: $0) }
      }
    } else {
      response = alert.runModal()
    }
    return response == .alertSecondButtonReturn
  }
}
