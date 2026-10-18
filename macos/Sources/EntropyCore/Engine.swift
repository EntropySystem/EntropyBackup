import CryptoKit
import Darwin
import Foundation

enum Stop: Error {
    case fail(String)
    case cancelled
}

// ---------------------------------------------------------------------------------------------
// Paths and files
// ---------------------------------------------------------------------------------------------
func join(_ a: String, _ b: String) -> String { b.isEmpty ? a : a.hasSuffix("/") ? a + b : a + "/" + b }
/// Absolute, tilde-expanded, without quotes or a trailing slash.
public func normPath(_ p: String) -> String {
    var s = p.trimmingCharacters(in: CharacterSet(charactersIn: " \t\n\"'"))
    if s.isEmpty { return s }
    s = (s as NSString).expandingTildeInPath
    if !s.hasPrefix("/") { s = join(FileManager.default.currentDirectoryPath, s) }
    s = (s as NSString).standardizingPath
    while s.count > 1 && s.hasSuffix("/") { s.removeLast() }
    return s
}
func isDir(_ p: String) -> Bool {
    var d: ObjCBool = false
    return FileManager.default.fileExists(atPath: p, isDirectory: &d) && d.boolValue
}
func isUnder(_ child: String, _ parent: String) -> Bool {
    let c = Excludes.key(child), p = Excludes.key(parent)
    return c == p || c.hasPrefix(p.hasSuffix("/") ? p : p + "/")
}
func makeDirs(_ p: String) throws {
    do { try FileManager.default.createDirectory(atPath: p, withIntermediateDirectories: true) }
    catch { throw Stop.fail("Cannot create \(p): \(error.localizedDescription)") }
}
func names(_ dir: String) -> [String] { (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? [] }
func errText(_ e: Int32) -> String { String(cString: strerror(e)) }
func lstatInfo(_ p: String) -> stat? {
    var st = stat()
    return lstat(p, &st) == 0 ? st : nil
}
func isLink(_ st: stat) -> Bool { st.st_mode & S_IFMT == S_IFLNK }
/// Modification time in Windows FILETIME ticks (100 ns since 1601), so manifests match the Windows app's.
func fileTicks(_ st: stat) -> UInt64 {
    let t = Int64(st.st_mtimespec.tv_sec) * 10_000_000 + Int64(st.st_mtimespec.tv_nsec) / 100 + 116_444_736_000_000_000
    return UInt64(max(0, t))
}
func writeText(_ path: String, _ s: String) -> Bool { FileManager.default.createFile(atPath: path, contents: Data(s.utf8)) }
func readJSON(_ path: String) -> [String: Any] {
    guard let d = FileManager.default.contents(atPath: path) else { return [:] }
    return (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] ?? [:]
}
/// Deletes a tree. Folders are made writable first (they are never hard-linked, so this cannot affect other
/// snapshots); files are only unlinked, so the flags they share with other snapshots stay intact.
func removeTree(_ p: String) {
    guard let st = lstatInfo(p) else { return }
    if st.st_mode & S_IFMT == S_IFDIR {
        chmod(p, (st.st_mode & 0o7777) | S_IRWXU)
        for n in names(p) { removeTree(join(p, n)) }
        rmdir(p)
    } else {
        unlink(p)
    }
}

// Copies with clonefile on APFS (instant, no extra space) and falls back to a normal copy elsewhere.
private final class CopyContext {
    let job: Job, base: Int64
    init(_ job: Job, _ base: Int64) { self.job = job; self.base = base }
}
private let copyProgress: copyfile_callback_t = { what, stage, state, _, _, ctx in
    guard let ctx = ctx else { return Int32(COPYFILE_CONTINUE) }
    let c = Unmanaged<CopyContext>.fromOpaque(ctx).takeUnretainedValue()
    if what == Int32(COPYFILE_COPY_DATA) && stage == Int32(COPYFILE_PROGRESS), let state = state {
        var copied: off_t = 0
        if copyfile_state_get(state, UInt32(COPYFILE_STATE_COPIED), &copied) == 0 {
            c.job.update { $0.workDone = c.base + Int64(copied) }
        }
    }
    return c.job.isCancelled ? Int32(COPYFILE_QUIT) : Int32(COPYFILE_CONTINUE)
}
/// Returns 0 or an errno. Symbolic links are copied as links.
func copyFile(_ src: String, _ dst: String, preserve: Bool, job: Job, base: Int64) -> Int32 {
    guard let state = copyfile_state_alloc() else { return ENOMEM }
    defer { copyfile_state_free(state) }
    let ctx = CopyContext(job, base)
    copyfile_state_set(state, UInt32(COPYFILE_STATE_STATUS_CB), unsafeBitCast(copyProgress, to: UnsafeRawPointer.self))
    copyfile_state_set(state, UInt32(COPYFILE_STATE_STATUS_CTX), Unmanaged.passUnretained(ctx).toOpaque())
    let flags = preserve ? COPYFILE_ALL | COPYFILE_CLONE : COPYFILE_DATA
    let r = withExtendedLifetime(ctx) { copyfile(src, dst, state, copyfile_flags_t(flags | COPYFILE_NOFOLLOW_SRC | COPYFILE_EXCL)) }
    return r == 0 ? 0 : (errno == 0 ? EIO : errno)
}

/// Hex SHA-256 of a file (of the link text for a symbolic link), or "" if it cannot be read.
/// `fromDisk` bypasses the file cache so a verification reads what is really stored.
func sha256(_ path: String, fromDisk: Bool, job: Job, base: Int64) -> String {
    guard let st = lstatInfo(path) else { return "" }
    if isLink(st) {
        guard let target = try? FileManager.default.destinationOfSymbolicLink(atPath: path) else { return "" }
        return SHA256.hash(data: Data(target.utf8)).map { String(format: "%02x", $0) }.joined()
    }
    let fd = open(path, O_RDONLY)
    if fd < 0 { return "" }
    defer { close(fd) }
    if fromDisk { _ = fcntl(fd, F_NOCACHE, 1) }
    let size = 1 << 20
    let buf = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 4096)
    defer { buf.deallocate() }
    var h = SHA256()
    var done = base
    while true {
        if job.isCancelled { return "" }
        let n = read(fd, buf, size)
        if n < 0 { if errno == EINTR { continue }; return "" }
        if n == 0 { break }
        h.update(bufferPointer: UnsafeRawBufferPointer(start: buf, count: n))
        done += Int64(n)
        job.update { $0.workDone = done }
    }
    return h.finalize().map { String(format: "%02x", $0) }.joined()
}

