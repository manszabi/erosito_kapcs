// Felület: be / ki, trigger figyelés, állapot; beállítások (keresés, kézi cím, jelszó)
import SwiftUI

struct ContentView: View {
    @Environment(ErositoClient.self) private var client
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSettings = false

    private var isOn: Bool { client.status?.isOn ?? false }

    var body: some View {
        NavigationStack {
            List {
                if let error = client.lastError {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.red)
                    }
                }
                Section("Erősítő") {
                    HStack {
                        Image(systemName: "power.circle.fill")
                            .font(.largeTitle)
                            .foregroundStyle(isOn ? .green : .secondary)
                        Text(client.status == nil ? "Nincs kapcsolat" : (isOn ? "Bekapcsolva" : "Kikapcsolva"))
                            .font(.title2.bold())
                        Spacer()
                        if client.busy { ProgressView() }
                    }
                    HStack {
                        Button { Task { await client.setPower(true) } } label: {
                            Label("Be", systemImage: "power").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent).tint(.green)
                        Button { Task { await client.setPower(false) } } label: {
                            Label("Ki", systemImage: "power").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent).tint(.red)
                    }
                    .controlSize(.large)
                    .disabled(client.status == nil || client.busy)
                    Toggle("Trigger figyelés", isOn: Binding(
                        get: { client.status?.triggerEnabled ?? false },
                        set: { newValue in Task { await client.setTrigger(newValue) } }))
                    .disabled(client.status == nil || client.busy)
                }
                if let s = client.status {
                    Section("Állapot") {
                        row("Trigger jel", s.trigger == "ON" ? "aktív" : "nincs")
                        row("NTC áthidalva (RL2)", s.rl2 == "ON" ? "igen" : "nem")
                        if s.hold == true { row("Áramszünet utáni várakozás", "folyamatban") }
                        row("Hiba", s.fault ?? "-", alert: (s.fault ?? "nincs") != "nincs")
                        row("Figyelmeztetés", s.warning ?? "-", alert: (s.warning ?? "nincs") != "nincs")
                        row("Üzemidő", s.onHours.map { String(format: "%.1f óra", $0) } ?? "-")
                        row("Bekapcsolások", s.onCount.map(String.init) ?? "-")
                        row("Hálózatkiesések", s.mainsLoss.map(String.init) ?? "-")
                        row("WiFi jel", s.rssi.map { "\($0) dBm" } ?? "-")
                        row("IP-cím", s.ip ?? client.host)
                        row("Alaplap", s.board ?? "-")
                    }
                }
            }
            .navigationTitle("Erősítő")
            .toolbar {
                Button { showSettings = true } label: { Image(systemName: "gearshape") }
            }
            .sheet(isPresented: $showSettings) { SettingsView() }
            .refreshable { await client.refresh() }
        }
        .onChange(of: scenePhase, initial: true) { _, phase in
            if phase == .active { client.startPolling() } else { client.stopPolling() }
        }
        .onAppear { if client.host.isEmpty { showSettings = true } }
    }

    private func row(_ title: String, _ value: String, alert: Bool = false) -> some View {
        LabeledContent(title) {
            Text(value).foregroundStyle(alert ? .red : .secondary)
        }
    }
}

private func ipField(_ title: String, _ text: Binding<String>) -> some View {
    TextField(title, text: text)
        .keyboardType(.numbersAndPunctuation)
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
}

struct SettingsView: View {
    @Environment(ErositoClient.self) private var client
    @Environment(Discovery.self) private var discovery
    @Environment(\.dismiss) private var dismiss
    @State private var host = ""
    @State private var password = ""
    @State private var espIp = ""
    @State private var espGw = ""
    @State private var espMask = ""
    @State private var espDns = ""
    @State private var ipMessage: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    ForEach(discovery.devices) { device in
                        Button { host = device.host } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(device.name)
                                    Text(device.host).font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if host == device.host { Image(systemName: "checkmark") }
                            }
                        }
                    }
                    if discovery.devices.isEmpty {
                        Text(discovery.searching ? "Keresés…" : "Nem található eszköz")
                            .foregroundStyle(.secondary)
                    }
                    Button("Keresés újra") { discovery.start() }
                } header: {
                    Text("Talált eszközök")
                } footer: {
                    Text("A telefon és az erősítő ugyanazon a WiFi-hálózaton legyen.")
                }
                Section("Kézi megadás") {
                    TextField("IP-cím vagy erosito.local", text: $host)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                Section {
                    SecureField("Weboldal jelszó", text: $password)
                } header: {
                    Text("Jelszó")
                } footer: {
                    Text("Felhasználó: admin. Ugyanaz a jelszó, mint az erősítő weboldalán.")
                }
                Section {
                    ipField("Fix IP-cím (üresen: DHCP)", $espIp)
                    ipField("Átjáró (router)", $espGw)
                    ipField("Maszk (üresen: 255.255.255.0)", $espMask)
                    ipField("DNS (üresen: az átjáró)", $espDns)
                    Button("Mentés az erősítőn") {
                        Task {
                            ipMessage = await client.setNetworkIP(ip: espIp, gateway: espGw, mask: espMask, dns: espDns)
                            host = client.host
                        }
                    }
                    .disabled(client.status == nil || client.busy)
                    if let ipMessage { Text(ipMessage).font(.callout).foregroundStyle(.secondary) }
                } header: {
                    Text("Az erősítő IP-címe")
                } footer: {
                    Text("Jelenleg: \(client.status?.ipMode ?? "-"). Hibás beállításnál az erősítő DHCP-t használ. Az erősítő a mentés után pár másodpercre lecsatlakozik; a relék nem kapcsolnak.")
                }
            }
            .navigationTitle("Eszköz")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Mégse") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Mentés") {
                        client.configure(host: host.trimmingCharacters(in: .whitespaces), password: password)
                        dismiss()
                    }
                    .disabled(host.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            .onAppear {
                host = client.host
                password = client.password
                espIp = client.status?.cfgIp ?? ""
                espGw = client.status?.cfgGw ?? ""
                espMask = client.status?.cfgMask ?? ""
                espDns = client.status?.cfgDns ?? ""
                discovery.start()
            }
            .onDisappear { discovery.stop() }
        }
    }
}
