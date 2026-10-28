//
// Lace macOS frontend
// SessionFile: session.json's JSON layer, the shared C code every GUI
// frontend runs (config/session_json.c, through csrc/macos_shared.c): the
// workspaces out of a file of any shape (version 2; a version-1 ncurses
// file converted; the old macos-session.json with its window geometry
// moved under "macos"), and the text to write, the file's other top-level
// members kept. A file that does not parse throws and is never written.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public enum SessionFile {
  public struct ParseError: Error, LocalizedError {
    public let message: String
    public var errorDescription: String? { message }
  }

  /// The workspace objects; legacy: macos-session.json's shape ("windows")
  public static func workspaces(text: String, legacy: Bool) throws -> [[String: Any]] {
    var err: UnsafeMutablePointer<CChar>? = nil
    guard let json = takeCString(lace_macos_session_workspaces(text, legacy, &err)) else {
      throw ParseError(message: takeCString(err) ?? "session.json could not be read")
    }
    free(err)
    guard let array = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [Any] else {
      throw ParseError(message: "session.json: the workspaces are not a list")
    }
    return array.compactMap { $0 as? [String: Any] }
  }

  /// The text to write over existing (nil: no file yet): version 2, the
  /// workspaces and current replaced, every other member kept
  public static func merge(existing: String?, workspaces: [[String: Any]], current: Int) throws -> String {
    let data = try JSONSerialization.data(withJSONObject: workspaces, options: [.sortedKeys])
    let json = String(decoding: data, as: UTF8.self)
    var err: UnsafeMutablePointer<CChar>? = nil
    guard let text = takeCString(lace_macos_session_merge(existing, json, Int32(current), &err)) else {
      throw ParseError(message: takeCString(err) ?? "session.json could not be merged")
    }
    free(err)
    return text
  }
}
