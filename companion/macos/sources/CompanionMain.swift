// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import Foundation

struct Options {
    let endpoint: CableEndpoint?
    let check: Bool

    init(_ arguments: [String]) throws {
        if arguments == ["--check"] { endpoint = nil; check = true; return }
        var values: [String: String] = [:]
        var index = 0
        while index < arguments.count {
            let name = arguments[index]
            guard ["--interface", "--address", "--port"].contains(name), index + 1 < arguments.count,
                  values[name] == nil else { throw CompanionError(Self.usage) }
            values[name] = arguments[index + 1]
            index += 2
        }
        guard let interface = values["--interface"], let address = values["--address"],
              let portText = values["--port"], let port = UInt16(portText)
        else { throw CompanionError(Self.usage) }
        endpoint = try CableEndpoint(host: address, port: port, interface: interface)
        check = false
    }

    static let usage = """
    usage: locachy-macos-sender --interface IFACE --address LINK_LOCAL --port PORT
           locachy-macos-sender --check
    The pairing code shown on the LOCachy screen is typed at the prompt; it is
    never accepted in arguments or the environment.
    """
}

@available(macOS 14.0, *)
final class Companion {
    private let transport: SecureTransport
    private var capture: CaptureSession?
    private var activePanel: Panel?
    private var finished = false
    private var signals: [DispatchSourceSignal] = []

    init(options: Options) throws {
        var key = try PairingCode.read()
        defer { key.resetBytes(in: 0..<key.count) }
        // The TLS options keep their own copy for the handshake; this one is
        // cleared as soon as the connection object holds it.
        transport = try SecureTransport(endpoint: options.endpoint!, key: key)
        transport.onControl = { [weak self] object in DispatchQueue.main.async { self?.control(object) } }
        transport.onEnd = { [weak self] reason in DispatchQueue.main.async { self?.finish(reason, code: 1) } }
    }

    func run() {
        for number in [SIGINT, SIGTERM, SIGHUP] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { [weak self] in self?.finish("Stopped by signal", code: 0) }
            source.resume()
            signals.append(source)
        }
        CompanionLog.info("Connecting over the selected dedicated link")
        transport.start()
    }

    private func control(_ object: [String: Any]) {
        guard !finished, let type = object["type"] as? String else { return }
        do {
            switch type {
            case "hello":
                let panel = try Panel(hello: object)
                if let current = activePanel {
                    // welcome is sent exactly once. v1 never resizes a live
                    // output: a changed panel ends the session so the next one
                    // is created at the new size.
                    if current != panel { finish("Receiver panel changed; reconnect to resize", code: 0) }
                    return
                }
                activePanel = panel
                transport.sendObject(["type": "welcome", "locachy": 1, "extended": true, "input": Wire.capabilities])
                let session = CaptureSession(transport: transport, panel: panel)
                capture = session
                Task { @MainActor in
                    do { try await session.start() }
                    catch { self.finish(String(describing: error), code: 1) }
                }
            case "ping":
                var pong: [String: Any] = ["type": "pong", "mt": Int(Date().timeIntervalSince1970 * 1000)]
                if object["t"] != nil { pong["t"] = try Wire.integer(object, "t", 0...Int.max) }
                transport.sendObject(pong)
            case "kf":
                capture?.requestKeyframe()
            case "closing", "sleeping":
                finish("Receiver ended the session", code: 0)
            case "key", "pointer", "button", "scroll", "cancel", "touch":
                guard let capture else { return }
                Task { @MainActor in
                    do { try capture.handleInput(object) }
                    catch { self.finish("Rejected malformed remote input", code: 1) }
                }
            default:
                break  // Unknown types are ignored, per the protocol.
            }
        } catch {
            finish(String(describing: error), code: 1)
        }
    }

    private func finish(_ reason: String, code: Int32) {
        guard !finished else { return }
        finished = true
        Task { @MainActor in
            // Releases held keys/buttons, ends any scroll gesture, stops
            // capture and drops the only owner of the virtual display.
            self.capture?.stop(reason)
            self.capture = nil
            self.transport.stop(reason)
            CompanionLog.info(reason)
            // Give WindowServer a moment to process the display release.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { exit(code) }
        }
    }
}

@main
enum CompanionMain {
    static func main() {
        do {
            let options = try Options(Array(CommandLine.arguments.dropFirst()))
            if options.check {
                let available = ["CGVirtualDisplay", "CGVirtualDisplayDescriptor", "CGVirtualDisplaySettings", "CGVirtualDisplayMode"]
                    .allSatisfy { NSClassFromString($0) != nil }
                print(available ? "PASS: CGVirtualDisplay classes are present; no display was created"
                                : "FAIL: CGVirtualDisplay is unavailable on this macOS")
                exit(available ? 0 : 1)
            }
            guard #available(macOS 14.0, *) else { throw CompanionError("macOS 14 or later is required") }
            let companion = try Companion(options: options)
            companion.run()
            withExtendedLifetime(companion) { dispatchMain() }
        } catch {
            fputs("locachy-macos: \(error)\n", stderr)
            exit(2)
        }
    }
}
