// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import Carbon

struct KeyIdentity: Hashable { let device: Int; let code: Int }
// flags: the flags the key itself needs when pressed (layout shift/option,
// and the held Command/Control). modifier: set when this key is a modifier, so
// its own press shows it and its release clears it.
struct MappedKey { let code: CGKeyCode; let text: String?; let flags: CGEventFlags; var modifier: CGEventFlags = [] }

// Logical keys are resolved against the target's active Unicode keyboard layout,
// not a hard-coded US map. Press-time mappings survive until the matching up.
enum KeyMapping {
    static func flags(_ modifiers: Int, _ locks: Int) -> CGEventFlags {
        var result: CGEventFlags = []
        if modifiers & 1 != 0 { result.insert(.maskShift) }
        if modifiers & 2 != 0 { result.insert(.maskControl) }
        if modifiers & (4 | 16) != 0 { result.insert(.maskAlternate) }
        if modifiers & 8 != 0 { result.insert(.maskCommand) }
        if locks & 1 != 0 { result.insert(.maskAlphaShift) }
        return result
    }
    static func special(_ keysym: UInt32) -> CGKeyCode? {
        let codes: [UInt32: CGKeyCode] = [
            0xff08: 51, 0xff09: 48, 0xfe20: 48, 0xff0d: 36, 0xff1b: 53,
            0xffff: 117, 0xff50: 115, 0xff51: 123, 0xff52: 126, 0xff53: 124,
            0xff54: 125, 0xff55: 116, 0xff56: 121, 0xff57: 119, 0xff63: 114,
            0xffe1: 56, 0xffe2: 60, 0xffe3: 59, 0xffe4: 62,
            0xffe7: 55, 0xffe8: 54, 0xffe9: 58, 0xffea: 61,
            0xffeb: 55, 0xffec: 54, 0xfe03: 61, 0xffe5: 57,
            0xff7e: 104, 0xff23: 104, 0xff22: 102, 0xff2d: 104,
            0xff8d: 76, 0xffaa: 67, 0xffab: 69, 0xffad: 78, 0xffae: 65, 0xffaf: 75,
            0xffb0: 82, 0xffb1: 83, 0xffb2: 84, 0xffb3: 85, 0xffb4: 86,
            0xffb5: 87, 0xffb6: 88, 0xffb7: 89, 0xffb8: 91, 0xffb9: 92,
            0xffbe: 122, 0xffbf: 120, 0xffc0: 99, 0xffc1: 118, 0xffc2: 96,
            0xffc3: 97, 0xffc4: 98, 0xffc5: 100, 0xffc6: 101, 0xffc7: 109,
            0xffc8: 103, 0xffc9: 111, 0xffca: 105, 0xffcb: 107, 0xffcc: 113,
            0xffcd: 106, 0xffce: 64, 0xffcf: 79, 0xffd0: 80, 0xffd1: 90
        ]
        return codes[keysym]
    }
    // The modifier a modifier keysym contributes; empty for every other key.
    static func modifierBit(_ keysym: UInt32) -> CGEventFlags {
        switch keysym {
        case 0xffe1, 0xffe2: return .maskShift                  // Shift_L, Shift_R
        case 0xffe3, 0xffe4: return .maskControl                // Control_L, Control_R
        case 0xffe9, 0xffea, 0xfe03: return .maskAlternate      // Alt_L, Alt_R, ISO_Level3_Shift (AltGr)
        case 0xffe7, 0xffe8, 0xffeb, 0xffec: return .maskCommand // Meta_L/R, Super_L/R
        default: return []
        }
    }
    static func isModifier(_ code: CGKeyCode) -> Bool { [54,55,56,57,58,59,60,61,62,63].contains(code) }
    static func unicode(_ keysym: UInt32) -> String? {
        let value: UInt32
        if keysym >= 0x20 && keysym <= 0xff { value = keysym }
        else if keysym & 0xff000000 == 0x01000000 { value = keysym & 0xffffff }
        else { return nil }
        guard let scalar = UnicodeScalar(value), !CharacterSet.controlCharacters.contains(scalar) else { return nil }
        return String(scalar)
    }
    static func logical(_ keysym: UInt32, flags: CGEventFlags) throws -> MappedKey {
        if let code = special(keysym) { return MappedKey(code: code, text: nil, flags: flags) }
        guard let text = unicode(keysym) else { throw CompanionError("Unsupported logical keysym") }
        // A target IME may not expose a Unicode layout. In that case fall back
        // to the layout used beneath the input method, not to a guessed US map.
        let source = TISCopyCurrentKeyboardLayoutInputSource().takeRetainedValue()
        if let raw = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) {
            let data = Unmanaged<CFData>.fromOpaque(raw).takeUnretainedValue()
            let layout = UnsafeRawPointer(CFDataGetBytePtr(data)).assumingMemoryBound(to: UCKeyboardLayout.self)
            for combination: UInt32 in [0, UInt32(shiftKey >> 8), UInt32(optionKey >> 8), UInt32((shiftKey | optionKey) >> 8)] {
                for code: UInt16 in 0...127 {
                    var state: UInt32 = 0, length = 0
                    var chars = [UniChar](repeating: 0, count: 8)
                    let result = UCKeyTranslate(layout, code, UInt16(kUCKeyActionDown), combination,
                        UInt32(LMGetKbdType()), OptionBits(kUCKeyTranslateNoDeadKeysBit), &state, chars.count, &length, &chars)
                    if result == noErr, length > 0, String(utf16CodeUnits: chars, count: length) == text {
                        var mapped = flags
                        mapped.remove([.maskShift, .maskAlternate, .maskAlphaShift])
                        if combination & UInt32(shiftKey >> 8) != 0 { mapped.insert(.maskShift) }
                        if combination & UInt32(optionKey >> 8) != 0 { mapped.insert(.maskAlternate) }
                        return MappedKey(code: code, text: nil, flags: mapped)
                    }
                }
            }
        }
        // Unicode fallback is text, not a fabricated physical key. Refuse it
        // under shortcut modifiers, where it would silently invoke wrong keys.
        guard flags.intersection([.maskControl, .maskCommand, .maskAlternate]).isEmpty else {
            throw CompanionError("Logical shortcut is unavailable in the target keyboard layout")
        }
        return MappedKey(code: 0, text: text, flags: [])
    }
}

