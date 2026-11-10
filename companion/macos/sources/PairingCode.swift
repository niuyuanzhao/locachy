// SPDX-License-Identifier: GPL-3.0-or-later
import Darwin
import Foundation

// The pairing secret is 128 random bits, shown on the LOCachy screen as RFC 4648
// base32 (26 characters, no padding, groups separated by '-'), and typed here.
// It is read only from a terminal prompt (echo off) or, when stdin is not a
// terminal, from a single line on stdin (tests and scripts). It is never taken
// from argv, the environment, a file, the Keychain, or logs.
enum PairingCode {
    static let characters = 26
    static let bytes = 16
    private static let maxLine = 256

    // Strict: case-insensitive, '-' and whitespace ignored, exactly 26 symbols,
    // and the two trailing padding bits must be zero (canonical encoding).
    static func decode(_ text: UnsafeBufferPointer<UInt8>) throws -> Data {
        var output = Data(capacity: bytes)
        var accumulator: UInt32 = 0
        var pending = 0
        var symbols = 0
        for byte in text {
            switch byte {
            case 0x2d, 0x20, 0x09, 0x0d, 0x0a:  // '-', space, tab, CR, LF
                continue
            default:
                break
            }
            var value: UInt32
            switch byte {
            case 0x41...0x5a: value = UInt32(byte - 0x41)          // A-Z
            case 0x61...0x7a: value = UInt32(byte - 0x61)          // a-z
            case 0x32...0x37: value = UInt32(byte - 0x32) + 26     // 2-7
            default: throw CompanionError("The code contains a character outside base32")
            }
            symbols += 1
            guard symbols <= characters else { throw CompanionError("The code is longer than 26 characters") }
            accumulator = (accumulator << 5) | value
            pending += 5
            if pending >= 8 {
                pending -= 8
                output.append(UInt8((accumulator >> UInt32(pending)) & 0xff))
                accumulator &= (1 << UInt32(pending)) - 1
            }
        }
        guard symbols == characters, output.count == bytes, pending == 2, accumulator == 0 else {
            throw CompanionError("The code must decode to exactly 16 bytes")
        }
        return output
    }

    // Reads one code. The buffer that held the text is zeroed before returning.
    static func read(fd: Int32 = 0) throws -> Data {
        var line = [UInt8](repeating: 0, count: maxLine)
        defer { zero(&line) }
        if isatty(fd) == 1 {
            let tty = open("/dev/tty", O_RDWR | O_NOCTTY)
            guard tty >= 0 else { throw CompanionError("No terminal is available for the code prompt") }
            defer { close(tty) }
            var original = termios()
            guard tcgetattr(tty, &original) == 0 else { throw CompanionError("Cannot read terminal settings") }
            var quiet = original
            quiet.c_lflag &= ~tcflag_t(ECHO)
            guard tcsetattr(tty, TCSAFLUSH, &quiet) == 0 else { throw CompanionError("Cannot disable terminal echo") }
            defer {
                tcsetattr(tty, TCSAFLUSH, &original)
                _ = write(tty, "\n", 1)
            }
            let prompt = Array("Enter the 26-character code shown on the LOCachy screen: ".utf8)
            _ = prompt.withUnsafeBufferPointer { write(tty, $0.baseAddress, $0.count) }
            return try readLine(from: tty, into: &line)
        }
        return try readLine(from: fd, into: &line)
    }

    private static func readLine(from fd: Int32, into line: inout [UInt8]) throws -> Data {
        var count = 0
        var byte: UInt8 = 0
        while true {
            let got = Darwin.read(fd, &byte, 1)
            if got < 0 && errno == EINTR { continue }
            guard got == 1 else { break }
            if byte == 0x0a { break }
            guard count < line.count else { throw CompanionError("The code line is too long") }
            line[count] = byte
            count += 1
        }
        return try line.withUnsafeBufferPointer { buffer in
            try decode(UnsafeBufferPointer(rebasing: buffer[0..<count]))
        }
    }

    static func zero(_ bytes: inout [UInt8]) {
        bytes.withUnsafeMutableBytes { _ = memset_s($0.baseAddress, $0.count, 0, $0.count) }
    }
}
