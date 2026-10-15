//
// Lace macOS frontend
// Small helpers for the C boundary
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace
import Foundation

/// A C string liblace allocated for us: copied into a String, then freed.
func takeCString(_ p: UnsafeMutablePointer<CChar>?) -> String? {
  guard let p else { return nil }
  defer { free(p) }
  return String(cString: p)
}

/// A borrowed C string (not freed), nil when NULL or empty.
func borrowCString(_ p: UnsafePointer<CChar>?) -> String? {
  guard let p, p.pointee != 0 else { return nil }
  return String(cString: p)
}

func borrowCString(_ p: UnsafeMutablePointer<CChar>?) -> String? {
  borrowCString(UnsafePointer(p))
}

/// Text held as a pointer and a length (a LaceValue's text). liblace leaves
/// len 0 for some producers, so a zero length means "up to the NUL".
func textString(_ data: UnsafeMutablePointer<CChar>?, _ len: Int) -> String {
  guard let data else { return "" }
  let n = len > 0 ? len : Int(strlen(data))
  return String(decoding: UnsafeRawBufferPointer(start: data, count: n), as: UTF8.self)
}