/// Exclusive lock on a versions folder, so a scheduled run and the app never write the same destination at once.
final class VersionsLock {
    private let fd: Int32
    init(_ root: String) throws {
        let f = open(join(root, ".lock"), O_CREAT | O_RDWR, 0o644)
        guard f >= 0 else { throw Stop.fail("Cannot lock the backup folder: \(errText(errno))") }
        if flock(f, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK {  // volumes without locking support are used unlocked
            close(f)
            throw Stop.fail("Another backup or verification is already using this backup folder.")
        }
        fd = f
    }
    deinit {
        flock(fd, LOCK_UN)
        close(fd)
    }
}

// ---------------------------------------------------------------------------------------------
// Manifest: size<TAB>mtime<TAB>path (v1) or size<TAB>mtime<TAB>sha256<TAB>path (v2, "-" = no checksum)
// ---------------------------------------------------------------------------------------------
struct ManEntry {
    var rel: String, size: Int64, mtime: UInt64, sha: String
}
func loadManifest(_ path: String) -> [ManEntry]? {
    guard let d = FileManager.default.contents(atPath: path), let text = String(data: d, encoding: .utf8) else { return nil }
    var out: [ManEntry] = []
    for line in text.split(whereSeparator: \.isNewline) where !line.hasPrefix("#") {
        let f = line.split(separator: "\t", maxSplits: 3, omittingEmptySubsequences: false)  // names cannot contain tabs
        guard f.count >= 3, let last = f.last, !last.isEmpty else { continue }
        out.append(ManEntry(rel: String(last), size: Int64(f[0]) ?? 0, mtime: UInt64(f[1]) ?? 0, sha: f.count == 4 && f[2] != "-" ? String(f[2]) : ""))
    }
    return out
}

struct FileItem {
    var rel: String, size: Int64, mtime: UInt64
    var reuse = false, failed = false, sha = ""
}
struct ScanOptions {
    var patterns: [String] = [], skipHidden = false, skipDir = ""
}
func scan(_ root: String, _ rel: String, _ o: ScanOptions, _ job: Job, _ files: inout [FileItem], _ dirs: inout [String]) {
    var subs: [String] = []
    for n in names(rel.isEmpty ? root : join(root, rel)).sorted() {
        if job.isCancelled { return }
        let r = rel.isEmpty ? n : rel + "/" + n
        if o.skipHidden && n.hasPrefix(".") { continue }
        if Excludes.excluded(n, r, o.patterns) { continue }
        guard let st = lstatInfo(join(root, r)) else { continue }
        let type = st.st_mode & S_IFMT
        if type == S_IFDIR {  // symbolic links to folders are copied as links, never followed
            if !o.skipDir.isEmpty && Excludes.key(join(root, r)) == o.skipDir { continue }
            dirs.append(r)
            subs.append(r)
        } else if type == S_IFREG || type == S_IFLNK {
            files.append(FileItem(rel: r, size: Int64(st.st_size), mtime: fileTicks(st)))
            let count = files.count
            job.update { $0.scanned = count; if count % 128 == 0 { $0.current = r } }
        }
    }
    for s in subs {
        if job.isCancelled { return }
        scan(root, s, o, job, &files, &dirs)
    }
}
func findPrevious(_ root: String, versioned: Bool) -> String {
    let best = names(root).filter { $0.hasPrefix("snapshot_") && isDir(join(root, $0)) }.max() ?? ""
    let snap = best.isEmpty ? "" : join(root, best), mirror = join(root, "mirror_latest")
    if !versioned { return isDir(mirror) ? mirror : snap }
    return !snap.isEmpty ? snap : (isDir(mirror) ? mirror : "")
}
let fileCost: Int64 = 16384  // per-file overhead used to weight progress

// ---------------------------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------------------------
public struct BackupOptions {
    public var src, dst, desc: String
    public var full, hardLinks, preserve, versioned, skipHidden, checksums, verify: Bool
    public var patterns: [String]
    public var keep, keepDays: Int
    public init(_ s: Settings, desc: String = "") {
        src = s.from; dst = s.to; self.desc = desc
        full = s.full; hardLinks = s.hardLinks; preserve = s.preserve; versioned = s.versioned; skipHidden = s.skipHidden
        checksums = s.checksums; verify = s.verifyAfter && s.checksums
        patterns = Excludes.patterns(s.excludes)
        keep = s.keepCount; keepDays = s.keepDays
    }
}
public struct VerifyResult {
    public var checked = 0, ok = 0, bad = 0, missing = 0
    public var problems: [String] = []
    public init() {}
    public var summary: String {
        let n = bad + missing
        if n == 0 { return "Verified: all \(Format.num(checked)) files match their checksums." }
        return "Verification found \(Format.num(n)) problem\(n == 1 ? "" : "s") in \(Format.num(checked)) files (\(Format.num(bad)) changed, "
            + "\(Format.num(missing)) missing or unreadable). See verify.json in the snapshot."
    }
}

public enum Engine {
    public static func versionsRoot(_ to: String) -> String { join(normPath(to), "EntropyBackup_Versions") }

