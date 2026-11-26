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

    var isOn: Bool { power == "ON" }
    var triggerEnabled: Bool { trigEnabled ?? false }
}

enum ClientError: LocalizedError {
    case noHost, unauthorized, badResponse(Int), unreachable(String)

    var errorDescription: String? {
        switch self {
        case .noHost: "Nincs kiválasztott eszköz (fogaskerék ikon)."
        case .unauthorized: "Hibás jelszó."
        case .badResponse(let code): "Váratlan válasz az eszköztől (HTTP \(code))."
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
            status = try decoder.decode(AmpStatus.self, from: data)
            lastError = nil
        } catch {
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
