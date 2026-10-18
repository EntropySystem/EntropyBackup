import Foundation

/// Scheduled backups through a per-user launchd agent, so they also run while the app is closed.
/// launchd runs a job that was missed while the Mac slept as soon as it wakes.
public enum Schedule {
    public static let label = "com.entropysystem.entropybackup.scheduled"
    static var plist: String { join(NSHomeDirectory(), "Library/LaunchAgents/\(label).plist") }
    static let dayNames = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]

    public struct Status {
        public var exists = false
        public var summary = "No backup is scheduled."
        public var next: Date?
        public var last = ""
        public init() {}
    }

    public static func describe(mode: Int, hour: Int, minute: Int, day: Int) -> String {
        let t = String(format: "%02ld:%02ld", hour, minute)
        switch mode {
        case 1: return "Every hour at :" + String(format: "%02ld", minute)
        case 3: return "Every \(dayNames[max(0, min(6, day))]) at \(t)"
        default: return "Every day at \(t)"
        }
    }

    /// Installs, updates or removes the agent to match `s`. Returns a message for the page.
    public static func apply(_ s: Settings, executable: String) -> String {
        let domain = "gui/\(getuid())"
        launchctl(["bootout", "\(domain)/\(label)"])
        if s.schedMode == 0 {
            try? FileManager.default.removeItem(atPath: plist)
            return "Scheduled backups are off."
        }
        guard !s.from.isEmpty, !s.to.isEmpty else { return "Set From and To on the Backup page first." }
        var when: [String: Int] = ["Minute": s.schedMin]
        if s.schedMode >= 2 { when["Hour"] = s.schedHour }
        if s.schedMode == 3 { when["Weekday"] = s.schedDay }  // 0 = Sunday, as in launchd
        let fire = s.fireOn && !s.fire.isEmpty ? normPath(s.fire) : ""
        if !fire.isEmpty, let why = Engine.fireboxConflict(s.to, fire) { return why }
        let job: [String: Any] = [
            "Label": label,
            "ProgramArguments": [executable, "backup", "--scheduled", "--from", normPath(s.from), "--to", normPath(s.to)]
                + (fire.isEmpty ? ["--no-firebox"] : ["--firebox", fire]),
            "StartCalendarInterval": when,
            "StandardOutPath": join(Settings.directory, "scheduled.log"),
            "StandardErrorPath": join(Settings.directory, "scheduled.log"),
            "Nice": 5,
        ]
        do {
            try FileManager.default.createDirectory(atPath: (plist as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(atPath: Settings.directory, withIntermediateDirectories: true)
            let data = try PropertyListSerialization.data(fromPropertyList: job, format: .xml, options: 0)
            try data.write(to: URL(fileURLWithPath: plist))
        } catch {
            return "Could not save the schedule: \(error.localizedDescription)"
        }
        let rc = launchctl(["bootstrap", domain, plist])
        return rc == 0 ? "Schedule saved." : "The schedule was saved but launchd did not load it (launchctl exit \(rc))."
    }

    public static func status() -> Status {
        var st = Status()
        guard let d = NSDictionary(contentsOfFile: plist) as? [String: Any], let when = d["StartCalendarInterval"] as? [String: Int] else { return st }
        let args = d["ProgramArguments"] as? [String] ?? []
        func arg(_ k: String) -> String { args.firstIndex(of: k).flatMap { $0 + 1 < args.count ? args[$0 + 1] : nil } ?? "" }
        let mode = when["Weekday"] != nil ? 3 : when["Hour"] != nil ? 2 : 1
        st.exists = true
        st.summary = describe(mode: mode, hour: when["Hour"] ?? 0, minute: when["Minute"] ?? 0, day: when["Weekday"] ?? 0) + ":  \(arg("--from"))  →  \(arg("--to"))"
            + (arg("--firebox").isEmpty ? "" : "  +  Firebox \(arg("--firebox"))")
        var c = DateComponents()
        c.minute = when["Minute"] ?? 0
        if mode >= 2 { c.hour = when["Hour"] ?? 0 }
        if mode == 3 { c.weekday = (when["Weekday"] ?? 0) % 7 + 1 }  // Calendar: 1 = Sunday
        st.next = Calendar.current.nextDate(after: Date(), matching: c, matchingPolicy: .nextTime)
        if let line = Log.last(containing: "| Scheduled backup ") ?? Log.last(containing: "Scheduled backup") {
            let parts = line.components(separatedBy: " | ")
            st.last = String(line.prefix(16)) + (parts.count > 1 ? " (\(parts[1].lowercased()))" : "")
        }
        return st
    }

    @discardableResult
    static func launchctl(_ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }
}
