import Foundation

/// Saved preferences, shared by the app, the command line and scheduled runs.
public struct Settings: Codable, Equatable {
    public static let defaultExcludes = ".DS_Store\nnode_modules\n.build\nDerivedData\n*.tmp"

    public var from = "", to = ""
    public var excludes = Settings.defaultExcludes
    public var full = false, hardLinks = true, preserve = true, versioned = true, openAfter = false
    public var remember = true, skipHidden = false, overwrite = false
    // Automation: schedule (0 off, 1 hourly, 2 daily, 3 weekly; day 0 = Sunday), retention (0 = no limit), checksums.
    public var schedMode = 2, schedHour = 2, schedMin = 0, schedDay = 1
    public var keepCount = 0, keepDays = 0
    public var checksums = true, verifyAfter = false

    public init() {}

    /// ~/Library/Application Support/EntropyBackup (ENTROPYBACKUP_HOME overrides it, for tests).
    public static var directory: String {
        if let h = ProcessInfo.processInfo.environment["ENTROPYBACKUP_HOME"], !h.isEmpty { return h }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?.path ?? NSHomeDirectory()
        return join(base, "EntropyBackup")
    }
    static var file: String { join(directory, "settings.json") }

    /// Missing keys keep their defaults, so older settings files still load.
    public static func load() -> Settings {
        var merged = (try? JSONSerialization.jsonObject(with: JSONEncoder().encode(Settings()))) as? [String: Any] ?? [:]
        if let d = FileManager.default.contents(atPath: file), let saved = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] {
            merged.merge(saved) { $1 }
        }
        guard let data = try? JSONSerialization.data(withJSONObject: merged), let s = try? JSONDecoder().decode(Settings.self, from: data) else { return Settings() }
        return s
    }
    public func save() {
        var s = self
        if !remember { s.from = ""; s.to = "" }
        try? FileManager.default.createDirectory(atPath: Settings.directory, withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let d = try? enc.encode(s) { FileManager.default.createFile(atPath: Settings.file, contents: d) }
    }
}

/// backup.log: one line per finished job, so scheduled and scripted runs leave a trail.
public enum Log {
    public static var path: String?  // --log overrides the default
    static var file: String { path ?? join(Settings.directory, "backup.log") }

    public static func event(_ line: String) {
        try? FileManager.default.createDirectory(atPath: (file as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        let stamp = SnapshotID.isoFormat.string(from: Date()).replacingOccurrences(of: "T", with: " ")
        let data = Data((stamp + "  " + line + "\n").utf8)
        if let h = FileHandle(forWritingAtPath: file) {
            h.seekToEndOfFile()
            h.write(data)
            h.closeFile()
        } else {
            FileManager.default.createFile(atPath: file, contents: data)
        }
    }
    /// The most recent line containing `text`, if any.
    public static func last(containing text: String) -> String? {
        guard let d = FileManager.default.contents(atPath: file), let s = String(data: d, encoding: .utf8) else { return nil }
        return s.split(whereSeparator: \.isNewline).last { $0.contains(text) }.map(String.init)
    }
}
