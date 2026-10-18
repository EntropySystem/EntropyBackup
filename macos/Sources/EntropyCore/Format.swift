import Foundation

public let appVersion = "1.6.1"

/// Number, size and time formatting shared by the engine, the command line and the app.
public enum Format {
    public static func num<T: BinaryInteger>(_ v: T) -> String {
        var out = ""
        for (i, c) in String(v).reversed().enumerated() {
            if i > 0 && i % 3 == 0 { out.append(",") }
            out.append(c)
        }
        return String(out.reversed())
    }
    public static func bytes(_ b: Int64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var d = Double(b), i = 0
        while d >= 1024 && i < 4 { d /= 1024; i += 1 }
        return String(format: i == 0 ? "%.0f %@" : "%.1f %@", d, units[i])
    }
    public static func duration(_ s: Double) -> String {
        let t = Int(s.rounded())
        return String(format: "%02ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60)
    }
    /// Escapes a string for the hand-written JSON files, which keep the Windows app's exact layout.
    static func json(_ s: String) -> String {
        var o = ""
        for u in s.unicodeScalars {
            switch u {
            case "\\": o += "\\\\"
            case "\"": o += "\\\""
            case "\n": o += "\\n"
            case "\r": o += "\\r"
            case "\t": o += "\\t"
            default:
                if u.value < 0x20 { o += String(format: "\\u%04x", u.value) } else { o.unicodeScalars.append(u) }
            }
        }
        return o
    }
}

/// Snapshot IDs are local times written as yyyy-MM-dd_HH-mm-ss, exactly like the Windows app.
public enum SnapshotID {
    private static func formatter(_ f: String) -> DateFormatter {
        let d = DateFormatter()
        d.locale = Locale(identifier: "en_US_POSIX")
        d.dateFormat = f
        return d
    }
    private static let idFormat = formatter("yyyy-MM-dd_HH-mm-ss")
    private static let dayFormat = formatter("MMM d, yyyy")
    private static let timeFormat = formatter("h:mm a")
    static let isoFormat = formatter("yyyy-MM-dd'T'HH:mm:ss")

    public static func make(_ d: Date = Date()) -> String { idFormat.string(from: d) }
    public static func date(_ id: String) -> Date? { idFormat.date(from: id) }
    public static func display(_ id: String) -> (date: String, time: String) {
        guard let d = date(id) else { return (id, "") }
        return (dayFormat.string(from: d), timeFormat.string(from: d))
    }
    public static func display(_ d: Date) -> String { dayFormat.string(from: d) + " " + timeFormat.string(from: d) }
}

/// Exclude patterns: one per line, `#` comments. A plain name matches at any depth; a pattern
/// containing `/` matches the path relative to the project. `*` and `?` are wildcards; case is ignored.
public enum Excludes {
    public static func patterns(_ text: String) -> [String] {
        text.split(whereSeparator: \.isNewline).compactMap { line in
            var l = line.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "\\", with: "/")
            while l.hasSuffix("/") { l.removeLast() }
            return l.isEmpty || l.hasPrefix("#") ? nil : l
        }
    }
    static func excluded(_ name: String, _ rel: String, _ patterns: [String]) -> Bool {
        patterns.contains { $0.contains("/") ? match($0, rel) : match($0, name) }
    }
    public static func match(_ pattern: String, _ text: String) -> Bool {
        let p = Array(key(pattern)), s = Array(key(text))
        var pi = 0, si = 0, star = -1, mark = 0
        while si < s.count {
            if pi < p.count && (p[pi] == "?" || (p[pi] != "*" && p[pi] == s[si])) { pi += 1; si += 1 }
            else if pi < p.count && p[pi] == "*" { star = pi; pi += 1; mark = si }
            else if star >= 0 { pi = star + 1; mark += 1; si = mark }
            else { return false }
        }
        while pi < p.count && p[pi] == "*" { pi += 1 }
        return pi == p.count
    }
    /// Case- and normalization-insensitive form: macOS volumes are usually case-insensitive and store decomposed names.
    static func key(_ s: String) -> String { s.precomposedStringWithCanonicalMapping.lowercased() }
}
