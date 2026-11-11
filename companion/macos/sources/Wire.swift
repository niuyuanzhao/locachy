// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import CoreFoundation

struct CompanionError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

// OpenDisplay pv3 framing with a mandatory authenticated LOCachy v1 profile.
// No discovery, mirror fallback, clipboard, file transfer, or power commands.
enum Wire {
    static let maxFrame = 4 * 1024 * 1024
    static let maxControl = 32767
    static let identity = "locachy-extdisplay-v1"
    // TLS_PSK_WITH_AES_128_GCM_SHA256: the only PSK suite Network.framework negotiates.
    static let pskSuite: UInt16 = 0x00A8
    static let alpn = "locachy-extdisplay-v1"
    static let capabilities = ["key", "button", "cancel", "pointer", "scroll"]

    static func frame(_ payload: Data) throws -> Data {
        guard !payload.isEmpty, payload.count <= maxFrame else { throw CompanionError("Invalid frame size") }
        var size = UInt32(payload.count).bigEndian
        var result = Data(bytes: &size, count: 4)
        result.append(payload)
        return result
    }
    static func json(_ object: [String: Any]) throws -> Data {
        let bytes = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        guard bytes.count <= maxControl else { throw CompanionError("Control message too large") }
        return bytes
    }
    static func object(_ data: Data) throws -> [String: Any] {
        guard !data.isEmpty, data.count <= maxControl, data.first == 123,
              !data.contains(0), let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              object["type"] is String else { throw CompanionError("Invalid control message") }
        return object
    }
    static func integer(_ object: [String: Any], _ name: String, _ range: ClosedRange<Int>) throws -> Int {
        guard let value = object[name] as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(),
              value.doubleValue.isFinite, value.doubleValue.rounded() == value.doubleValue,
              value.doubleValue >= Double(range.lowerBound), value.doubleValue <= Double(range.upperBound)
        else { throw CompanionError("Invalid \(name)") }
        return value.intValue
    }
    static func number(_ object: [String: Any], _ name: String, _ range: ClosedRange<Double>) throws -> Double {
        guard let value = object[name] as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(),
              value.doubleValue.isFinite, range.contains(value.doubleValue)
        else { throw CompanionError("Invalid \(name)") }
        return value.doubleValue
    }
    static func boolean(_ object: [String: Any], _ name: String) throws -> Bool {
        guard let value = object[name] as? NSNumber, CFGetTypeID(value) == CFBooleanGetTypeID()
        else { throw CompanionError("Invalid \(name)") }
        return value.boolValue
    }
}

struct Panel: Equatable {
    // H.264 High level 5.2: 2,073,600 macroblocks per second (MaxMBPS).
    static let maxMacroblocksPerSecond = 2_073_600

    static func h264SafeRate(width: Int, height: Int, requested: Int) -> Int {
        let macroblocks = max(1, ((width + 15) / 16) * ((height + 15) / 16))
        let levelRate = maxMacroblocksPerSecond / macroblocks
        guard levelRate < requested else { return requested }
        // One frame of headroom below the nominal limit, as the level allows.
        return max(1, levelRate - 1)
    }

    let width: Int
    let height: Int
    let scale: Int
    let fps: Int
    let codec: String

