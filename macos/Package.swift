// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "EntropyBackup",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "EntropyBackup", targets: ["EntropyBackup"])],
    targets: [
        // Snapshot engine, scheduling and command line; no UI.
        .target(name: "EntropyCore"),
        // The app. Started with a command (backup, verify, ...) it runs without a window.
        .executableTarget(name: "EntropyBackup", dependencies: ["EntropyCore"]),
        .testTarget(name: "EntropyCoreTests", dependencies: ["EntropyCore"]),
    ]
)
