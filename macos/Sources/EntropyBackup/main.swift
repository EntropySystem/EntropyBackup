import EntropyCore
import Foundation

// With a command (as the scheduled job and scripts use) the app runs headless and exits with the result.
let arguments = Array(CommandLine.arguments.dropFirst())
if let first = arguments.first, CLI.isCommand(first) {
    exit(CLI.run(arguments))
}
MainActor.assumeIsolated { EntropyBackupApp.main() }  // top-level code runs on the main thread
