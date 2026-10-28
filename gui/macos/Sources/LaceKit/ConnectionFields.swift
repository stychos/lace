//
// Lace macOS frontend
// ConnectionFields: the connection form's field model, the TUIs' shared
// core/conn_fields.c (compiled into the app): filled from a saved
// connection or a URI, turned back into a saved connection, a connection
// string or the URI line, whose passwords are always shown as •••••. Dirty
// is anything (a typed password included) that differs from the last fill,
// clear or markClean(). Main thread only; the typed text is wiped when it goes.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class ConnectionFields {
  /// The fields, in form order (the C CF_* values)
  public enum Field: Int, CaseIterable, Sendable {
    case name, driver, file, host, port, database, user, password, savePassword, ssh, sshHost
    case sshPort, sshUser, sshKey, sshPassword, saveSSHPassword

    /// What the script driver and the GTK frontend call it
    public var key: String {
      switch self {
      case .name: "name"
      case .driver: "driver"
      case .file: "file"
      case .host: "host"
      case .port: "port"
      case .database: "database"
      case .user: "user"
      case .password: "password"
      case .savePassword: "save-password"
      case .ssh: "ssh"
      case .sshHost: "ssh-host"
      case .sshPort: "ssh-port"
      case .sshUser: "ssh-user"
      case .sshKey: "ssh-key"
      case .sshPassword: "ssh-password"
      case .saveSSHPassword: "save-ssh-password"
      }
    }

    public init?(key: String) {
      guard let f = Field.allCases.first(where: { $0.key == key }) else { return nil }
      self = f
    }

    /// A checkbox (save password, SSH tunnel, save SSH password)
    public var isCheck: Bool { conn_fields_is_check(Int32(rawValue)) }
    /// A text field (not the driver pick, not a checkbox)
    public var isText: Bool { self != .driver && !isCheck }
  }

  /// What the URI line shows in place of a password
  public static let passwordMask = "•••••"

  private let f: UnsafeMutablePointer<ConnFields>

  /// Empty, driver postgres, clean
  public init() {
    f = .allocate(capacity: 1)
    conn_fields_init(f)
  }

  deinit {
    conn_fields_free(f)
    f.deallocate()
  }

  /// A text field's text ("" for the driver and the checkboxes)
  public subscript(field: Field) -> String {
    get { String(cString: conn_fields_text(f, Int32(field.rawValue))) }
    set { conn_fields_set_text(f, Int32(field.rawValue), newValue) }
  }

  /// A checkbox
  public subscript(check field: Field) -> Bool {
    get { lace_macos_fields_check(f, Int32(field.rawValue)) }
    set { lace_macos_fields_set_check(f, Int32(field.rawValue), newValue) }
  }

  /// Index into drivers; 0 is SQLite
  public var driver: Int {
    get { Int(f.pointee.driver) }
    set { f.pointee.driver = Int32(max(0, min(newValue, drivers.count - 1))) }
  }

  /// "sqlite", "postgres", "mysql", "mariadb", and a saved driver this build
  /// does not know (shown rather than turned into another)
  public var drivers: [String] {
    (0..<Int(conn_fields_num_drivers(f))).map { String(cString: conn_fields_driver_name(f, Int32($0))) }
  }

  public var driverName: String { drivers[driver] }

  /// Whether the form shows the field (SQLite: a file; a server: host...; the
  /// tunnel's fields while SSH is on)
  public func isVisible(_ field: Field) -> Bool {
    conn_fields_visible(f, Int32(field.rawValue))
  }

  /// Every field from c: a password only when it is saved, never one left
  /// from before; the form is clean afterwards
  public func fill(_ c: SavedConnection) {
    c.withCStruct { conn_fields_fill(f, $0) }
  }

  /// Empty again, clean
  public func clear() {
    conn_fields_clear(f)
  }

  public var isDirty: Bool { conn_fields_dirty(f) }

  public func markClean() {
    conn_fields_mark_clean(f)
  }

  /// The URI line: the connection string with every password as •••••; ""
  /// when the fields do not make one
  public var uri: String {
    guard let p = conn_fields_uri(f) else { return "" }
    defer { str_secure_free(p) }
    return String(cString: p)
  }

  /// Typed into the URI line: a URL or a SQLite file. On success every
  /// field but the name follows it (a password still ••••• keeps the
  /// field's); otherwise the fields stay and the message says why.
  @discardableResult
  public func setURI(_ text: String) -> Bool {
    conn_fields_set_uri(f, text)
  }

  /// The saved connection the fields describe (a new id); nil (the message
  /// says why) when they do not validate. withSecrets keeps typed passwords
  /// whatever the save boxes say.
  public func collect(withSecrets: Bool) -> SavedConnection? {
    guard let c = conn_fields_collect(f, withSecrets) else { return nil }
    defer { lace_saved_conn_free(c) }
    return SavedConnection(c.pointee)  // no id in the fields: a new one
  }

  /// The string to connect with (typed passwords in); nil (the message says
  /// why) when the fields do not validate. May hold passwords.
  public func connectionString() -> String? {
    guard let p = conn_fields_connstr(f) else { return nil }
    defer { str_secure_free(p) }
    return String(cString: p)
  }

  /// The message line: a validation or parse error, a test result
  public var message: (text: String, isError: Bool)? {
    guard let m = f.pointee.message, m.pointee != 0 else { return nil }
    return (String(cString: m), f.pointee.message_error)
  }

  public func setMessage(_ text: String?, isError: Bool = false) {
    lace_macos_fields_set_message(f, isError, text)
  }
}