    public static func backup(_ o: BackupOptions, job: Job) {
        var work = ""
        do {
            let (msg, snapDir) = try runBackup(o, job, &work)
            job.finish(.done, msg, path: snapDir)
        } catch Stop.cancelled {
            if !work.isEmpty { removeTree(work) }
            job.finish(.cancelled, "Cancelled. No partial snapshot was kept.")
        } catch Stop.fail(let m) {
            if !work.isEmpty { removeTree(work) }
            job.finish(.failed, m)
        } catch {
            if !work.isEmpty { removeTree(work) }
            job.finish(.failed, error.localizedDescription)
        }
    }

    private static func runBackup(_ o: BackupOptions, _ job: Job, _ work: inout String) throws -> (String, String) {
        let src = normPath(o.src), dst = normPath(o.dst)
        guard isDir(src) else { throw Stop.fail("The source folder does not exist.") }
        let root = join(dst, "EntropyBackup_Versions")
        if Excludes.key(src) == Excludes.key(dst) { throw Stop.fail("Source and backup folders must be different.") }
        if isUnder(src, root) { throw Stop.fail("The source folder is inside the backup folder.") }
        try makeDirs(root)
        let lock = try VersionsLock(root)
        defer { withExtendedLifetime(lock) {} }
        // Leftovers of an interrupted run (power loss, killed process) are never valid snapshots.
        for n in names(root) where n.hasPrefix("incomplete_") { removeTree(join(root, n)) }

        let now = Date(), id = SnapshotID.make(now), prev = findPrevious(root, versioned: o.versioned)
        work = join(root, "incomplete_" + id)
        let filesDir = join(work, "files")
        try makeDirs(filesDir)

        // 1. Scan
        var opts = ScanOptions(patterns: o.patterns, skipHidden: o.skipHidden)
        if isUnder(root, src) { opts.skipDir = Excludes.key(root) }  // never back up our own versions folder
        var files: [FileItem] = [], dirs: [String] = []
        scan(src, "", opts, job, &files, &dirs)
        if job.isCancelled { throw Stop.cancelled }

        // 2. Classify unchanged files against the previous snapshot
        let incremental = !o.full && !prev.isEmpty
        let prevMan = incremental ? loadManifest(join(prev, "manifest.tsv")) : nil
        var man: [String: ManEntry] = [:]
        for e in prevMan ?? [] { man[Excludes.key(e.rel)] = e }
        var copyBytes: Int64 = 0, hashBytes: Int64 = 0, totalBytes: Int64 = 0
        for i in files.indices {
            var f = files[i]
            totalBytes += f.size
            var pe: ManEntry?
            if incremental {
                if prevMan != nil {
                    pe = man[Excludes.key(f.rel)]
                    if let e = pe { f.reuse = e.size == f.size && e.mtime == f.mtime }
                } else if let st = lstatInfo(join(join(prev, "files"), f.rel)) {  // snapshot without manifest
                    f.reuse = Int64(st.st_size) == f.size && fileTicks(st) == f.mtime
                }
            }
            if !f.reuse || !o.hardLinks { copyBytes += f.size }
            if o.checksums {  // an unchanged file keeps the checksum it was stored with
                if f.reuse, let sha = pe?.sha, !sha.isEmpty { f.sha = sha } else { hashBytes += f.size }
            }
            files[i] = f
        }
        let workTotal = copyBytes + hashBytes + Int64(files.count) * fileCost + 1
        job.update { $0.workTotal = workTotal; $0.phase = .copying }

        // 3. Copy / link
        for d in dirs { mkdir(join(filesDir, d), 0o755) }
        var base: Int64 = 0, failed = 0, linksWork = true
        var failedNames: [String] = []
        let prevFiles = prev.isEmpty ? "" : join(prev, "files")
        for i in files.indices {
            if job.isCancelled { throw Stop.cancelled }
            var f = files[i]
            job.update { $0.current = f.rel }
            let out = join(filesDir, f.rel), inp = join(src, f.rel)
            if o.checksums && f.sha.isEmpty {
                f.sha = sha256(inp, fromDisk: false, job: job, base: base)
                if job.isCancelled { throw Stop.cancelled }
                base += f.size
            }
            var done = false
            if f.reuse {
                let old = join(prevFiles, f.rel)
                if o.hardLinks && linksWork {
                    if linkat(AT_FDCWD, old, AT_FDCWD, out, 0) == 0 { done = true }
                    else { let e = errno; if e == ENOTSUP || e == EPERM || e == EOPNOTSUPP { linksWork = false } }
                } else if !o.hardLinks {  // unchanged: copy (clone) from the previous snapshot
                    done = copyFile(old, out, preserve: true, job: job, base: base) == 0
                    if done { let n = f.size; job.update { $0.bytesWritten += n } }
                }
                if done { job.update { $0.linked += 1 } }
            }
            if !done {
                let e = copyFile(inp, out, preserve: o.preserve, job: job, base: base)
                if e == 0 {
                    let n = f.size
                    job.update { $0.copied += 1; $0.bytesWritten += n }
                    done = true
                } else {
                    if e == ECANCELED || job.isCancelled { throw Stop.cancelled }
                    if e == ENOSPC { throw Stop.fail("The backup drive is full.") }
                    unlink(out)
                    f.failed = true
                    failed += 1
                    if failedNames.count < 200 { failedNames.append("\(f.rel) (\(errText(e)))") }
                }
            }
            base += (f.reuse && o.hardLinks ? 0 : f.size) + fileCost
            files[i] = f
            let b = base
            job.update { $0.workDone = b }
        }

        // 4. Finalize: folder metadata, manifest, backup.json, atomic rename
        job.update { $0.phase = .finalizing; $0.current = "" }
        if o.preserve {
            for d in dirs.reversed() { copyfile(join(src, d), join(filesDir, d), nil, copyfile_flags_t(COPYFILE_METADATA | COPYFILE_NOFOLLOW_SRC)) }
        }
        var manifest = o.checksums ? "# Entropy Backup manifest v2: size<TAB>mtime<TAB>sha256<TAB>path\n" : "# Entropy Backup manifest v1: size<TAB>mtime<TAB>path\n"
        for f in files where !f.failed {
            manifest += "\(f.size)\t\(f.mtime)\t" + (o.checksums ? (f.sha.isEmpty ? "-" : f.sha) + "\t" : "") + f.rel + "\n"
        }
        _ = writeText(join(work, "manifest.tsv"), manifest)

        let st = job.state
        let human = SnapshotID.display(now)
        var j = "{\n"
        j += "  \"version\": 3,\n"
        j += "  \"app\": \"Entropy Backup \(appVersion) (macOS)\",\n"
        j += "  \"created\": \"\(Format.json(human))\",\n"
        j += "  \"created_iso\": \"\(SnapshotID.isoFormat.string(from: now))\",\n"
        j += "  \"source\": \"\(Format.json(src))\",\n"
        j += "  \"description\": \"\(Format.json(o.desc))\",\n"
        j += "  \"type\": \"\(o.full ? "full" : "incremental")\",\n"
        j += "  \"hard_links\": \(o.hardLinks && !o.full),\n"
        j += "  \"shadow_copy\": false,\n"
        j += "  \"checksums\": \(o.checksums),\n"
        j += "  \"previous\": \"\(Format.json(prev.isEmpty ? "" : (prev as NSString).lastPathComponent))\",\n"
        j += "  \"files\": \(files.count - failed),\n"
        j += "  \"copied\": \(st.copied),\n"
        j += "  \"reused\": \(st.linked),\n"
        j += "  \"bytes_written\": \(st.bytesWritten),\n"
        j += "  \"bytes_total\": \(totalBytes),\n"
        j += "  \"duration_seconds\": \(String(format: "%.1f", Date().timeIntervalSince(st.started))),\n"
        j += "  \"excludes\": [" + o.patterns.map { "\"\(Format.json($0))\"" }.joined(separator: ", ") + "],\n"
        j += "  \"failed\": \(failed),\n"
        j += "  \"failed_files\": [" + (failedNames.isEmpty ? "]\n" : "\n    " + failedNames.map { "\"\(Format.json($0))\"" }.joined(separator: ",\n    ") + "\n  ]\n")
        j += "}\n"
        _ = writeText(join(work, "backup.json"), j)

        let snapDir = join(root, o.versioned ? "snapshot_" + id : "mirror_latest")
        if !o.versioned && isDir(snapDir) { removeTree(snapDir) }
        if rename(work, snapDir) != 0 { throw Stop.fail("Could not finalize the snapshot: \(errText(errno))") }
        work = ""

        var msg = "\(Format.num(files.count - failed)) files protected, \(Format.num(st.linked)) unchanged reused, \(Format.bytes(st.bytesWritten)) written."
        if failed > 0 {
            msg += " \(Format.num(failed)) unreadable files skipped."
            job.update { $0.warnings += failed }
        }
        // 5. Optional verification of what was written, then retention
        if o.verify && o.checksums {
            do {
                var v = VerifyResult()
                _ = try verifySnapshot(snapDir, job, &v)
                msg += " " + v.summary
                let n = v.bad + v.missing
                job.update { $0.warnings += n }
            } catch {
                msg += " Verification was cancelled; the snapshot was kept."
                job.update { $0.warnings += 1 }
            }
        }
        if o.versioned {
            let n = prune(root, keep: o.keep, keepDays: o.keepDays, job: job)
            if n > 0 { msg += " Removed \(Format.num(n)) old snapshot\(n == 1 ? "" : "s")." }
        }
        return (msg, snapDir)
    }

