// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol and input-validation tests. Nothing here creates a display, reads
// the screen, posts an event or touches TCC: RemoteInput records the CGEvents
// it would post through an injected sink.
import CoreGraphics
import Foundation

final class Recorder {
    var events: [CGEventType] = []
    var posted: [CGEvent] = []
}

@main
enum ProtocolTests {
    static var failures = 0

    static func check(_ condition: Bool, _ name: String) {
        if condition {
            print("PASS: \(name)")
        } else {
            failures += 1
            print("FAIL: \(name)")
        }
    }

    static func expectThrow(_ name: String, _ body: () throws -> Void) {
        do {
            try body()
            check(false, name)
        } catch {
            check(true, name)
        }
    }

    static func hello(_ json: String) throws -> Panel {
        try Panel(hello: Wire.object(Data(json.utf8)))
    }

    // The exact text kvm-protocol.c emits for a 1920x1200 @2x H.264 panel.
    static let receiverHello = """
    {"type":"hello","pv":3,"locachy":1,"device":"LOCachy","pixelsWide":1920,"pixelsHigh":1200,"scale":2.000,"panel":{"width":1920,"height":1200,"scale":2.000},"displayMaxFrameRate":60,"extendedOnly":true,"videoCaps":[{"codec":"h264","maxWidth":4096,"maxHeight":4096,"maxFramesPerSecond":60}],"input":["key","button","cancel","pointer","scroll"]}
    """

    static func main() {
        panelTests()
        frameTests()
        annexBTests()
        inputTests()
        pairingTests()
        scrollSemanticTests()
        modifierTransitionTests()
        if failures == 0 {
            print("PASS: all protocol tests")
            exit(0)
        }
        print("FAIL: \(failures) protocol test(s)")
        exit(1)
    }

    static func panelTests() {
        do {
            let panel = try hello(receiverHello)
            check(panel.width == 1920 && panel.height == 1200 && panel.scale == 2 && panel.fps == 60 && panel.codec == "h264",
                  "accepts the exact receiver hello")
        } catch {
            check(false, "accepts the exact receiver hello: \(error)")
        }
        expectThrow("rejects HEVC-only receivers") {
            _ = try hello(receiverHello.replacingOccurrences(of: "\"codec\":\"h264\"", with: "\"codec\":\"hevc\""))
        }
        expectThrow("rejects a missing scroll capability") {
            _ = try hello(receiverHello.replacingOccurrences(of: ",\"scroll\"", with: ""))
        }
        expectThrow("rejects extendedOnly=false") {
            _ = try hello(receiverHello.replacingOccurrences(of: "\"extendedOnly\":true", with: "\"extendedOnly\":false"))
        }
        expectThrow("rejects protocol version 2") {
            _ = try hello(receiverHello.replacingOccurrences(of: "\"pv\":3", with: "\"pv\":2"))
        }
        expectThrow("rejects panels wider than 4096") {
            _ = try hello(receiverHello.replacingOccurrences(of: "\"width\":1920", with: "\"width\":5120"))
        }
        do {
            // 4096x2304 exceeds H.264 level 5.2 at 60 fps; the rate must drop.
            let big = try hello(receiverHello
                .replacingOccurrences(of: "\"width\":1920,\"height\":1200", with: "\"width\":4096,\"height\":2304"))
            check(big.fps == 55, "lowers the rate for a 4096x2304 panel to the H.264 macroblock limit")
        } catch {
            check(false, "lowers the rate for a 4096x2304 panel: \(error)")
        }
    }

    static func frameTests() {
        do {
            let frame = try Wire.frame(Data("{\"type\":\"ping\"}".utf8))
            var reader = FrameReader()
            var received: [Data] = []
            try reader.feed(frame.prefix(5)) { received.append($0) }
            try reader.feed(frame.dropFirst(5)) { received.append($0) }
            check(received.count == 1 && received[0] == Data("{\"type\":\"ping\"}".utf8),
                  "reassembles a frame split across reads")
        } catch {
            check(false, "reassembles a frame split across reads: \(error)")
        }
        do {
            let one = try Wire.frame(Data("{\"type\":\"a\"}".utf8))
            let two = try Wire.frame(Data("{\"type\":\"b\"}".utf8))
            var reader = FrameReader()
            var count = 0
            try reader.feed(one + two) { _ in count += 1 }
            check(count == 2, "delivers two frames packed in one read")
        } catch {
            check(false, "delivers two frames packed in one read: \(error)")
        }
        var zero = FrameReader()
        expectThrow("rejects a zero-length control frame") {
            try zero.feed(Data([0, 0, 0, 0])) { _ in }
        }
        expectThrow("a poisoned stream stays poisoned") {
            try zero.feed(Data([0, 0, 0, 2, 0x7b, 0x7d])) { _ in }
        }
        var oversized = FrameReader()
        expectThrow("rejects a control frame above the control limit") {
            try oversized.feed(Data([0, 0, 0x80, 0x00])) { _ in }
        }
        expectThrow("rejects a NUL byte inside control JSON") {
            _ = try Wire.object(Data([0x7b, 0x00, 0x7d]))
        }
        expectThrow("rejects a control payload that does not start with an object") {
            _ = try Wire.object(Data("[]".utf8))
        }
    }

