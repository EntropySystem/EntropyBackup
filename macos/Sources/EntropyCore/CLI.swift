import Darwin
import Foundation

/// Command line: scripted and scheduled runs without a window. The app's own executable takes the same
/// commands (EntropyBackup.app/Contents/MacOS/EntropyBackup backup ...), which is what the schedule runs.
public enum CLI {
    public static let usage = """
        Entropy Backup \(appVersion) - incremental, versioned project backups

        Usage: EntropyBackup <command> [options]

        Commands
          backup          Create a snapshot of --from inside --to
          verify [ID]     Check a snapshot against its SHA-256 checksums (default: the newest)
          prune           Apply the retention rules (--keep / --keep-days) now
          list            List the snapshots in --to
          help            Show this text

        Options (anything not given comes from the app's saved settings)
          --from DIR      Project folder to back up
          --to DIR        Backup folder; snapshots go in DIR/EntropyBackup_Versions
          --desc TEXT     Description stored with the snapshot
          --full          Full copy instead of incremental      (--incremental to force)
          --mirror        Keep one mirror_latest instead of versioned snapshots
          --no-links      Copy unchanged files instead of hard-linking them
          --exclude PAT   Add an exclude pattern (repeatable)
          --checksums     Record SHA-256 checksums                                  (--no-checksums)
          --verify        Verify the new snapshot after the backup; implies --checksums (--no-verify)
          --keep N        Keep only the newest N snapshots (0 = all)
          --keep-days N   Delete snapshots older than N days (0 = never)
          --log FILE      Append the result to FILE instead of the app's backup.log
          --quiet         Print nothing; check the exit code

        Exit codes: 0 success, 1 success with warnings, 2 failed, 3 invalid arguments, 4 cancelled

        """
    static var quiet = false
    static var current: Job?  // for Ctrl+C

