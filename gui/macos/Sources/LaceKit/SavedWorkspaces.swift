//
// Lace macOS frontend
// SavedWorkspaces: the named workspace layouts in workspaces.json, shared
// with the TUIs (their Workspaces window), through the TUI's own
// config/saved_workspaces.c and config/workspace_json.c behind the C bridge
// (csrc/macos_shared.c): a layout is a workspace as session.json holds one
// (a flat ncurses layout comes back nested); every call reads the file
// again, so another frontend's changes are kept; a file that does not parse
// is never written. Also the palette's fuzzy score (core/fuzzy.c), shared
// with both TUIs.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public enum SavedWorkspaces {
  public struct Layout: @unchecked Sendable {
    public let name: String
    public let saved: Date
    /// The workspace object: "name", "connections" (each with "saved_id"
    /// and "tabs"), "current"
    public let workspace: [String: Any]

    /// Its connections (session.json's entries)
    public var connections: [[String: Any]] { workspace["connections"] as? [[String: Any]] ?? [] }
  }

  public struct Failure: Error, LocalizedError {
    public let message: String
    public var errorDescription: String? { message }
  }

  /// Every saved layout, in the file's order
  public static func list() throws -> [Layout] {
    var err: UnsafeMutablePointer<CChar>? = nil
    guard let json = takeCString(lace_macos_saved_workspaces_list(&err)) else {
      throw Failure(message: takeCString(err) ?? "workspaces.json could not be read")
    }
    free(err)
    guard let array = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [[String: Any]] else {
      throw Failure(message: "workspaces.json: the layouts are not a list")
    }
    return array.compactMap { e in
      guard let name = e["name"] as? String, let ws = e["workspace"] as? [String: Any] else { return nil }
      let saved = (e["saved"] as? NSNumber)?.doubleValue ?? 0
      return Layout(name: name, saved: Date(timeIntervalSince1970: saved), workspace: ws)
    }
  }

  /// Store workspace under name, replacing a layout of that name
  public static func put(name: String, workspace: [String: Any]) throws {
    let data = try JSONSerialization.data(withJSONObject: workspace, options: [.sortedKeys])
    try call { lace_macos_saved_workspaces_put(name, String(decoding: data, as: UTF8.self), &$0) }
  }

  public static func remove(name: String) throws {
    try call { lace_macos_saved_workspaces_remove(name, &$0) }
  }

  /// Refused when a layout named newName exists already
  public static func rename(_ name: String, to newName: String) throws {
    try call { lace_macos_saved_workspaces_rename(name, newName, &$0) }
  }

  private static func call(_ body: (inout UnsafeMutablePointer<CChar>?) -> Bool) throws {
    var err: UnsafeMutablePointer<CChar>? = nil
    let ok = body(&err)
    let message = takeCString(err)
    if !ok {
      throw Failure(message: message ?? "workspaces.json could not be written")
    }
  }
}

/// The command palettes' fuzzy match (core/fuzzy.c, both TUIs')
public enum Fuzzy {
  /// -1 no match; higher is better: a prefix, a word start, a substring, a
  /// subsequence per space-separated word. Case-insensitive.
  public static func score(_ needle: String, _ hay: String) -> Int {
    Int(fuzzy_score(needle, hay))
  }
}
