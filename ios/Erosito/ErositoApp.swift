// Erősítő bekapcsoló – iOS vezérlő: belépési pont
import SwiftUI

@main
struct ErositoApp: App {
    @State private var client = ErositoClient()
    @State private var discovery = Discovery()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(client)
                .environment(discovery)
        }
    }
}
