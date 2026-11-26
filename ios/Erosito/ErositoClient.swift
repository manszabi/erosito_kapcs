// Kapcsolat az ESP weboldalával: /api/status, /api/power, /api/trigger (Basic jelszó: admin / weboldal jelszó)
import Foundation
import Observation

/// Az ESP /api/status válasza (minden mező opcionális, így egy régebbi firmware sem okoz hibát)
struct AmpStatus: Decodable, Sendable {
    var power: String?
    var rl2: String?
    var trigger: String?
    var trigEnabled: Bool?
    var hold: Bool?
    var fault: String?
    var warning: String?
    var onHours: Double?
    var onCount: Int?
    var mainsLoss: Int?
    var faults: Int?
    var optoNow: Int?
    var rssi: Int?
    var wifi: String?
    var ip: String?
    var mqtt: String?
    var board: String?
    var ipMode: String?
    var ipError: String?
    var cfgIp: String?
    var cfgGw: String?
    var cfgMask: String?
    var cfgDns: String?

    var isOn: Bool { power == "ON" }
    var triggerEnabled: Bool { trigEnabled ?? false }
}

enum ClientError: LocalizedError {
    case noHost, unauthorized, badResponse(Int), unreachable(String)

    var errorDescription: String? {
        switch self {
        case .noHost: "Nincs kiválasztott eszköz (fogaskerék ikon)."
        case .unauthorized: "Hibás jelszó."
        case .badResponse(let code): code == 200 ? "Hibás állapotválasz az eszköztől." : "Váratlan válasz az eszköztől (HTTP \(code))."
        case .unreachable(let host): "Az eszköz nem érhető el: \(host)"
        }
    }
}

@MainActor
@Observable
final class ErositoClient {
    private(set) var host: String
    private(set) var password: String
    private(set) var status: AmpStatus?
    private(set) var lastError: String?
    private(set) var busy = false
    private var pollTask: Task<Void, Never>?
    private var quietUntil = Date.distantPast   // IP-váltás után az ESP pár másodpercig nem érhető el: ezt nem jelezzük hibaként

    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 4
        config.waitsForConnectivity = false
        return URLSession(configuration: config)
    }()

    init() {
        host = UserDefaults.standard.string(forKey: "host") ?? ""
        password = Keychain.load(account: "web") ?? ""
    }

    /// Eszköz és jelszó mentése (cím: UserDefaults, jelszó: Kulcskarika)
    func configure(host newHost: String, password newPassword: String) {
        host = newHost
        password = newPassword
        UserDefaults.standard.set(newHost, forKey: "host")
        Keychain.save(newPassword, account: "web")
        status = nil
        Task { await refresh() }
    }

    /// Állapot lekérdezése 2 s-onként, amíg az app előtérben van
    func startPolling() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refresh()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    func stopPolling() {
        pollTask?.cancel()
        pollTask = nil
    }

    func refresh() async {
        do {
            let data = try await request("/api/status", body: nil)
            let decoder = JSONDecoder()
            decoder.keyDecodingStrategy = .convertFromSnakeCase
            let decoded = try decoder.decode(AmpStatus.self, from: data)
            guard decoded.power != nil else { throw ClientError.badResponse(200) }   // üres / sérült válasz ne látsszon "Kikapcsolva"-nak
            status = decoded
            lastError = nil
        } catch {
            if Date() < quietUntil { return }
            lastError = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        }
    }

    /// Be / ki: ugyanazok a szabályok, mint a gombnál (minimális kikapcsolt idő, RL2 minimum, hibák)
    func setPower(_ on: Bool) async {
        await send("/api/power", body: on ? "state=on" : "state=off")
    }

    /// Trigger figyelés be / ki (az ESP elmenti)
    func setTrigger(_ enabled: Bool) async {
        await send("/api/trigger", body: enabled ? "enabled=1" : "enabled=0")
    }

    /// Az erősítő IP-címe: fix (ip + átjáró, maszk és DNS opcionális) vagy üres IP-vel DHCP.
    /// Hibás beállításnál az ESP DHCP-t használ. Siker esetén az app az új címre vált.
    func setNetworkIP(ip: String, gateway: String, mask: String, dns: String) async -> String {
        var form = URLComponents()
        form.queryItems = [URLQueryItem(name: "ip", value: ip), URLQueryItem(name: "gw", value: gateway),
                           URLQueryItem(name: "mask", value: mask), URLQueryItem(name: "dns", value: dns)]
        busy = true
        defer { busy = false }
        do {
            let data = try await request("/api/netip", body: form.percentEncodedQuery ?? "")
            struct Reply: Decodable { let ok: Bool; let mode: String; let ip: String; let error: String }
            let reply = try JSONDecoder().decode(Reply.self, from: data)
            quietUntil = Date().addingTimeInterval(15)   // az ESP ~1,5 s múlva bont, majd újracsatlakozik
            if reply.mode == "fix", !reply.ip.isEmpty {
                configure(host: reply.ip, password: password)
                return "Mentve: fix IP \(reply.ip). Az app erre a címre váltott."
            }
            if !reply.error.isEmpty { return "Hibás beállítás (\(reply.error)): az erősítő DHCP-t használ. Keresd meg újra." }
            return "Mentve: DHCP. Az új címet a keresés találja meg."
        } catch {
            return (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        }
    }

    private func send(_ path: String, body: String) async {
        busy = true
        defer { busy = false }
        do {
            _ = try await request(path, body: body)
            try? await Task.sleep(for: .milliseconds(300))
            await refresh()
        } catch {
            lastError = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        }
    }

    private func request(_ path: String, body: String?) async throws -> Data {
        guard !host.isEmpty, let url = URL(string: "http://\(host)\(path)") else { throw ClientError.noHost }
        var req = URLRequest(url: url)
        let token = Data("admin:\(password)".utf8).base64EncodedString()
        req.setValue("Basic \(token)", forHTTPHeaderField: "Authorization")
        if let body {
            req.httpMethod = "POST"
            req.httpBody = Data(body.utf8)
            req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        }
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: req)
        } catch {
            throw ClientError.unreachable(host)
        }
        guard let http = response as? HTTPURLResponse else { throw ClientError.badResponse(0) }
        if http.statusCode == 401 { throw ClientError.unauthorized }
        guard http.statusCode == 200 else { throw ClientError.badResponse(http.statusCode) }
        return data
    }
}
