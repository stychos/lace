//
// Lace macOS frontend
// Recent connections: the TUI's recent.json (the last RECENT_MAX URLs and
// files connected to that are not saved connections, newest first, never
// with a password). Main thread only.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class RecentConnections {
  public struct Entry: Hashable, Sendable {
    /// A connection string without passwords
    public let url: String
    public let used: Date
  }

  public static let maxCount = Int(RECENT_MAX)
  public private(set) var entries: [Entry] = []
  /// Why the last write failed
  public private(set) var lastError: String?
  public var onChanged: (() -> Void)?
  /// Posted after every change (every connection form's list)
  public static let didChange = Notification.Name("LaceRecentConnectionsDidChange")

  public init() {
    reload()
  }

  /// Read recent.json again (a missing file is an empty list)
  public func reload() {
    var list = RecentList()
    recent_load(&list)
    defer { recent_free(&list) }
    entries = (0..<list.count).compactMap { i in
      guard let url = borrowCString(list.items[i].url) else { return nil }
      return Entry(url: url, used: Date(timeIntervalSince1970: TimeInterval(list.items[i].used)))
    }
  }

  /// The URL without its passwords; nil when it does not parse
  public static func stripPasswords(_ url: String) -> String? {
    takeCString(recent_strip_passwords(url))
  }

  /// Put a URL first (passwords removed, an older copy dropped, the list
  /// trimmed) and write the file. False when the URL does not parse.
  @discardableResult
  public func add(_ url: String) -> Bool {
    var list = RecentList()
    recent_load(&list)
    defer { recent_free(&list) }
    guard recent_add(&list, url) else { return false }
    save(&list)
    return true
  }

  public func remove(at index: Int) {
    var list = RecentList()
    recent_load(&list)
    defer { recent_free(&list) }
    guard index >= 0, index < list.count else { return }
    recent_remove(&list, index)
    save(&list)
  }

  public func remove(url: String) {
    if let i = entries.firstIndex(where: { $0.url == url }) {
      remove(at: i)
    }
  }

  private func save(_ list: inout RecentList) {
    var err: UnsafeMutablePointer<CChar>? = nil
    if recent_save(&list, &err) {
      lastError = nil
    } else {
      lastError = takeCString(err) ?? "recent.json could not be written"
    }
    reload()
    onChanged?()
    NotificationCenter.default.post(name: RecentConnections.didChange, object: self)
  }
}