    /// Retention: removes versioned snapshots beyond the newest `keep`, or older than `keepDays` (0 = no limit).
    /// The newest snapshot always stays. Unchanged files are hard links, so removing one never affects the others.
    @discardableResult
    public static func prune(_ root: String, keep: Int, keepDays: Int, job: Job? = nil) -> Int {
        if keep <= 0 && keepDays <= 0 { return 0 }
        let snaps = names(root).filter { $0.hasPrefix("snapshot_") && isDir(join(root, $0)) }.sorted(by: >)  // newest first
        var removed = 0
        for (i, n) in snaps.enumerated() where i > 0 {
            var drop = keep > 0 && i >= keep
            if !drop && keepDays > 0, let d = SnapshotID.date(String(n.dropFirst(9))) {
                drop = Date().timeIntervalSince(d) > Double(keepDays) * 86400
            }
            guard drop else { continue }
            job?.update { $0.current = "Removing old snapshot " + n.dropFirst(9) }
            removeTree(join(root, n))
            if !isDir(join(root, n)) { removed += 1 }
        }
        return removed
    }
    public static func pruneLocked(_ to: String, keep: Int, keepDays: Int) throws -> Int {
        let root = versionsRoot(to)
        let lock = try VersionsLock(root)
        defer { withExtendedLifetime(lock) {} }
        return prune(root, keep: keep, keepDays: keepDays)
    }

