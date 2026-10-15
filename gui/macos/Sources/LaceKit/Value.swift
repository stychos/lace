//
// Lace macOS frontend
// Values: liblace's exact cell values as a Swift enum, and the text rules
// (display text, edit text, the one set of cell-edit rules) from liblace
//
// Values keep their exact form end to end: integers beyond 2^53 stay
// integers, NUMERIC/DECIMAL keeps its digits, BLOBs their bytes, a value too
// large to load stays a label that is never written back.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

public enum ValueType: Sendable, Hashable, CaseIterable {
  case null, int, float, text, blob, bool, date, timestamp, decimal

  init(_ c: LaceValueType) {
    switch c {
    case LACE_TYPE_NULL: self = .null
    case LACE_TYPE_INT: self = .int
    case LACE_TYPE_FLOAT: self = .float
    case LACE_TYPE_BLOB: self = .blob
    case LACE_TYPE_BOOL: self = .bool
    case LACE_TYPE_DATE: self = .date
    case LACE_TYPE_TIMESTAMP: self = .timestamp
    case LACE_TYPE_DECIMAL: self = .decimal
    default: self = .text
    }
  }

  var c: LaceValueType {
    switch self {
    case .null: LACE_TYPE_NULL
    case .int: LACE_TYPE_INT
    case .float: LACE_TYPE_FLOAT
    case .text: LACE_TYPE_TEXT
    case .blob: LACE_TYPE_BLOB
    case .bool: LACE_TYPE_BOOL
    case .date: LACE_TYPE_DATE
    case .timestamp: LACE_TYPE_TIMESTAMP
    case .decimal: LACE_TYPE_DECIMAL
    }
  }

  /// Values of this type are drawn right-aligned
  public var isNumeric: Bool { lace_type_is_numeric(c) }

  /// liblace's name: "INT", "TEXT", "DECIMAL"
  public var name: String { borrowCString(lace_type_name(c)) ?? "TEXT" }
}

public enum Value: Sendable, Hashable {
  case null
  case int(Int64)
  case float(Double)
  case text(String)
  case date(String)
  case timestamp(String)
  /// Exact NUMERIC/DECIMAL, kept as its digits
  case decimal(String)
  case blob(Data)
  case bool(Bool)
  /// Too large to load: the daemon's label and the size. Never written back,
  /// never usable as a key.
  case omitted(label: String, bytes: Int)

  /// From a LaceValue liblace handed us (copied; the LaceValue stays the owner's)
  public init(_ v: LaceValue) {
    if v.is_null || v.type == LACE_TYPE_NULL {
      self = .null
      return
    }
    if v.omitted > 0 {
      self = .omitted(label: textString(v.text.data, v.text.len), bytes: Int(v.omitted))
      return
    }
    switch v.type {
    case LACE_TYPE_INT: self = .int(v.int_val)
    case LACE_TYPE_FLOAT: self = .float(v.float_val)
    case LACE_TYPE_BOOL: self = .bool(v.bool_val)
    case LACE_TYPE_BLOB:
      self = .blob(v.blob.data.map { Data(bytes: $0, count: Int(v.blob.len)) } ?? Data())
    case LACE_TYPE_DATE: self = .date(textString(v.text.data, v.text.len))
    case LACE_TYPE_TIMESTAMP: self = .timestamp(textString(v.text.data, v.text.len))
    case LACE_TYPE_DECIMAL: self = .decimal(textString(v.text.data, v.text.len))
    default: self = .text(textString(v.text.data, v.text.len))
    }
  }

  public var type: ValueType {
    switch self {
    case .null: .null
    case .int: .int
    case .float: .float
    case .text, .omitted: .text
    case .date: .date
    case .timestamp: .timestamp
    case .decimal: .decimal
    case .blob: .blob
    case .bool: .bool
    }
  }

  public var isNull: Bool {
    if case .null = self { return true }
    return false
  }

  public var isOmitted: Bool {
    if case .omitted = self { return true }
    return false
  }

