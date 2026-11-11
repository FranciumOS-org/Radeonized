import SwiftUI

@main
struct RadeonizeApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("Radeonize") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 620, minHeight: 760)
                .task { await model.refresh() }
        }
        .windowResizability(.contentMinSize)
    }
}