    /// Re-reads a snapshot from disk and compares every file with the SHA-256 recorded when it was backed up.
    /// The outcome is saved as verify.json. Returns false when the snapshot has no manifest.
    static func verifySnapshot(_ snap: String, _ job: Job, _ v: inout VerifyResult) throws -> Bool {
        guard let man = loadManifest(join(snap, "manifest.tsv")) else { return false }
        let own = job.state.kind == .verify  // a standalone verification drives the counters
        var base = job.state.workDone
        let total = man.filter { !$0.sha.isEmpty }.reduce(Int64(0)) { $0 + $1.size + fileCost }
        let start = base
        job.update { $0.workTotal = start + total + 1; if own { $0.phase = .copying } }
        let files = join(snap, "files")
        for e in man where !e.sha.isEmpty {
            if job.isCancelled { throw Stop.cancelled }
            v.checked += 1
            job.update { $0.current = "Verifying " + e.rel; if own { $0.scanned += 1 } }
            let p = join(files, e.rel)
            let exists = lstatInfo(p) != nil
            let h = exists ? sha256(p, fromDisk: true, job: job, base: base) : ""
            if job.isCancelled { throw Stop.cancelled }
            var problem = ""
            if h.isEmpty { v.missing += 1; problem = e.rel + (exists ? " (unreadable)" : " (missing)") }
            else if h != e.sha { v.bad += 1; problem = e.rel + " (checksum mismatch)" }
            else { v.ok += 1 }
            if !problem.isEmpty && v.problems.count < 200 { v.problems.append(problem) }
            base += e.size + fileCost
            let b = base, size = e.size, ok = problem.isEmpty
            job.update {
                $0.workDone = b
                if own { if ok { $0.copied += 1 } else { $0.linked += 1 }; $0.bytesWritten += size }
            }
        }
        if v.checked == 0 { return true }
        var j = "{\n  \"verified_iso\": \"\(SnapshotID.isoFormat.string(from: Date()))\",\n"
        j += "  \"checked\": \(v.checked),\n  \"ok\": \(v.ok),\n  \"mismatched\": \(v.bad),\n  \"missing\": \(v.missing),\n  \"problems\": ["
        j += v.problems.isEmpty ? "]\n}\n" : "\n    " + v.problems.map { "\"\(Format.json($0))\"" }.joined(separator: ",\n    ") + "\n  ]\n}\n"
        _ = writeText(join(snap, "verify.json"), j)
        return true
    }