  /// This value as a LaceValue (owned by the caller: lace_value_free it)
  func makeLaceValue() -> LaceValue {
    switch self {
    case .null: return lace_value_null()
    case .int(let i): return lace_value_int(i)
    case .float(let d): return lace_value_float(d)
    case .bool(let b): return lace_value_bool(b)
    case .text(let s): return lace_value_text(s)
    case .date(let s):
      var v = lace_value_text(s)
      v.type = LACE_TYPE_DATE
      return v
    case .timestamp(let s):
      var v = lace_value_text(s)
      v.type = LACE_TYPE_TIMESTAMP
      return v
    case .decimal(let s):
      var v = lace_value_text(s)
      v.type = LACE_TYPE_DECIMAL
      return v
    case .blob(let data):
      return data.withUnsafeBytes { buf -> LaceValue in
        if let base = buf.baseAddress {
          return lace_value_blob(base.assumingMemoryBound(to: UInt8.self), buf.count)
        }
        // An empty BLOB is a BLOB, not NULL: lace_value_blob needs a non-NULL pointer
        var zero: UInt8 = 0
        return lace_value_blob(&zero, 0)
      }
    case .omitted(let label, let bytes):
      var v = lace_value_text(label)
      v.omitted = bytes
      return v
    }
  }

  /// Run body with a LaceValue holding this value; freed afterwards
  public func withLaceValue<R>(_ body: (UnsafePointer<LaceValue>) throws -> R) rethrows -> R {
    var v = makeLaceValue()
    defer { lace_value_free(&v) }
    return try withUnsafePointer(to: &v) { try body($0) }
  }

  /// One line for a cell: "NULL", exact numbers, true/false, line breaks as
  /// "⏎", a BLOB as "0x<hex>… (N bytes)", a value too large to load as its
  /// label; at most maxChars characters (0: no limit), "…" when cut
  public func displayText(maxChars: Int = 0) -> String {
    withLaceValue { takeCString(lace_value_display_text($0, maxChars)) ?? "" }
  }

  /// The full, exact text for an edit box (NULL is ""; a BLOB all its hex)
  public var editText: String {
    withLaceValue { takeCString(lace_value_to_edit_text($0)) ?? "" }
  }

  /// Edited text back into a value for a column of type `like`: hex for a
  /// BLOB, exact digits for a DECIMAL, an integer for INT when it is one,
  /// else text the database converts. Throws for text that cannot be such a
  /// value (bad hex).
  public static func fromEditText(_ text: String, like type: ValueType) throws -> Value {
    var out = LaceValue()
    var err: UnsafeMutablePointer<CChar>? = nil
    guard lace_value_from_edit_text(text, type.c, &out, &err) else {
      throw LaceError(
        code: LACE_ERR_INVALID_PARAMS, message: takeCString(err) ?? "Not a valid value")
    }
    defer { lace_value_free(&out) }
    return Value(out)
  }

  public enum EditOutcome: Sendable, Equatable {
    /// Nothing to write: the text is what the cell already shows
    case unchanged
    case write(Value)
  }

  /// The rules every edit path follows (lace_edit_value): unchanged text, or
  /// a NULL cell left empty, writes nothing; empty text is '' (never NULL);
  /// nil text is the explicit NULL action; the text converts like the value
  /// the cell held, else like the column's type.
  public static func edit(original: Value, columnType: ValueType, text: String?) throws
    -> EditOutcome
  {
    try original.withLaceValue { orig in
      var out = LaceValue()
      var unchanged = false
      var err: UnsafeMutablePointer<CChar>? = nil
      guard lace_edit_value(orig, columnType.c, text, &out, &unchanged, &err) else {
        throw LaceError(
          code: LACE_ERR_INVALID_PARAMS, message: takeCString(err) ?? "Not a valid value")
      }
      defer { lace_value_free(&out) }
      return unchanged ? .unchanged : .write(Value(out))
    }
  }
}
