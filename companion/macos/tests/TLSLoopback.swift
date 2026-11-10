// SPDX-License-Identifier: GPL-3.0-or-later
// Loopback interoperability client for the cable profile. It uses production
// tlsOptions() and PairingCode.read(); the code arrives on stdin as a pipe.
// Loopback is used only here: production never dials 127.0.0.1.
import Foundation
import Network

@main
enum TLSLoopback {
    static func main() {
        let arguments = CommandLine.arguments
        let expectations = ["ready", "reject-tls", "refuse"]
        guard arguments.count == 5, arguments[1] == "--port", let port = UInt16(arguments[2]),
              arguments[3] == "--expect", expectations.contains(arguments[4]) else {
            fputs("usage: tls-loopback --port P --expect ready|reject-tls|refuse < code-pipe\n", stderr)
            exit(2)
        }
        var key: Data
        do {
            key = try PairingCode.read()
        } catch {
            print("FAIL: \(error)")
            exit(1)
        }
        let queue = DispatchQueue(label: "locachy.tls-loopback")
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.connectionTimeout = 5
        let parameters = NWParameters(tls: SecureTransport.tlsOptions(key: key, queue: queue), tcp: tcp)
        key.resetBytes(in: 0..<key.count)
        let connection = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: parameters)
        let finished = DispatchSemaphore(value: 0)
        var outcome = "no state within 8 seconds"
        var profileAccepted = false
        var tlsFailed = false

        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                profileAccepted = SecureTransport.profileOK(connection)
                outcome = profileAccepted ? "ready" : "ready with a refused TLS profile"
                let frame = try! Wire.frame(Wire.json(["type": "ping"]))
                connection.send(content: frame, completion: .contentProcessed { _ in
                    Thread.sleep(forTimeInterval: 0.5)
                    finished.signal()
                })
            case .failed(let error), .waiting(let error):
                // Only a TLS-layer error proves the handshake itself was
                // refused; refused or reset TCP says nothing about the profile.
                if case .tls = error { tlsFailed = true }
                outcome = "\(error)"
                finished.signal()
            default:
                break
            }
        }
        connection.start(queue: queue)
        _ = finished.wait(timeout: .now() + 8)
        connection.cancel()

        switch arguments[4] {
        case "ready":
            if profileAccepted {
                print("PASS: TLS 1.2 PSK_AES128_GCM handshake, identity \(Wire.identity), ALPN \(Wire.alpn)")
                exit(0)
            }
            print("FAIL: \(outcome)")
            exit(1)
        case "reject-tls":
            if !profileAccepted && tlsFailed {
                print("PASS: handshake refused at the TLS layer (\(outcome))")
                exit(0)
            }
            print("FAIL: expected a TLS-layer refusal, got: \(outcome)")
            exit(1)
        default:
            // Hard failure either way: the peer refuses at the TLS layer, or
            // the handshake completes and production's profile check refuses
            // it before any application data (Network.framework offers all of
            // its PSK suites regardless of the requested one).
            if !profileAccepted && (tlsFailed || outcome.hasPrefix("ready")) {
                print("PASS: session refused (\(tlsFailed ? "TLS layer: " + outcome : "profile check after handshake"))")
                exit(0)
            }
            print("FAIL: expected a refusal, got: \(outcome)")
            exit(1)
        }
    }
}