    @discardableResult
    public static func verify(_ snap: String, job: Job) -> VerifyResult {
        var v = VerifyResult()
        do {
            let lock = try VersionsLock((snap as NSString).deletingLastPathComponent)
            defer { withExtendedLifetime(lock) {} }
            guard try verifySnapshot(snap, job, &v) else { throw Stop.fail("This snapshot has no manifest to verify against.") }
            guard v.checked > 0 else { throw Stop.fail("This snapshot has no checksums. Turn on checksums on the Automation page and later snapshots will record them.") }
            let n = v.bad + v.missing
            job.update { $0.warnings = n }
            job.finish(.done, v.summary, path: snap)
        } catch Stop.fail(let m) {
            job.finish(.failed, m)
        } catch {
            job.finish(.cancelled, "Verification cancelled.")
        }
        return v
    }

    /// Restores a snapshot's files into `target`. Existing files are skipped unless `overwrite`.
    public static func restore(_ snap: String, to target: String, overwrite: Bool, job: Job) {
        let from = join(snap, "files"), target = normPath(target)
        do {
            guard isDir(from) else { throw Stop.fail("This snapshot has no files folder.") }
            if isUnder(target, snap) { throw Stop.fail("Choose a restore target outside the snapshot.") }
            try makeDirs(target)
            var files: [FileItem] = [], dirs: [String] = []
            scan(from, "", ScanOptions(), job, &files, &dirs)
            if job.isCancelled { throw Stop.cancelled }
            let total = files.reduce(Int64(0)) { $0 + $1.size + fileCost } + 1
            job.update { $0.workTotal = total; $0.phase = .copying }
            for d in dirs { mkdir(join(target, d), 0o755) }
            var base: Int64 = 0, failed = 0
            for f in files {
                if job.isCancelled { throw Stop.cancelled }
                job.update { $0.current = f.rel }
                let out = join(target, f.rel)
                if lstatInfo(out) != nil && !overwrite {
                    job.update { $0.skipped += 1 }
                } else {
                    if overwrite { unlink(out) }
                    let e = copyFile(join(from, f.rel), out, preserve: true, job: job, base: base)
                    if e == 0 { let n = f.size; job.update { $0.copied += 1; $0.bytesWritten += n } }
                    else if e == ECANCELED || job.isCancelled { throw Stop.cancelled }
                    else if e == ENOSPC { throw Stop.fail("The target drive is full.") }
                    else { failed += 1 }
                }
                base += f.size + fileCost
                let b = base
                job.update { $0.workDone = b }
            }
            let st = job.state
            var msg = "\(Format.num(st.copied)) files restored to \(target)."
            if st.skipped > 0 { msg += " \(Format.num(st.skipped)) existing files left untouched." }
            if failed > 0 { msg += " \(Format.num(failed)) files could not be written."; job.update { $0.warnings += failed } }
            job.finish(.done, msg, path: target)
        } catch Stop.fail(let m) {
            job.finish(.failed, m)
        } catch {
            job.finish(.cancelled, "Restore cancelled. Files already restored were kept.")
        }
    }
}