    init(hello: [String: Any]) throws {
        guard hello["type"] as? String == "hello", try Wire.integer(hello, "locachy", 1...1) == 1,
              try Wire.integer(hello, "pv", 3...3) == 3,
              try Wire.boolean(hello, "extendedOnly"),
              let input = hello["input"] as? [String], Set(Wire.capabilities).isSubset(of: Set(input)),
              let caps = hello["videoCaps"] as? [[String: Any]], !caps.isEmpty, caps.count <= 16
        else { throw CompanionError("Receiver does not support the authenticated extended-display profile") }
        let panel = hello["panel"] as? [String: Any] ?? hello
        width = try Wire.integer(panel, panel["width"] == nil ? "pixelsWide" : "width", 64...4096)
        height = try Wire.integer(panel, panel["height"] == nil ? "pixelsHigh" : "height", 64...4096)
        let requestedScale = try Wire.number(panel, "scale", 0.5...4)
        // CGVirtualDisplay supports integral 1x and 2x backing. Explicitly use
        // 1x for fractional panel scales instead of silently changing pixels.
        scale = requestedScale == 2 ? 2 : 1
        // VirtualCanvasSizing rounds to even points; capture verifies the
        // resulting mode and the receiver reads the real size from the SPS.
        guard width * height <= 16 * 1024 * 1024 else { throw CompanionError("Panel is too large") }
        // The receiver always sends displayMaxFrameRate, but absence means the
        // legacy 60 Hz contract. H.264 level 5.2 caps macroblocks per second,
        // so a large panel must lower its rate instead of relying on VT to
        // silently reject output.
        let requestedRate = hello["displayMaxFrameRate"] == nil ? 60
            : try Wire.integer(hello, "displayMaxFrameRate", 1...120)
        fps = Panel.h264SafeRate(width: width, height: height, requested: min(60, requestedRate))
        // Keep the first profile conservative and interoperable. HEVC is
        // negotiated only when H.264 is absent, never sent without an offer.
        // The capture path encodes H.264 only in this companion. A receiver
        // that offers HEVC alone must not be answered with an H.264 stream
        // labelled as anything else, so it is refused outright.
        let offered = caps.compactMap { $0["codec"] as? String }
        guard offered.contains("h264") else { throw CompanionError("Receiver does not offer H.264") }
        codec = "h264"
        guard caps.contains(where: { capability in
            guard capability["codec"] as? String == codec else { return false }
            for (key, required) in [("maxWidth", width), ("maxHeight", height), ("maxFramesPerSecond", fps), ("maxFrameRate", fps)] {
                if capability[key] != nil {
                    guard let maximum = try? Wire.integer(capability, key, 1...Int.max), maximum >= required else { return false }
                }
            }
            return true
        }) else { throw CompanionError("Panel exceeds the offered decoder envelope") }
    }
}

// A bounded incremental reader. Failure poisons the stream; no resynchronizing
// through attacker-controlled bytes after a malformed length or truncated EOF.
struct FrameReader {
    private var bytes = Data()
    private var failed = false
    mutating func feed(_ chunk: Data, receive: (Data) throws -> Void) throws {
        guard !failed else { throw CompanionError("Poisoned frame stream") }
        do {
            // Network reads are capped at 64 KiB. Process between appends so a
            // peer cannot inflate the accumulator with concatenated frames.
            guard chunk.count <= 65536 else { throw CompanionError("Read chunk too large") }
            bytes.append(chunk)
            while bytes.count >= 4 {
                let size = bytes.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
                guard size > 0, size <= Wire.maxControl else { throw CompanionError("Invalid control frame length") }
                guard bytes.count >= size + 4 else { break }
                let payload = Data(bytes.dropFirst(4).prefix(size))
                bytes.removeFirst(size + 4)
                try receive(payload)
            }
            guard bytes.count <= Wire.maxControl + 4 else { throw CompanionError("Control buffer overflow") }
        } catch { failed = true; bytes.removeAll(); throw error }
    }
    func finish() throws {
        guard !failed, bytes.isEmpty else { throw CompanionError("Truncated control frame") }
    }
}

// Encodes H.264 AVCC samples as complete Annex-B access units. Keyframes carry
// SPS/PPS. No telemetry prefix: the frozen LOCachy receiver requires 00000001.
enum AnnexB {
    static let start: [UInt8] = [0, 0, 0, 1]
    static func fromAVCC(_ bytes: UnsafeRawBufferPointer, parameterSets: [Data]) -> Data? {
        var output = Data(capacity: bytes.count + parameterSets.reduce(0) { $0 + $1.count + 4 })
        for set in parameterSets where !set.isEmpty { output.append(contentsOf: start); output.append(set) }
        var offset = 0
        while offset < bytes.count {
            guard bytes.count - offset >= 4 else { return nil }
            let size = bytes[offset..<(offset + 4)].reduce(0) { ($0 << 8) | Int($1) }
            offset += 4
            guard size > 0, size <= bytes.count - offset else { return nil }
            output.append(contentsOf: start)
            output.append(contentsOf: bytes[offset..<(offset + size)])
            offset += size
        }
        return output.starts(with: start) && output.count <= Wire.maxFrame ? output : nil
    }
}
