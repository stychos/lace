//
// Lace macOS frontend
// Query history: liblace's LaceHistory with the TUI's persistence
// (history/<connection id>.json in the data directory, compiled into the
// app), so the TUI, the GTK frontend and this app share one file per saved
// connection. Main thread only.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public final class History {
  public enum Kind: Sendable, Hashable, CaseIterable {
    case query, select, update, delete, insert, ddl

    init(_ c: LaceHistoryType) {
      switch c {
      case LACE_HISTORY_SELECT: self = .select
      case LACE_HISTORY_UPDATE: self = .update
      case LACE_HISTORY_DELETE: self = .delete
      case LACE_HISTORY_INSERT: self = .insert
      case LACE_HISTORY_DDL: self = .ddl
      default: self = .query
      }
    }

    var c: LaceHistoryType {
      switch self {
      case .query: LACE_HISTORY_QUERY
      case .select: LACE_HISTORY_SELECT
      case .update: LACE_HISTORY_UPDATE
      case .delete: LACE_HISTORY_DELETE
      case .insert: LACE_HISTORY_INSERT
      case .ddl: LACE_HISTORY_DDL
      }
    }

    /// "SEL", "UPD", ... for lists
    public var tag: String { borrowCString(lace_history_type_tag(c)) ?? "?" }
    public var name: String { borrowCString(lace_history_type_name(c)) ?? "Query" }
  }

  /// The preference values the TUI and the GTK frontend use
  public enum Mode: Int, Sendable, CaseIterable {
    case off = 0
    /// Kept while the connection is open
    case session = 1
    /// history/<id>.json, for connections with a saved id
    case persistent = 2
  }

  public struct Entry: Sendable, Hashable {
    public let sql: String
    public let date: Date
    public let kind: Kind
  }

  public static let sizeRange = Int(LACE_HISTORY_SIZE_MIN)...Int(LACE_HISTORY_SIZE_MAX)
  public static let defaultSize = Int(LACE_HISTORY_SIZE_DEFAULT)

  /// The saved connection's id; nil for a connection that is not saved
  public let connectionID: String?
  public let mode: Mode
  public let maxSize: Int
  /// Why the file could not be read or written (the last problem)
  public private(set) var lastError: String?
  public var onChanged: (() -> Void)?

  private let raw: UnsafeMutablePointer<LaceHistory>

  /// Loads the file when persistent and the id is a valid file name
  public init(connectionID: String?, mode: Mode, maxSize: Int = History.defaultSize) {
    self.connectionID = connectionID.flatMap { connmgr_id_valid($0) ? $0 : nil }
    self.mode = mode
    self.maxSize = max(1, maxSize)
    guard let h = lace_history_create(self.connectionID) else { fatalError("out of memory") }
    raw = h
    if isPersistent {
      var err: UnsafeMutablePointer<CChar>? = nil
      if !history_load(raw, &err) {
        lastError = takeCString(err) ?? "the history file could not be read"
      }
    }
  }

  deinit {
    lace_history_free(raw)
  }

  public var isPersistent: Bool { mode == .persistent && connectionID != nil }
  public var count: Int { Int(lace_history_count(raw)) }

  /// Oldest first
  public var entries: [Entry] {
    (0..<count).compactMap { i in
      guard let e = lace_history_get(raw, i) else { return nil }
      return Entry(
        sql: borrowCString(e.pointee.sql) ?? "", date: Date(timeIntervalSince1970: TimeInterval(e.pointee.timestamp)),
        kind: Kind(e.pointee.type))
    }
  }

  public static func kind(of sql: String) -> Kind {
    Kind(lace_history_detect_type(sql))
  }

  /// Record a statement that ran (nothing when the mode is off or the text empty)
  public func record(_ sql: String, kind: Kind? = nil) {
    let trimmed = sql.trimmingCharacters(in: .whitespacesAndNewlines)
    guard mode != .off, !trimmed.isEmpty else { return }
    lace_history_add(raw, trimmed, (kind ?? History.kind(of: trimmed)).c, Int32(maxSize))
    persist()
  }

  public func remove(at index: Int) {
    guard index >= 0, index < count else { return }
    lace_history_remove(raw, index)
    persist()
  }

  public func clear() {
    lace_history_clear(raw)
    persist()
  }

  /// The file this history is kept in, when persistent
  public var filePath: String? {
    guard let connectionID else { return nil }
    return takeCString(history_get_file_path(connectionID))
  }

  private func persist() {
    defer { onChanged?() }
    guard isPersistent else { return }
    var err: UnsafeMutablePointer<CChar>? = nil
    if history_save(raw, &err) {
      lastError = nil
    } else {
      lastError = takeCString(err) ?? "the history file could not be written"
    }
  }
}