    static func annexBTests() {
        let idr = Data([0x65, 0x88, 0x84])
        let slice = Data([0x41, 0x9a])
        var avcc = Data([0, 0, 0, 3]) + idr + Data([0, 0, 0, 2]) + slice
        let sets = [Data([0x67, 0x42]), Data([0x68, 0xce])]
        let expected = Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xce]) + Data([0, 0, 0, 1]) + idr
            + Data([0, 0, 0, 1]) + slice
        let output = avcc.withUnsafeBytes { AnnexB.fromAVCC($0, parameterSets: sets) }
        check(output == expected, "converts AVCC to Annex-B with SPS/PPS on keyframes")
        avcc = Data([0, 0, 0, 9, 0x65])
        check(avcc.withUnsafeBytes { AnnexB.fromAVCC($0, parameterSets: []) } == nil,
              "rejects an AVCC length that runs past the sample")
    }

    // A modifier key's own press must show the modifier and its release must
    // clear it, whether the message reports the state before or after the event.
    static func modifierTransitionTests() {
        let recorder = Recorder()
        let input = RemoteInput(bounds: { CGRect(x: 0, y: 0, width: 1920, height: 1200) },
                                sink: { recorder.posted.append($0) })
        func shiftKey(_ state: Int, modifiers: Int) throws {
            try input.handle(["type": "key", "device": 1, "code": 42, "keysym": 0xffe1,
                              "modifiers": modifiers, "locks": 0, "state": state, "mapping": "logical"])
        }
        do {
            try shiftKey(1, modifiers: 0)   // state before: nothing held; Shift goes down
            try shiftKey(0, modifiers: 1)   // state before: Shift held; Shift goes up
            check(recorder.posted.map { $0.flags.contains(.maskShift) } == [true, false],
                  "a Shift press shows Shift and its release clears it (state before the event)")
        } catch {
            check(false, "Shift transitions, state before: \(error)")
        }
        recorder.posted.removeAll()
        do {
            try shiftKey(1, modifiers: 1)   // state after: Shift now held
            try shiftKey(0, modifiers: 0)   // state after: Shift now released
            check(recorder.posted.map { $0.flags.contains(.maskShift) } == [true, false],
                  "a Shift press shows Shift and its release clears it (state after the event)")
        } catch {
            check(false, "Shift transitions, state after: \(error)")
        }
    }

    // Scroll semantics as the receiver sends them: wheel steps are phase-2
    // updates with source 1 and no gesture phase; trackpad gestures bracket
    // with begin/update/end.
    static func scrollSemanticTests() {
        let recorder = Recorder()
        let input = RemoteInput(bounds: { CGRect(x: 0, y: 0, width: 1920, height: 1200) },
                                sink: { recorder.posted.append($0) })
        do {
            try input.handle(["type": "scroll", "dx": 0, "dy": -15, "phase": 2, "source": 1])
            let wheel = recorder.posted.last!
            check(wheel.getIntegerValueField(.scrollWheelEventIsContinuous) == 0
                  && wheel.getIntegerValueField(.scrollWheelEventScrollPhase) == 0,
                  "a wheel step is a discrete event with no gesture phase")
        } catch {
            check(false, "wheel step: \(error)")
        }
        recorder.posted.removeAll()
        do {
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 1, "source": 2])
            try input.handle(["type": "scroll", "dx": 0, "dy": -4, "phase": 2, "source": 2])
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 3, "source": 2])
            let phases = recorder.posted.map { $0.getIntegerValueField(.scrollWheelEventScrollPhase) }
            let continuous = recorder.posted.map { $0.getIntegerValueField(.scrollWheelEventIsContinuous) }
            check(phases == [1, 2, 4] && continuous == [1, 1, 1],
                  "a trackpad gesture maps to began, changed, ended and stays continuous")
        } catch {
            check(false, "trackpad gesture: \(error)")
        }
        expectThrow("a wheel step cannot begin a gesture") {
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 1, "source": 1])
        }
    }

    static func code(_ text: String) throws -> Data {
        let bytes = Array(text.utf8)
        return try bytes.withUnsafeBufferPointer { try PairingCode.decode($0) }
    }

    static func pairingTests() {
        check((try? code("AAAAAAAAAAAAAAAAAAAAAAAAAA")) == Data(count: 16), "decodes the all-zero code")
        check((try? code("77777777777777777777777774")) == Data(repeating: 0xff, count: 16), "decodes the all-ones code")
        check((try? code("77777-77777-7777-7777-7777-7774")) == Data(repeating: 0xff, count: 16),
              "accepts the grouped display form")
        check((try? code("aaaaa aaaaa-aaaa aaaa-aaaa-aaaa")) == Data(count: 16), "is case-insensitive and ignores spaces")
        expectThrow("refuses 25 characters") { _ = try code("AAAAAAAAAAAAAAAAAAAAAAAAA") }
        expectThrow("refuses 27 characters") { _ = try code("AAAAAAAAAAAAAAAAAAAAAAAAAAA") }
        expectThrow("refuses non-canonical trailing bits") { _ = try code("AAAAAAAAAAAAAAAAAAAAAAAAAB") }
        expectThrow("refuses characters outside base32") { _ = try code("AAAAAAAAAAAAAAAAAAAAAAAAA1") }
        expectThrow("refuses base32 padding") { _ = try code("AAAAAAAAAAAAAAAAAAAAAAAAAA======") }
    }

    static func inputTests() {
        let recorder = Recorder()
        let input = RemoteInput(bounds: { CGRect(x: 0, y: 0, width: 1920, height: 1200) },
                                sink: { recorder.events.append($0.type); recorder.posted.append($0) })
        do {
            try input.handle(["type": "pointer", "x": 0.5, "y": 0.5])
            check(recorder.events == [.mouseMoved], "pointer move posts a mouse-moved event")
        } catch {
            check(false, "pointer move: \(error)")
        }
        recorder.events.removeAll()
        expectThrow("pointer coordinates outside [0,1] are refused") {
            try input.handle(["type": "pointer", "x": 1.5, "y": 0.5])
        }
        recorder.events.removeAll()
        do {
            try input.handle(["type": "button", "button": 273, "state": 1, "x": 0.1, "y": 0.1])
            try input.handle(["type": "cancel", "scope": "all"])
            check(recorder.events == [.rightMouseDown, .rightMouseUp], "cancel releases a held secondary button")
        } catch {
            check(false, "button then cancel: \(error)")
        }
        recorder.events.removeAll()
        expectThrow("an unknown button code is refused") {
            try input.handle(["type": "button", "button": 275, "state": 1, "x": 0.1, "y": 0.1])
        }
        recorder.events.removeAll()
        do {
            let key: [String: Any] = ["type": "key", "device": 1, "code": 30, "keysym": 0x61,
                                      "modifiers": 0, "locks": 0, "state": 1, "mapping": "logical"]
            try input.handle(key)
            var release = key
            release["state"] = 0
            try input.handle(release)
            check(recorder.events.count == 2 && recorder.events[0] == .keyDown && recorder.events[1] == .keyUp,
                  "logical key press and release produce a matched key pair")
        } catch {
            check(false, "logical key pair: \(error)")
        }
        recorder.events.removeAll()
        expectThrow("a key without logical mapping is refused") {
            try input.handle(["type": "key", "device": 1, "code": 30, "keysym": 0x61,
                              "modifiers": 0, "locks": 0, "state": 1])
        }
        expectThrow("a non-Unicode, non-special keysym is refused") {
            try input.handle(["type": "key", "device": 1, "code": 30, "keysym": 0x1000,
                              "modifiers": 0, "locks": 0, "state": 1, "mapping": "logical"])
        }
        recorder.events.removeAll()
        do {
            try input.handle(["type": "scroll", "dx": 0, "dy": -5, "phase": 2, "source": 0])
            check(recorder.events == [.scrollWheel], "numeric CG scroll phase posts a scroll event")
        } catch {
            check(false, "scroll with numeric phase: \(error)")
        }
        expectThrow("a scroll phase outside 0...4 is refused") {
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 5, "source": 0])
        }
        expectThrow("a scroll source outside 0...4 is refused") {
            try input.handle(["type": "scroll", "dx": 0, "dy": 1, "phase": 2, "source": 5])
        }
        expectThrow("scroll begin with non-zero deltas is refused") {
            try input.handle(["type": "scroll", "dx": 3, "dy": 0, "phase": 1, "source": 2])
        }
        recorder.events.removeAll()
        do {
            // begin, then cancel(): cancel must end the open gesture.
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 1, "source": 2])
            try input.handle(["type": "cancel", "scope": "all"])
            check(recorder.events == [.scrollWheel, .scrollWheel], "cancel ends an open scroll gesture")
        } catch {
            check(false, "scroll gesture cancel: \(error)")
        }
        recorder.events.removeAll()
        do {
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 1, "source": 2])
            try input.handle(["type": "scroll", "dx": 0, "dy": 0, "phase": 3, "source": 2])
            try input.handle(["type": "cancel", "scope": "all"])
            check(recorder.events == [.scrollWheel, .scrollWheel], "an ended gesture is not ended again by cancel")
        } catch {
            check(false, "scroll begin/end: \(error)")
        }
        expectThrow("a cancel with a non-'all' scope is refused") {
            try input.handle(["type": "cancel", "scope": "some"])
        }
    }
}
