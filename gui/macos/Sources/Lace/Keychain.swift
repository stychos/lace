//
// Lace macOS frontend
// Passwords of saved connections in the login Keychain (service
// dev.hexed.lace, account = the saved connection's id, "/ssh" appended for
// the tunnel's), instead of connections.json. The file keeps save_password
// set with no password, so the TUI and the GTK frontend ask for it; this app
// reads it back from the Keychain. A dev build signed ad hoc gets a new
// identity every build, so macOS may ask before handing an item back.
//
// A scripted run (LACE_MACOS_SCRIPT) with $LACE_MACOS_KEYCHAIN_FILE keeps
// the items in that file instead (owner-only JSON): the login Keychain asks
// the person at the screen before an unsigned build may write to it, which
// a smoke test must never make it do.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import Security

enum Keychain {
  static let service = "dev.hexed.lace"

  /// A scripted run's stand-in for the Keychain, if it named one
  private static let testFile: URL? = {
    let env = ProcessInfo.processInfo.environment
    guard !(env["LACE_MACOS_SCRIPT"] ?? "").isEmpty, let path = env["LACE_MACOS_KEYCHAIN_FILE"], !path.isEmpty
    else { return nil }
    return URL(fileURLWithPath: path)
  }()

  /// Whether the items are the stand-in file's (the script driver keeps the
  /// Keychain preference on then)
  static var isStandIn: Bool { testFile != nil }
  /// The stand-in hands nothing back, as the Keychain does when the person
  /// denies its prompt (the script's keychain-refuse)
  nonisolated(unsafe) static var standInRefuses = false

  /// The saved password, or nil (none, or the Keychain refused)
  static func password(account: String) -> String? {
    if let testFile {
      return standInRefuses ? nil : readTestFile(testFile)[account]
    }
    let query: [CFString: Any] = [
      kSecClass: kSecClassGenericPassword,
      kSecAttrService: service,
      kSecAttrAccount: account,
      kSecReturnData: true,
      kSecMatchLimit: kSecMatchLimitOne,
    ]
    var item: CFTypeRef?
    guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess, let data = item as? Data else {
      return nil
    }
    return String(data: data, encoding: .utf8)
  }

  /// Store a password (nil or empty removes the item). Returns a message
  /// when the Keychain refused.
  @discardableResult
  static func set(_ password: String?, account: String) -> String? {
    if let testFile {
      var items = readTestFile(testFile)
      items[account] = (password ?? "").isEmpty ? nil : password
      do {
        let data = try JSONSerialization.data(withJSONObject: items, options: [.sortedKeys])
        try Prefs.writePrivate(data, to: testFile)
        return nil
      } catch {
        return "\(error)"
      }
    }
    let base: [CFString: Any] = [
      kSecClass: kSecClassGenericPassword,
      kSecAttrService: service,
      kSecAttrAccount: account,
    ]
    guard let password, !password.isEmpty else {
      let status = SecItemDelete(base as CFDictionary)
      return status == errSecSuccess || status == errSecItemNotFound ? nil : message(status)
    }
    let data = Data(password.utf8)
    let update: [CFString: Any] = [kSecValueData: data]
    var status = SecItemUpdate(base as CFDictionary, update as CFDictionary)
    if status == errSecItemNotFound {
      var add = base
      add[kSecValueData] = data
      add[kSecAttrLabel] = "Lace database connection"
      status = SecItemAdd(add as CFDictionary, nil)
    }
    return status == errSecSuccess ? nil : message(status)
  }

  static func remove(account: String) {
    set(nil, account: account)
  }

  private static func readTestFile(_ url: URL) -> [String: String] {
    guard let data = try? Data(contentsOf: url) else { return [:] }
    return (try? JSONSerialization.jsonObject(with: data)) as? [String: String] ?? [:]
  }

  private static func message(_ status: OSStatus) -> String {
    (SecCopyErrorMessageString(status, nil) as String?) ?? "Keychain error \(status)"
  }
}
