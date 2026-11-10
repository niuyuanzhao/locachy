// Thin companion logging shim for the tracked upstream virtual-display source.
// No file logging or credentials are written by the companion.
import Foundation
enum Log {
    static func info(_ message: String) { CompanionLog.info(message) }
}