    public static func isCommand(_ s: String) -> Bool {
        ["backup", "verify", "prune", "list", "help", "h", "?"].contains(s.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "-/")))
    }
    static func out(_ s: String) {
        guard !quiet else { return }
        FileHandle.standardOutput.write(Data(s.utf8))
    }
    static func pad(_ s: String, _ n: Int) -> String { s.count >= n ? String(s.prefix(n - 2)) + "… " : s + String(repeating: " ", count: n - s.count) }

    /// Runs `work` on a worker thread, drawing a one-line progress meter on a terminal.
    static func wait(_ job: Job, _ work: @escaping () -> Void) -> Int32 {
        current = job
        signal(SIGINT) { _ in CLI.current?.cancel() }
        let finished = DispatchSemaphore(value: 0)
        Thread { work(); finished.signal() }.start()
        let tty = isatty(STDOUT_FILENO) != 0 && !quiet
        while finished.wait(timeout: .now() + 0.2) == .timedOut {
            guard tty else { continue }
            let s = job.state
            out(String(format: "\r[%3ld%%] ", Int(s.fraction * 100)) + pad("\(Format.num(s.scanned)) files  \(Format.bytes(s.bytesWritten))  \(s.current)", 70))
        }
        if tty { out("\r" + String(repeating: " ", count: 78) + "\r") }
        out(job.state.message + "\n")
        return job.exitCode
    }

    public static func run(_ a: [String]) -> Int32 {
        var cmd = (a.first ?? "help").lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "-/"))
        let s = Settings.load()
        var o = BackupOptions(s)
        var scheduled = false, snapArg = ""
        func bad(_ m: String) -> Int32 { out("Error: \(m)\nRun with help for usage.\n"); return 3 }
        var i = 1
        while i < a.count {
            let k = a[i].lowercased(), hasValue = i + 1 < a.count
            func value() -> String { i += 1; return a[i] }
            func number() -> Int? { let v = Int(value()); return v.flatMap { $0 >= 0 ? $0 : nil } }
            switch k {
            case "--from" where hasValue: o.src = value()
            case "--to" where hasValue: o.dst = value()
            case "--desc" where hasValue, "--description" where hasValue: o.desc = value()
            case "--exclude" where hasValue: o.patterns += Excludes.patterns(value())
            case "--log" where hasValue: Log.path = normPath(value())
            case "--keep" where hasValue: guard let n = number() else { return bad("--keep needs a number.") }; o.keep = n
            case "--keep-days" where hasValue: guard let n = number() else { return bad("--keep-days needs a number.") }; o.keepDays = n
            case "--full": o.full = true
            case "--incremental": o.full = false
            case "--mirror": o.versioned = false
            case "--no-links": o.hardLinks = false
            case "--checksums": o.checksums = true
            case "--no-checksums": o.checksums = false; o.verify = false
            case "--verify": o.verify = true; o.checksums = true
            case "--no-verify": o.verify = false
            case "--quiet", "-q": quiet = true
            case "--scheduled": scheduled = true
            case "--help", "-h": cmd = "help"
            default:
                if cmd == "verify" && snapArg.isEmpty && !k.hasPrefix("-") { snapArg = a[i] } else { return bad("unknown or incomplete option \(a[i])") }
            }
            i += 1
        }
        if ["help", "h", "?"].contains(cmd) { out(usage); return 0 }
        guard ["backup", "verify", "prune", "list"].contains(cmd) else { return bad("unknown command \(a[0])") }
        guard !o.dst.isEmpty else { return bad("--to is required.") }
        let root = Engine.versionsRoot(o.dst)

        switch cmd {
        case "backup":
            guard !o.src.isEmpty else { return bad("--from is required.") }
            if o.desc.isEmpty && scheduled { o.desc = "Scheduled backup" }
            let job = Job(.backup, title: (scheduled ? "Scheduled backup " : "Backup ") + normPath(o.src) + " -> " + normPath(o.dst))
            out("Backing up \(normPath(o.src)) to \(root)\n")
            let opts = o
            return wait(job) { Engine.backup(opts, job: job) }
        case "prune":
            guard o.keep > 0 || o.keepDays > 0 else { return bad("give --keep or --keep-days (or set retention in the app).") }
            do {
                let n = try Engine.pruneLocked(o.dst, keep: o.keep, keepDays: o.keepDays)
                let msg = "Removed \(Format.num(n)) old snapshot\(n == 1 ? "" : "s")."
                Log.event("Prune \(root) | OK | \(msg)")
                out(msg + "\n")
                return 0
            } catch Stop.fail(let m) {
                Log.event("Prune \(root) | FAILED | \(m)")
                out("Error: \(m)\n")
                return 2
            } catch {
                return 2
            }
        case "list":
            let snaps = Snapshot.list(o.dst)
            if snaps.isEmpty { out("No snapshots in \(root)\n"); return 0 }
            out(pad("ID", 21) + pad("TYPE", 13) + pad("FILES", 10) + pad("SIZE", 11) + pad("CHECKSUMS", 12) + "DESCRIPTION\n")
            for s in snaps {
                let sums = !s.sums ? "-" : !s.verified ? "recorded" : s.vBad > 0 ? "PROBLEMS" : "verified"
                out(pad(s.mirror ? "mirror_latest" : s.sid, 21) + pad(s.type.isEmpty ? "-" : s.type, 13) + pad(Format.num(s.files), 10)
                    + pad(Format.bytes(s.bytesTotal), 11) + pad(sums, 12) + s.desc + "\n")
            }
            return 0
        default:  // verify: the newest, or the one named by ID, folder name or path
            let key = snapArg.lowercased()
            guard let snap = Snapshot.list(o.dst).first(where: { key.isEmpty || $0.sid.lowercased() == key || $0.name.lowercased() == key || $0.dir == normPath(snapArg) }) else {
                out("Error: no snapshot \(snapArg.isEmpty ? "found in \(root)" : snapArg)\n")
                return 2
            }
            let job = Job(.verify, title: "Verify " + snap.dir)
            out("Verifying \(snap.dir)\n")
            var result = VerifyResult()
            let rc = wait(job) { result = Engine.verify(snap.dir, job: job) }
            for p in result.problems { out("  \(p)\n") }
            return rc
        }
    }
}
