// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import Network
import Security
import Darwin

struct CableEndpoint {
    let host: String
    let port: UInt16
    let interface: String

    init(host: String, port: UInt16, interface: String) throws {
        guard port > 0, !interface.isEmpty, if_nametoindex(interface) != 0 else { throw CompanionError("Invalid dedicated interface or port") }
        // Never DNS-resolve a name or route to a non-link-local network.
        let parts = host.split(separator: "%", omittingEmptySubsequences: false)
        let bare = String(parts[0])
        guard parts.count <= 2, parts.count == 1 || String(parts[1]) == interface else { throw CompanionError("Address zone must match the selected interface") }
        var v4 = in_addr(), v6 = in6_addr()
        let is4 = bare.withCString { inet_pton(AF_INET, $0, &v4) } == 1
        let is6 = bare.withCString { inet_pton(AF_INET6, $0, &v6) } == 1
        if is4 {
            let octets = withUnsafeBytes(of: v4.s_addr) { Array($0) }
            guard octets[0] == 169, octets[1] == 254, parts.count == 1 else { throw CompanionError("Only link-local addresses are accepted") }
            self.host = bare
        } else if is6 {
            let octets = withUnsafeBytes(of: v6) { Array($0) }
            guard octets[0] == 0xfe, octets[1] & 0xc0 == 0x80 else { throw CompanionError("Only link-local addresses are accepted") }
            self.host = bare + "%" + interface
        } else { throw CompanionError("A numeric link-local address is required") }
        self.port = port
        self.interface = interface
    }
}

private func dispatchBytes(_ data: Data) -> dispatch_data_t {
    // DispatchData copies bytes here. The TLS implementation owns that copy
    // until its options and connection are released; no secret is persisted.
    let copied = data.withUnsafeBytes { DispatchData(bytes: $0) }
    return copied as __DispatchData
}

