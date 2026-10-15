//
// Lace macOS frontend
// Passwords of saved connections in the login Keychain (service
// dev.hexed.lace, account = the saved connection's id, "/ssh" appended for
// the tunnel's), instead of connections.json. The file keeps save_password
// set with no password, so the TUI and the GTK frontend ask for it; this app
// reads it back from the Keychain. A dev build signed ad hoc gets a new
// identity every build, so macOS may ask before handing an item back.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import Security

enum Keychain {
  static let service = "dev.hexed.lace"

  /// The saved password, or nil (none, or the Keychain refused)
  static func password(account: String) -> String? {
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

  private static func message(_ status: OSStatus) -> String {
    (SecCopyErrorMessageString(status, nil) as String?) ?? "Keychain error \(status)"
  }
}
