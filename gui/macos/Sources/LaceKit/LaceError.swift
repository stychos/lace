//
// Lace macOS frontend
// Why a liblace call failed: the code from error.h and the message
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import CLace

public struct LaceError: Error, Sendable, Equatable, CustomStringConvertible {
  /// LACE_ERR_* from error.h (the daemon's code, or liblace's own)
  public let code: Int32
  /// The daemon's or liblace's message, as lace_client_error() gave it
  public let message: String

  public init(code: Int32, message: String) {
    self.code = code
    self.message = message
  }

  public var description: String { message }

  /// The statement was cancelled (Esc, Stop)
  public var isCancelled: Bool { code == LACE_ERR_QUERY_CANCELLED }
  /// The database connection (or the daemon) was lost
  public var isConnectionLost: Bool { code == LACE_ERR_CONNECTION_LOST }
  /// Our Connection was closed before the call
  public var isConnectionClosed: Bool { code == LACE_ERR_CONNECTION_CLOSED }
  /// The database or the SSH server refused the password
  public var isAuthError: Bool {
    code == LACE_ERR_AUTH_REQUIRED || code == LACE_ERR_AUTH_FAILED
  }
  /// A write was refused because the row changed or vanished
  public var isRowNotFound: Bool { code == LACE_ERR_ROW_NOT_FOUND }
}
