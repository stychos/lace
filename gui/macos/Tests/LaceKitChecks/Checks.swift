//
// Lace macOS frontend
// Minimal check harness (one program, like tests/test.h: the Command Line
// Tools ship neither XCTest nor swift-testing)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

enum Checks {
  // The checks run one after another on the main task
  nonisolated(unsafe) static var count = 0
  nonisolated(unsafe) static var failures = 0

  static func expect(
    _ condition: Bool, _ what: @autoclosure () -> String, file: StaticString = #filePath,
    line: UInt = #line
  ) {
    count += 1
    if !condition {
      failures += 1
      FileHandle.standardError.write(Data("\(file):\(line): \(what())\n".utf8))
    }
  }

  static func equal<T: Equatable>(
    _ got: T, _ want: T, _ what: String = "", file: StaticString = #filePath, line: UInt = #line
  ) {
    expect(
      got == want, "\(what.isEmpty ? "" : what + ": ")got \(got), want \(want)", file: file,
      line: line)
  }

  static func fail(_ what: String, file: StaticString = #filePath, line: UInt = #line) {
    expect(false, what, file: file, line: line)
  }

  /// body must throw a LaceError that matches
  static func throwsLaceError(
    _ what: String, file: StaticString = #filePath, line: UInt = #line,
    _ body: () async throws -> Void, matching matches: (LaceError) -> Bool
  ) async {
    do {
      try await body()
      fail("\(what): did not throw", file: file, line: line)
    } catch let error as LaceError {
      expect(matches(error), "\(what): wrong error \(error.code) \(error.message)", file: file, line: line)
    } catch {
      fail("\(what): not a LaceError: \(error)", file: file, line: line)
    }
  }

  static func report(_ suite: String) -> Int32 {
    print("\(suite): \(count) checks, \(failures) failed")
    return failures == 0 ? 0 : 1
  }
}