final class RemoteInput {
    typealias Sink = (CGEvent) -> Void
    private let bounds: () -> CGRect
    private let sink: Sink
    // A real HID-state source, as upstream InputInjector uses: menu tracking
    // treats sourceless synthetic clicks as malformed.
    private let source = CGEventSource(stateID: .hidSystemState)
    private var held: [KeyIdentity: MappedKey] = [:]
    private var buttons = Set<Int>()
    private var lastPoint = CGPoint.zero
    private var flags: CGEventFlags = []
    private var scrollX = 0.0, scrollY = 0.0
    private var scrollActive = false
    private var lastClickTime = [Int: TimeInterval]()
    private var lastClickPoint = [Int: CGPoint]()
    private var clickCounts = [Int: Int64]()

    init(displayID: CGDirectDisplayID) {
        bounds = { CGDisplayBounds(displayID) }
        sink = { $0.post(tap: .cghidEventTap) }
    }
    // Unit tests create CGEvents but never post, query a display or request TCC.
    init(bounds: @escaping () -> CGRect, sink: @escaping Sink) { self.bounds = bounds; self.sink = sink }
    var heldCount: Int { held.count + buttons.count }

    private func point(_ object: [String: Any]) throws -> CGPoint {
        let x = try Wire.number(object, "x", 0...1), y = try Wire.number(object, "y", 0...1)
        let rect = bounds()
        guard !rect.isEmpty else { throw CompanionError("Virtual display is unavailable") }
        return CGPoint(x: rect.minX + x * max(0, rect.width - 1), y: rect.minY + y * max(0, rect.height - 1))
    }
    private func postKey(_ mapped: MappedKey, down: Bool, flags: CGEventFlags, repeatKey: Bool = false) {
        guard let event = CGEvent(keyboardEventSource: source, virtualKey: mapped.code, keyDown: down) else { return }
        event.flags = flags
        if KeyMapping.isModifier(mapped.code) { event.type = .flagsChanged }
        event.setIntegerValueField(.keyboardEventAutorepeat, value: repeatKey ? 1 : 0)
        if let text = mapped.text {
            let utf16 = Array(text.utf16)
            utf16.withUnsafeBufferPointer { event.keyboardSetUnicodeString(stringLength: $0.count, unicodeString: $0.baseAddress) }
        }
        sink(event)
    }
    private func mouse(_ button: Int, _ down: Bool, at point: CGPoint, cancelled: Bool = false) {
        let cgButton: CGMouseButton = button == 272 ? .left : button == 273 ? .right : .center
        let type: CGEventType = button == 272 ? (down ? .leftMouseDown : .leftMouseUp)
            : button == 273 ? (down ? .rightMouseDown : .rightMouseUp) : (down ? .otherMouseDown : .otherMouseUp)
        guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: cgButton) else { return }
        if down {
            let now = ProcessInfo.processInfo.systemUptime, previous = lastClickPoint[button] ?? point
            let close = hypot(point.x - previous.x, point.y - previous.y) <= 4
            let soon = now - (lastClickTime[button] ?? -100) <= NSEvent.doubleClickInterval
            clickCounts[button] = soon && close ? min(3, (clickCounts[button] ?? 0) + 1) : 1
            lastClickTime[button] = now; lastClickPoint[button] = point
        }
        event.flags = flags
        event.setIntegerValueField(.mouseEventClickState, value: cancelled ? 0 : clickCounts[button] ?? 1)
        sink(event)
    }
    func handle(_ object: [String: Any], scrollScale: Double = 1) throws {
        guard let type = object["type"] as? String else { throw CompanionError("Missing input type") }
        switch type {
        case "key":
            let identity = KeyIdentity(device: try Wire.integer(object, "device", 1...Int(UInt32.max)), code: try Wire.integer(object, "code", 0...767))
            let state = try Wire.integer(object, "state", 0...2)
            let modifiers = try Wire.integer(object, "modifiers", 0...127), locks = try Wire.integer(object, "locks", 0...3)
            guard object["mapping"] as? String == "logical" else { throw CompanionError("Unsupported keyboard mapping") }
            // Modifiers and locks are the state before this event. A modifier
            // key's own transition is applied on top of it, so its press shows
            // the modifier and its release clears it. Reporting the state after
            // the event would give the same result, since both are idempotent.
            var before = KeyMapping.flags(modifiers, locks)
            if state == 0 {
                guard let mapped = held.removeValue(forKey: identity) else { return }
                before.subtract(mapped.modifier)
                flags = before
                // Multiple physical keyboards may hold the same logical key.
                if !held.values.contains(where: { $0.code == mapped.code && $0.text == mapped.text }) {
                    postKey(mapped, down: false, flags: before)
                }
            } else if state == 2 {
                if let mapped = held[identity] {
                    postKey(mapped, down: true, flags: mapped.flags.union(mapped.modifier), repeatKey: true)
                }
            } else if held[identity] == nil {
                guard held.count < 128 else { throw CompanionError("Too many held keys") }
                let sym = UInt32(try Wire.integer(object, "keysym", 0...Int(UInt32.max)))
                var mapped = try KeyMapping.logical(sym, flags: before)
                mapped.modifier = KeyMapping.modifierBit(sym)
                flags = before.union(mapped.modifier)
                held[identity] = mapped
                postKey(mapped, down: true, flags: mapped.flags.union(mapped.modifier))
            }
        case "pointer":
            lastPoint = try point(object)
            let button = buttons.contains(272) ? 272 : buttons.contains(273) ? 273 : 274
            let cgButton: CGMouseButton = button == 272 ? .left : button == 273 ? .right : .center
            let type: CGEventType = buttons.isEmpty ? .mouseMoved : button == 272 ? .leftMouseDragged : button == 273 ? .rightMouseDragged : .otherMouseDragged
            if let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: lastPoint, mouseButton: cgButton) {
                event.flags = flags; sink(event)
            }
        case "button":
            let button = try Wire.integer(object, "button", 272...274), down = try Wire.integer(object, "state", 0...1) == 1
            lastPoint = try point(object)
            if down ? buttons.insert(button).inserted : buttons.remove(button) != nil { mouse(button, down, at: lastPoint) }
        case "touch":
            let phase = object["phase"] as? String
            guard ["began", "moved", "ended", "cancelled"].contains(phase ?? "") else { throw CompanionError("Invalid touch phase") }
            let p = try point(object)
            if phase == "moved" { try handle(["type": "pointer", "x": object["x"]!, "y": object["y"]!]) }
            else if phase == "began" { if buttons.insert(272).inserted { mouse(272, true, at: p) } }
            else if buttons.remove(272) != nil { mouse(272, false, at: p, cancelled: phase == "cancelled") }
            lastPoint = p
        case "scroll":
            guard scrollScale.isFinite, scrollScale > 0, scrollScale <= 4 else { throw CompanionError("Invalid scroll scale") }
            // Receiver phase: 0 none (discrete), 1 begin, 2 update, 3 end,
            // 4 cancel. Absent phase/source is upstream OpenDisplay's plain
            // discrete scroll. Mapped explicitly to CGScrollPhase bits.
            let phase = object["phase"] == nil ? 0 : try Wire.integer(object, "phase", 0...4)
            let origin = object["source"] == nil ? 0 : try Wire.integer(object, "source", 0...4)
            let rawX = try Wire.number(object, "dx", -16384...16384)
            let rawY = try Wire.number(object, "dy", -16384...16384)
            guard [1, 3, 4].contains(phase) ? (rawX == 0 && rawY == 0) : true else {
                throw CompanionError("Scroll begin/end/cancel must carry zero deltas")
            }
            // The receiver sends wheel steps (source 1) as phase 2 with deltas
            // and never brackets them with begin/end. A wheel step is a plain
            // non-continuous wheel event with no gesture phase; a gesture phase
            // without a begin would look like a broken trackpad gesture.
            let wheel = origin == 1
            if wheel && phase != 0 && phase != 2 {
                throw CompanionError("A wheel step carries no gesture begin, end or cancel")
            }
            let cgPhase: Int64 = wheel ? 0 : [0: 0, 1: 1, 2: 2, 3: 4, 4: 8][phase]!
            let continuous: Int64 = wheel || (phase == 0 && origin == 0) ? 0 : 1
            let rect = bounds()
            guard rect.width > 0 else { throw CompanionError("Virtual display is unavailable") }
            let dx = rawX * scrollScale, dy = rawY * scrollScale
            scrollX += dx; scrollY += dy
            let x = scrollX.rounded(.towardZero), y = scrollY.rounded(.towardZero)
            scrollX -= x; scrollY -= y
            postScroll(wheelX: Int32(x), wheelY: Int32(y), pointX: dx, pointY: dy, phase: cgPhase, continuous: continuous)
            if !wheel && phase == 1 { scrollActive = true }
            if phase == 3 || phase == 4 { scrollActive = false; scrollX = 0; scrollY = 0 }
        case "cancel":
            guard object["scope"] as? String == "all" else { throw CompanionError("Invalid cancel scope") }
            cancel()
        default: break
        }
    }
    private func postScroll(wheelX: Int32, wheelY: Int32, pointX: Double, pointY: Double, phase: Int64, continuous: Int64) {
        guard let event = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 2,
                                  wheel1: wheelY, wheel2: wheelX, wheel3: 0) else { return }
        event.flags = flags
        event.setDoubleValueField(.scrollWheelEventPointDeltaAxis1, value: pointY)
        event.setDoubleValueField(.scrollWheelEventPointDeltaAxis2, value: pointX)
        event.setIntegerValueField(.scrollWheelEventIsContinuous, value: continuous)
        event.setIntegerValueField(.scrollWheelEventScrollPhase, value: phase)
        sink(event)
    }

    func cancel() {
        // End an open scroll gesture first so the target does not keep a
        // phantom trackpad gesture alive after focus loss or teardown.
        if scrollActive { postScroll(wheelX: 0, wheelY: 0, pointX: 0, pointY: 0, phase: 8, continuous: 1) }
        scrollActive = false
        flags = []
        var released = Set<CGKeyCode>()
        for mapped in held.values where released.insert(mapped.code).inserted {
            postKey(mapped, down: false, flags: [])
        }
        held.removeAll()
        for button in buttons { mouse(button, false, at: lastPoint, cancelled: true) }
        buttons.removeAll()
        scrollX = 0; scrollY = 0
    }
}
