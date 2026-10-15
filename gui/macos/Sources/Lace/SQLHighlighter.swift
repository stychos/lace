//
// Lace macOS frontend
// A light SQL highlighter for the query editor: keywords, strings, quoted
// identifiers, comments and numbers, as colours on the text storage. It
// only colours; statement boundaries still come from liblace's lexer.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
enum SQLHighlighter {
  private static let keywords: Set<String> = [
    "select", "from", "where", "and", "or", "not", "in", "is", "null", "as", "on", "join", "inner", "left", "right",
    "full", "outer", "cross", "natural", "using", "group", "by", "order", "having", "limit", "offset", "fetch",
    "first", "next", "rows", "only", "distinct", "all", "union", "intersect", "except", "insert", "into", "values",
    "update", "set", "delete", "create", "alter", "drop", "table", "index", "view", "trigger", "if", "exists",
    "primary", "key", "foreign", "references", "unique", "check", "default", "constraint", "begin", "commit",
    "rollback", "transaction", "savepoint", "release", "with", "recursive", "case", "when", "then", "else", "end",
    "between", "like", "ilike", "escape", "asc", "desc", "nulls", "last", "returning", "explain", "analyze",
    "vacuum", "pragma", "show", "describe", "truncate", "cascade", "restrict", "add", "column", "rename", "to",
    "true", "false", "cast", "over", "partition", "window", "exists", "any", "some", "lateral", "for", "share",
    "nowait", "skip", "locked", "grant", "revoke", "temporary", "temp", "replace", "ignore", "conflict", "do",
    "nothing", "collate", "glob", "regexp", "match", "indexed", "reindex", "attach", "detach", "database",
    "schema", "sequence", "function", "procedure", "call", "declare", "return", "returns", "language", "deferrable",
    "initially", "deferred", "immediate", "exclusive", "abort", "fail", "generated", "always", "stored", "virtual",
    "int", "integer", "bigint", "smallint", "text", "varchar", "char", "boolean", "bool", "real", "double",
    "precision", "float", "numeric", "decimal", "date", "time", "timestamp", "timestamptz", "interval", "blob",
    "bytea", "json", "jsonb", "uuid", "serial", "bigserial",
  ]

  private static var keywordColor: NSColor { .systemPurple }
  private static var stringColor: NSColor { .systemRed }
  private static var identifierColor: NSColor { .systemTeal }
  private static var commentColor: NSColor { .systemGray }
  private static var numberColor: NSColor { .systemBlue }

  /// Recolour the whole text (editor-sized texts: a few hundred KB at most)
  static func highlight(_ textView: NSTextView, driver: Driver) {
    guard let storage = textView.textStorage else { return }
    let text = storage.string
    let utf16 = Array(text.utf16)
    let n = utf16.count
    storage.beginEditing()
    defer { storage.endEditing() }
    storage.removeAttribute(.foregroundColor, range: NSRange(location: 0, length: n))
    storage.addAttribute(.foregroundColor, value: NSColor.textColor, range: NSRange(location: 0, length: n))
    let mysql = driver == .mysql || driver == .mariadb
    var i = 0
    func color(_ from: Int, _ to: Int, _ c: NSColor) {
      if to > from {
        storage.addAttribute(.foregroundColor, value: c, range: NSRange(location: from, length: to - from))
      }
    }
    func at(_ k: Int) -> UInt16 { k < n ? utf16[k] : 0 }
    func isWord(_ u: UInt16) -> Bool {
      (u >= 0x30 && u <= 0x39) || (u >= 0x41 && u <= 0x5A) || (u >= 0x61 && u <= 0x7A) || u == 0x5F || u > 0x7F
    }
    while i < n {
      let c = at(i)
      // Comments: -- to the end of the line, /* */, # on MySQL
      if c == 0x2D && at(i + 1) == 0x2D || (mysql && c == 0x23) {
        let start = i
        while i < n && at(i) != 0x0A { i += 1 }
        color(start, i, commentColor)
        continue
      }
      if c == 0x2F && at(i + 1) == 0x2A {
        let start = i
        i += 2
        while i < n && !(at(i) == 0x2A && at(i + 1) == 0x2F) { i += 1 }
        i = min(n, i + 2)
        color(start, i, commentColor)
        continue
      }
      // Strings and quoted identifiers (a doubled quote stays inside;
      // backslashes escape on MySQL and in E'...')
      if c == 0x27 || c == 0x22 || c == 0x60 {
        let quote = c
        let start = i
        let escapes = quote == 0x27 && (mysql || (i > 0 && (at(i - 1) == 0x45 || at(i - 1) == 0x65)))
        i += 1
        while i < n {
          if escapes && at(i) == 0x5C {
            i += 2
            continue
          }
          if at(i) == quote {
            if at(i + 1) == quote {
              i += 2
              continue
            }
            i += 1
            break
          }
          i += 1
        }
        i = min(i, n)
        color(start, i, quote == 0x27 ? stringColor : identifierColor)
        continue
      }
      // Dollar-quoted bodies: $tag$ ... $tag$
      if c == 0x24, !(i > 0 && isWord(at(i - 1))) {
        var j = i + 1
        while j < n && isWord(at(j)) && !(at(j) >= 0x30 && at(j) <= 0x39 && j == i + 1) { j += 1 }
        if at(j) == 0x24 {
          let tag = Array(utf16[i...j])
          var k = j + 1
          var found = false
          while k + tag.count <= n {
            if Array(utf16[k..<(k + tag.count)]) == tag {
              found = true
              break
            }
            k += 1
          }
          let end = found ? k + tag.count : n
          color(i, end, stringColor)
          i = end
          continue
        }
      }
      // Numbers
      if c >= 0x30 && c <= 0x39, !(i > 0 && isWord(at(i - 1))) {
        let start = i
        while i < n && (isWord(at(i)) || at(i) == 0x2E) { i += 1 }
        color(start, i, numberColor)
        continue
      }
      // Words
      if isWord(c) {
        let start = i
        while i < n && isWord(at(i)) { i += 1 }
        let word = String(utf16CodeUnits: Array(utf16[start..<i]), count: i - start).lowercased()
        if keywords.contains(word) {
          color(start, i, keywordColor)
        }
        continue
      }
      i += 1
    }
  }
}
