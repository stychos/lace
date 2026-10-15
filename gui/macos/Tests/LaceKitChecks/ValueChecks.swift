//
// Lace macOS frontend
// Checks: values round-trip through LaceValue, display text, the edit rules
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

func checkValues() {
  // Display text (liblace's rules; the C tests cover the corners)
  Checks.equal(Value.null.displayText(), "NULL")
  Checks.equal(Value.int(-42).displayText(), "-42")
  Checks.equal(Value.float(0.1).displayText(), "0.1")
  Checks.equal(Value.bool(true).displayText(), "true")
  Checks.equal(Value.text("a\nb\r\nc").displayText(), "a⏎b⏎c")
  Checks.equal(Value.text("héllo wörld").displayText(maxChars: 5), "héllo…")
  Checks.equal(Value.blob(Data([0xde, 0xad, 0x01])).displayText(), "0xdead01 (3 bytes)")
  Checks.equal(Value.blob(Data()).displayText(), "0x (0 bytes)")
  Checks.equal(Value.decimal("12345678901234567890.5").displayText(), "12345678901234567890.5")
  Checks.equal(Value.omitted(label: "[TEXT: 5 MB]", bytes: 5 << 20).displayText(), "[TEXT: 5 MB]")

  // Every kind of value survives a trip through a LaceValue
  let samples: [Value] = [
    .null, .int(Int64.max), .int(-1), .float(1.5), .text(""), .text("x'y"), .date("2026-10-15"),
    .timestamp("2026-10-15 12:00:00"), .decimal("1.000"), .blob(Data([0, 255])), .blob(Data()),
    .bool(false), .omitted(label: "[BLOB: 9 bytes]", bytes: 9),
  ]
  for v in samples {
    let back = v.withLaceValue { Value($0.pointee) }
    Checks.equal(back, v, "round trip")
  }
  Checks.equal(Value.blob(Data()).isNull, false, "an empty BLOB is a BLOB")
  Checks.equal(Value.date("2026-01-01").type, .date)
  Checks.expect(ValueType.decimal.isNumeric && ValueType.int.isNumeric, "numeric types")
  Checks.expect(!ValueType.text.isNumeric && !ValueType.blob.isNumeric, "non-numeric types")
  Checks.equal(ValueType.decimal.name, "DECIMAL")

  // Edit text
  Checks.equal(Value.blob(Data([0xde, 0xad])).editText, "dead")
  Checks.equal(Value.null.editText, "")
  Checks.equal(try? Value.fromEditText("ff00", like: .blob), .blob(Data([0xff, 0x00])))
  Checks.equal(try? Value.fromEditText("42", like: .int), .int(42))
  Checks.expect((try? Value.fromEditText("zz", like: .blob)) == nil, "bad hex is refused")

  // The one set of edit rules
  do {
    Checks.equal(try Value.edit(original: .text("abc"), columnType: .text, text: "abc"), .unchanged)
    Checks.equal(try Value.edit(original: .text("abc"), columnType: .text, text: ""), .write(.text("")))
    Checks.equal(try Value.edit(original: .text("abc"), columnType: .text, text: nil), .write(.null))
    Checks.equal(try Value.edit(original: .null, columnType: .int, text: ""), .unchanged)
    Checks.equal(try Value.edit(original: .null, columnType: .int, text: nil), .unchanged)
    Checks.equal(try Value.edit(original: .null, columnType: .int, text: "42"), .write(.int(42)))
    Checks.equal(
      try Value.edit(original: .decimal("1.50"), columnType: .decimal, text: "2.25"),
      .write(.decimal("2.25")))
  } catch {
    Checks.fail("edit rules threw: \(error)")
  }
  do {
    _ = try Value.edit(original: .blob(Data([1])), columnType: .blob, text: "zz")
    Checks.fail("bad hex edit did not throw")
  } catch let e as LaceError {
    Checks.expect(!e.message.isEmpty, "bad hex edit has a message")
  } catch {
    Checks.fail("bad hex edit: not a LaceError")
  }
}
