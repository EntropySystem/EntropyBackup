import Foundation

/// One snapshot (or the mirror) in a versions folder, as read from its backup.json and verify.json.
public struct Snapshot: Identifiable, Hashable {
    public var dir = "", sid = "", date = "", time = "", desc = "", source = "", type = "", created = ""
    public var files = 0, copied = 0, reused = 0, failed = 0
    public var bytes: Int64 = 0, bytesTotal: Int64 = 0
    public var seconds = 0.0
    public var mirror = false, sums = false, verified = false
    public var vChecked = 0, vBad = 0, vWhen = ""
    public var id: String { dir }
    public var name: String { (dir as NSString).lastPathComponent }

    /// Newest first. Snapshots made by the Windows app are listed too.
    public static func list(_ to: String) -> [Snapshot] {
        guard !to.isEmpty else { return [] }
        let root = Engine.versionsRoot(to)
        var out: [Snapshot] = []
        for n in names(root) where isDir(join(root, n)) {
            let isSnap = n.hasPrefix("snapshot_"), isMirror = n == "mirror_latest"
            guard isSnap || isMirror else { continue }
            var s = Snapshot()
            s.dir = join(root, n)
            s.mirror = isMirror
            let j = readJSON(join(s.dir, "backup.json"))
            func int(_ k: String) -> Int { (j[k] as? NSNumber)?.intValue ?? 0 }
            s.desc = j["description"] as? String ?? ""
            s.source = j["source"] as? String ?? ""
            s.type = j["type"] as? String ?? ""
            s.created = j["created"] as? String ?? ""
            s.files = int("files"); s.copied = int("copied"); s.reused = int("reused"); s.failed = int("failed")
            s.bytes = (j["bytes_written"] as? NSNumber)?.int64Value ?? 0
            s.bytesTotal = (j["bytes_total"] as? NSNumber)?.int64Value ?? 0
            s.seconds = (j["duration_seconds"] as? NSNumber)?.doubleValue ?? 0
            s.sums = j["checksums"] as? Bool ?? false
            let iso = (j["created_iso"] as? String ?? "").replacingOccurrences(of: "T", with: "_").replacingOccurrences(of: ":", with: "-")
            s.sid = isSnap ? String(n.dropFirst(9)) : (iso.isEmpty ? "0000-00-00_00-00-00" : iso)
            (s.date, s.time) = SnapshotID.display(s.sid)
            let v = readJSON(join(s.dir, "verify.json"))
            if !v.isEmpty {
                s.verified = true
                s.vChecked = (v["checked"] as? NSNumber)?.intValue ?? 0
                s.vBad = ((v["mismatched"] as? NSNumber)?.intValue ?? 0) + ((v["missing"] as? NSNumber)?.intValue ?? 0)
                let when = SnapshotID.display((v["verified_iso"] as? String ?? "").replacingOccurrences(of: "T", with: "_").replacingOccurrences(of: ":", with: "-"))
                s.vWhen = when.date + " " + when.time
            }
            out.append(s)
        }
        return out.sorted { $0.sid > $1.sid }
    }
}
