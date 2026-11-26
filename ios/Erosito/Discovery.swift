// Keresés a helyi hálózaton: Bonjour "_erosito._tcp" (az ESP mDNS-sel hirdeti), majd IPv4-címre feloldás
import Foundation
import Network
import Observation

struct FoundDevice: Identifiable, Hashable, Sendable {
    let id: String     // Bonjour szolgáltatásnév
    let name: String
    let host: String   // IPv4-cím (és port, ha nem 80)
}

@MainActor
@Observable
final class Discovery {
    private(set) var devices: [FoundDevice] = []
    private(set) var searching = false
    private var browser: NWBrowser?
    private var stopTask: Task<Void, Never>?

    func start() {
        stop()
        devices = []
        searching = true
        let newBrowser = NWBrowser(for: .bonjour(type: "_erosito._tcp", domain: nil), using: .tcp)
        newBrowser.browseResultsChangedHandler = { [weak self] results, _ in
            let endpoints = results.map(\.endpoint)
            Task { @MainActor in await self?.resolveAll(endpoints) }
        }
        newBrowser.stateUpdateHandler = { [weak self] state in
            if case .failed = state {
                Task { @MainActor in self?.searching = false }
            }
        }
        newBrowser.start(queue: .main)
        browser = newBrowser
        stopTask = Task { [weak self] in   // a "Keresés…" felirat 10 s után eltűnik; a böngészés közben tovább fut
            try? await Task.sleep(for: .seconds(10))
            self?.searching = false
        }
    }

    func stop() {
        browser?.cancel()
        browser = nil
        stopTask?.cancel()
        searching = false
    }

    private func resolveAll(_ endpoints: [NWEndpoint]) async {
        for endpoint in endpoints {
            guard case let .service(name, _, _, _) = endpoint else { continue }
            if devices.contains(where: { $0.id == name }) { continue }
            if let host = await Self.resolve(endpoint) {
                devices.append(FoundDevice(id: name, name: name, host: host))
            }
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