final class SecureTransport {
    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "locachy.companion.transport")
    private var reader = FrameReader()
    private var timer: DispatchSourceTimer?
    private var lastReceived = ProcessInfo.processInfo.systemUptime
    private var ready = false
    private var ended = false
    private var queuedVideo = 0
    private var queuedControl = 0
    var onReady: (() -> Void)?
    var onControl: (([String: Any]) -> Void)?
    var onEnd: ((String) -> Void)?

    // Shared by production and the loopback interoperability test, so the
    // tested TLS profile is exactly the one used on the dedicated cable.
    static func tlsOptions(key: Data, queue: DispatchQueue) -> NWProtocolTLS.Options {
        let tls = NWProtocolTLS.Options()
        let options = tls.securityProtocolOptions
        // TLS 1.2 PSK: the Apple client cannot complete external TLS 1.3 PSK
        // (verified against OpenSSL 4.1; see README). One non-forward-secret
        // suite is offered and required; no certificate path exists.
        sec_protocol_options_set_min_tls_protocol_version(options, .TLSv12)
        sec_protocol_options_set_max_tls_protocol_version(options, .TLSv12)
        sec_protocol_options_append_tls_ciphersuite(options, tls_ciphersuite_t(rawValue: Wire.pskSuite)!)
        sec_protocol_options_add_tls_application_protocol(options, Wire.alpn)
        sec_protocol_options_add_pre_shared_key(options, dispatchBytes(key), dispatchBytes(Data(Wire.identity.utf8)))
        sec_protocol_options_set_tls_tickets_enabled(options, false)
        sec_protocol_options_set_tls_resumption_enabled(options, false)
        // TLS 1.2 renegotiation defaults to enabled in Network.framework.
        sec_protocol_options_set_tls_renegotiation_enabled(options, false)
        // PSK is the only authentication method; never accept a certificate.
        sec_protocol_options_set_verify_block(options, { _, _, completion in completion(false) }, queue)
        return tls
    }

    static func profileOK(_ connection: NWConnection) -> Bool {
        guard let metadata = connection.metadata(definition: NWProtocolTLS.definition) as? NWProtocolTLS.Metadata else { return false }
        let m = metadata.securityProtocolMetadata
        return sec_protocol_metadata_get_negotiated_tls_protocol_version(m) == .TLSv12
            && sec_protocol_metadata_get_negotiated_tls_ciphersuite(m).rawValue == Wire.pskSuite
            && negotiatedProtocol(m) == Wire.alpn
    }

    // The cable profile: TLS above, Nagle off, bounded connect, and no path
    // other than the one named interface. Wi-Fi, cellular and loopback are
    // prohibited outright so a missing cable can never fall back to them.
    static func cableParameters(key: Data, queue: DispatchQueue, interface: NWInterface) -> NWParameters {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.connectionTimeout = 8
        let parameters = NWParameters(tls: tlsOptions(key: key, queue: queue), tcp: tcp)
        parameters.prohibitedInterfaceTypes = [.wifi, .cellular, .loopback]
        parameters.prohibitExpensivePaths = true
        parameters.allowLocalEndpointReuse = false
        parameters.requiredInterface = interface
        return parameters
    }

    // NWInterface objects only come from a started monitor's path updates; a
    // monitor that was never started reports no interfaces at all.
    static func resolveInterface(named name: String, timeout: TimeInterval = 3) throws -> NWInterface {
        let monitor = NWPathMonitor()
        let lock = NSLock()
        let done = DispatchSemaphore(value: 0)
        var found: NWInterface?
        var signalled = false
        monitor.pathUpdateHandler = { path in
            lock.lock(); defer { lock.unlock() }
            guard !signalled, let match = path.availableInterfaces.first(where: { $0.name == name }) else { return }
            found = match
            signalled = true
            done.signal()
        }
        monitor.start(queue: DispatchQueue(label: "locachy.companion.interface"))
        defer { monitor.cancel() }
        _ = done.wait(timeout: .now() + timeout)
        lock.lock(); defer { lock.unlock() }
        guard let selected = found else { throw CompanionError("Selected interface is unavailable to Network.framework") }
        guard selected.type == .wiredEthernet || selected.type == .other else {
            throw CompanionError("Selected interface is not a dedicated wired path")
        }
        return selected
    }

    init(endpoint: CableEndpoint, key: Data) throws {
        guard key.count == PairingCode.bytes else { throw CompanionError("Pairing code must be 16 bytes") }
        let selected = try Self.resolveInterface(named: endpoint.interface)
        let parameters = Self.cableParameters(key: key, queue: queue, interface: selected)
        connection = NWConnection(host: NWEndpoint.Host(endpoint.host), port: NWEndpoint.Port(rawValue: endpoint.port)!, using: parameters)
    }

    func start() {
        queue.async { [weak self] in
            guard let self, let connection = self.connection else { return }
            connection.stateUpdateHandler = { [weak self] state in
                guard let self, !self.ended else { return }
                switch state {
                case .ready:
                    guard Self.profileOK(connection) else { self.finish("TLS profile negotiation failed"); return }
                    self.ready = true
                    self.lastReceived = ProcessInfo.processInfo.systemUptime
                    self.onReady?()
                    self.receive()
                case .waiting, .failed:
                    self.finish("Dedicated cable connection failed")
                case .cancelled:
                    self.finish("Connection closed")
                default: break
                }
            }
            connection.start(queue: self.queue)
            let timer = DispatchSource.makeTimerSource(queue: self.queue)
            timer.schedule(deadline: .now() + 2, repeating: 2)
            timer.setEventHandler { [weak self] in
                guard let self, !self.ended else { return }
                guard ProcessInfo.processInfo.systemUptime - self.lastReceived < 8 else { self.finish("Cable session timed out"); return }
                if self.ready { self.sendObject(["type": "ping"]) }
            }
            self.timer = timer
            timer.resume()
        }
    }

    static func negotiatedProtocol(_ metadata: sec_protocol_metadata_t) -> String? {
        if #available(macOS 15.5, *) {
            guard let copied = sec_protocol_metadata_copy_negotiated_protocol(metadata) else { return nil }
            defer { free(UnsafeMutableRawPointer(mutating: copied)) }
            return String(cString: copied)
        }
        guard let name = sec_protocol_metadata_get_negotiated_protocol(metadata) else { return nil }
        return String(cString: name)
    }

    private func receive() {
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, complete, error in
            guard let self, !self.ended else { return }
            do {
                if let data, !data.isEmpty {
                    self.lastReceived = ProcessInfo.processInfo.systemUptime
                    try self.reader.feed(data) { payload in self.onControl?(try Wire.object(payload)) }
                }
                if complete || error != nil {
                    try self.reader.finish()
                    self.finish("Cable disconnected")
                } else { self.receive() }
            } catch { self.finish("Invalid control stream") }
        }
    }
    func sendObject(_ object: [String: Any]) {
        do { try send(try Wire.json(object), video: false) }
        catch { stop("Control message could not be sent") }
    }
    func sendVideo(_ data: Data) throws { try send(data, video: true) }
    private func send(_ payload: Data, video: Bool) throws {
        let framed = try Wire.frame(payload)
        queue.async { [weak self] in
            guard let self, self.ready, !self.ended, let connection = self.connection else { return }
            // Never discard an encoded P frame: doing so would break its
            // reference chain. A saturated connection ends instead of growing
            // an unbounded queue or leaving a frozen screen controlling a Mac.
            guard video ? self.queuedVideo < 3 : self.queuedControl < 64 else { self.finish("Peer is not consuming the stream"); return }
            if video { self.queuedVideo += 1 } else { self.queuedControl += 1 }
            connection.send(content: framed, completion: .contentProcessed { [weak self] error in
                guard let self else { return }
                if video { self.queuedVideo -= 1 } else { self.queuedControl -= 1 }
                if error != nil { self.finish("Cable send failed") }
            })
        }
    }
    func stop(_ reason: String) { queue.async { self.finish(reason) } }
    private func finish(_ reason: String) {
        guard !ended else { return }
        ended = true
        ready = false
        timer?.cancel(); timer = nil
        connection?.stateUpdateHandler = nil
        connection?.cancel(); connection = nil
        onEnd?(reason)
    }
}
