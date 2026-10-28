//
// Lace macOS frontend
// Checks: the connection form's field model (the TUIs' conn_fields.c through
// LaceKit's ConnectionFields): a URI into the fields and back masked, a URI
// that does not parse leaving them, dirty tracking with a typed password,
// filling from a saved connection, collecting one
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkConnectionFields() {
  let f = ConnectionFields()
  Checks.equal(f.driverName, "postgres", "a new form is for a server")
  Checks.expect(!f.isDirty, "a new form is clean")

  // A URI fills the fields; the URI line masks the password
  Checks.expect(f.setURI("postgres://app:hunter2@db1:5432/shop"), "a postgres URI parses")
  Checks.equal(f[.host], "db1")
  Checks.equal(f[.port], "5432")
  Checks.equal(f[.database], "shop")
  Checks.equal(f[.user], "app")
  Checks.equal(f[.password], "hunter2")
  Checks.expect(f.uri.contains(ConnectionFields.passwordMask), "URI masked: \(f.uri)")
  Checks.expect(!f.uri.contains("hunter2"), "no password in the URI line: \(f.uri)")
  Checks.expect(f.isDirty, "a parsed URI is an edit")

  // One that does not parse leaves the fields and says why
  Checks.expect(!f.setURI("::nonsense"), "::nonsense does not parse")
  Checks.equal(f[.host], "db1", "host unchanged")
  Checks.expect(f.message?.isError == true, "an error message")
  Checks.expect(f.message?.text.contains("Not a connection URL") == true, "message: \(f.message?.text ?? "nil")")

  // The URI line's mask keeps the field's password
  Checks.expect(f.setURI(f.uri), "the masked URI parses back")
  Checks.equal(f[.password], "hunter2", "the mask keeps the password")

  // Fill from a saved connection: clean, its saved password only
  let saved = SavedConnection(
    name: "Prod", driver: "postgres", host: "h", port: 5433, database: "d", user: "u", password: "pw",
    savePassword: false)
  f.fill(saved)
  Checks.expect(!f.isDirty, "clean after a fill")
  Checks.equal(f[.name], "Prod")
  Checks.equal(f[.password], "", "a password not saved is not filled")
  Checks.equal(f[check: .savePassword], false)
  f[.password] = "x"
  Checks.expect(f.isDirty, "a typed password alone is an edit")
  f.markClean()
  Checks.expect(!f.isDirty, "clean after markClean")

  // Visibility follows the driver and the tunnel
  Checks.expect(f.isVisible(.host) && !f.isVisible(.file), "a server shows the host, not a file")
  Checks.expect(!f.isVisible(.sshHost), "no tunnel fields while SSH is off")
  f[check: .ssh] = true
  Checks.expect(f.isVisible(.sshHost), "the tunnel's fields with SSH on")
  f[check: .ssh] = false
  f.driver = 0
  Checks.expect(f.isVisible(.file) && !f.isVisible(.host), "SQLite shows a file")

  // Collect: validation, a new id, the typed name
  f.driver = 1
  f[.port] = "99999"
  Checks.expect(f.collect(withSecrets: false) == nil, "port 99999 refused")
  Checks.expect(f.message?.text.contains("1 to 65535") == true, "port message: \(f.message?.text ?? "nil")")
  f[.port] = ""
  let c = f.collect(withSecrets: true)
  Checks.equal(c?.name, "Prod")
  Checks.equal(c?.host, "h")
  Checks.equal(c?.password, "x", "withSecrets keeps a typed password")
  Checks.expect(c?.id != nil && c?.id != saved.id, "collect makes a new id")
  Checks.expect(f.connectionString()?.hasPrefix("postgres://") == true, "a connection string")

  // Clear: empty, clean, a server again
  f.clear()
  Checks.expect(!f.isDirty && f[.host].isEmpty && f.driverName == "postgres", "cleared")
  Checks.equal(ConnectionFields.Field(key: "ssh-password"), .sshPassword)
  Checks.expect(ConnectionFields.Field.savePassword.isCheck && !ConnectionFields.Field.host.isCheck, "checkboxes")
}
