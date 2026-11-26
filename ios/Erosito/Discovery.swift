// Keresés a helyi hálózaton: Bonjour "_erosito._tcp" (ha az ESP mDNS-sel hirdeti) és IP-szkennelés (/api/id), ha a multicast nem jut át
import Foundation
import Network
import Observation

struct FoundDevice: Identifiable, Hashable, Sendable {
    let id: String     // Bonjour szolgáltatásnév vagy IP-cím
    let name: String
    let host: String   // IPv4-cím (és port, ha nem 80)
}

@MainActor
@Observable
final class Discovery {
    private(set) var devices: [FoundDevice] = []
    private(set) var searching = false
    private var browser: NWBrowser?
    private var scanTask: Task<Void, Never>?

    func start() {
        stop()
        devices = []
        searching = true
        let newBrowser = NWBrowser(for: .bonjour(type: "_erosito._tcp", domain: nil), using: .tcp)
        newBrowser.browseResultsChangedHandler = { [weak self] results, _ in
            let endpoints = results.map(\.endpoint)
            Task { @MainActor in await self?.resolveAll(endpoints) }
        }
        newBrowser.start(queue: .main)
        browser = newBrowser
        scanTask = Task { [weak self] in   // a Bonjourral párhuzamosan: végigpróbálja a saját /24 hálózatot
            await self?.scanSubnet()
            self?.searching = false
        }
    }

    func stop() {
        browser?.cancel()
        browser = nil
        scanTask?.cancel()
        searching = false
    }

    private func add(name: String, host: String) {
        if devices.contains(where: { $0.host == host }) { return }
        devices.append(FoundDevice(id: host, name: name, host: host))
    }

    // ---- Bonjour ----
    private func resolveAll(_ endpoints: [NWEndpoint]) async {
        for endpoint in endpoints {
            guard case let .service(name, _, _, _) = endpoint else { continue }
            if let host = await Self.resolve(endpoint) { add(name: name, host: host) }
        }
    }

    /// A szolgáltatás feloldása IPv4-címre egy rövid TCP-kapcsolattal (legfeljebb 5 s)
    nonisolated static func resolve(_ endpoint: NWEndpoint) async -> String? {
        await withCheckedContinuation { continuation in
            let params = NWParameters.tcp
            if let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
                ip.version = .v4
            }
            let connection = NWConnection(to: endpoint, using: params)
            let once = OnceFlag()
            connection.stateUpdateHandler = { state in
                switch state {
                case .ready:
                    var result: String?
                    if case let .hostPort(host, port)? = connection.currentPath?.remoteEndpoint {
                        let address: String?
                        switch host {
                        case .ipv4(let v4): address = "\(v4)"
                        case .name(let name, _): address = name
                        default: address = nil
                        }
                        if let address { result = port.rawValue == 80 ? address : "\(address):\(port.rawValue)" }
                    }
                    if once.claim() {
                        connection.cancel()
                        continuation.resume(returning: result)
                    }
                case .failed, .cancelled:
                    connection.cancel()
                    if once.claim() { continuation.resume(returning: nil) }
                default:
                    break
                }
            }
            connection.start(queue: .global())
            DispatchQueue.global().asyncAfter(deadline: .now() + 5) {
                if once.claim() {
                    connection.cancel()
                    continuation.resume(returning: nil)
                }
            }
        }
    }

    // ---- IP-szkennelés: a saját /24 hálózat címein az /api/id (jelszó nélkül, csak azonosít) ----
    private func scanSubnet() async {
        guard let (ip, mask) = Self.localIPv4() else { return }
        var net = ip & mask
        var size = ~mask
        if size > 255 {   // nagy hálózatnál csak a telefon saját /24 tartománya
            net = ip & 0xFFFF_FF00
            size = 255
        }
        guard size >= 2 else { return }
        let candidates = (1..<size).map { net | $0 }.filter { $0 != ip }
        var iterator = candidates.makeIterator()
        await withTaskGroup(of: String?.self) { group in
            for _ in 0..<32 {   // egyszerre legfeljebb 32 kérés
                if let next = iterator.next() { group.addTask { await Self.probe(Self.ipString(next)) } }
            }
            while let result = await group.next() {
                if Task.isCancelled { group.cancelAll(); break }
                if let host = result { add(name: "Erősítő", host: host) }
                if let next = iterator.next() { group.addTask { await Self.probe(Self.ipString(next)) } }
            }
        }
    }

    nonisolated static let scanSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 1.5
        config.timeoutIntervalForResource = 2
        config.waitsForConnectivity = false
        return URLSession(configuration: config)
    }()

    nonisolated static func probe(_ host: String) async -> String? {
        guard let url = URL(string: "http://\(host)/api/id"),
              let result = try? await scanSession.data(from: url),
              (result.1 as? HTTPURLResponse)?.statusCode == 200,
              let object = try? JSONSerialization.jsonObject(with: result.0) as? [String: Any],
              object["device"] as? String == "erosito" else { return nil }
        return host
    }

    nonisolated static func ipString(_ a: UInt32) -> String {
        "\(a >> 24 & 0xFF).\(a >> 16 & 0xFF).\(a >> 8 & 0xFF).\(a & 0xFF)"
    }

    /// A telefon WiFi (en0) IPv4-címe és hálózati maszkja
    nonisolated static func localIPv4() -> (UInt32, UInt32)? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        for pointer in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let entry = pointer.pointee
            guard String(cString: entry.ifa_name) == "en0",
                  let addr = entry.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
                  let netmask = entry.ifa_netmask else { continue }
            let ip = addr.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { UInt32(bigEndian: $0.pointee.sin_addr.s_addr) }
            let mask = netmask.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { UInt32(bigEndian: $0.pointee.sin_addr.s_addr) }
            return (ip, mask)
        }
        return nil
    }
}

/// Egyszeri jelző: a continuation pontosan egyszer folytatódjon
final class OnceFlag: @unchecked Sendable {
    private var done = false
    private let lock = NSLock()

    func claim() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if done { return false }
        done = true
        return true
    }
}
