import EntropyCore
import SwiftUI

struct EntropyBackupApp: App {
    @StateObject private var model = AppModel()
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        WindowGroup("Entropy Backup") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 1080, minHeight: 720)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
                    if !model.running { model.refresh(); model.schedule = Schedule.status() }  // pick up scheduled runs
                }
        }
        .windowStyle(.hiddenTitleBar)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
