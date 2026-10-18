import Foundation

/// Progress and outcome of one backup, restore or verification. The engine updates it from a
/// worker thread; the app and the command line read snapshots of `state`.
public final class Job {
    public enum Kind: Int { case backup, restore, verify }
    public enum Phase: Int {
        case idle, scanning, copying, finalizing, done, failed, cancelled
        public var running: Bool { self == .scanning || self == .copying || self == .finalizing }
    }
    public struct State {
        public var kind = Kind.backup, phase = Phase.idle
        public var scanned = 0, copied = 0, linked = 0, skipped = 0, warnings = 0
        public var legs = 1, leg = 0, failedLegs = 0  // backup destinations (2 in Firebox mode), the one being written, those not saved
        public var bytesWritten: Int64 = 0, workDone: Int64 = 0, workTotal: Int64 = 0
        public var current = "", message = "Choose a project and a backup destination.", resultPath = "", title = ""
        public var started = Date(), ended = Date()
        public init() {}
        public var fraction: Double {
            if phase == .done { return 1 }
            guard workTotal > 0, phase != .scanning, phase != .idle else { return 0 }
            return min(running ? 0.995 : 1, Double(workDone) / Double(workTotal))
        }
        public var running: Bool { phase.running }
        public var elapsed: Double { (running ? Date() : ended).timeIntervalSince(started) }
    }

    private let lock = NSLock()
    private var s = State()
    private var stop = false

    public init(_ kind: Kind, title: String) {
        s.kind = kind
        s.title = title
        s.phase = .scanning  // running from the start, so a watcher never sees it idle
    }
    public var state: State { lock.lock(); defer { lock.unlock() }; return s }
    public func update(_ f: (inout State) -> Void) { lock.lock(); f(&s); lock.unlock() }
    public var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return stop }
    public func cancel() { lock.lock(); stop = true; lock.unlock() }

    func finish(_ phase: Phase, _ message: String, path: String = "") {
        var title = "", warnings = 0
        update {
            $0.phase = phase
            $0.message = message
            $0.resultPath = path
            $0.current = ""
            $0.ended = Date()
            title = $0.title
            warnings = $0.warnings
        }
        let status = phase == .done ? (warnings > 0 ? "WARNINGS" : "OK") : phase == .cancelled ? "CANCELLED" : "FAILED"
        Log.event("\(title) | \(status) | \(message)")
    }
    /// 0 success, 1 success with warnings, 2 failed, 4 cancelled (3 is reserved for invalid arguments).
    public var exitCode: Int32 {
        let st = state
        return st.phase == .done ? (st.warnings > 0 ? 1 : 0) : st.phase == .cancelled ? 4 : 2
    }
}
