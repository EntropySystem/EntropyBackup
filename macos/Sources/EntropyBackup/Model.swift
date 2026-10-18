import AppKit
import EntropyCore
import SwiftUI

/// App state: settings, snapshots and the running job, polled from the worker while it runs.
final class AppModel: ObservableObject {
    @Published var settings: EntropyCore.Settings { didSet { if settings != oldValue { settings.save() } } }
    @Published var snapshots: [Snapshot] = []
    @Published var selected: String?
    @Published var state = Job.State()
    @Published var desc = ""
    @Published var restoreTarget = ""
    @Published var schedule = Schedule.Status()
    @Published var flash = ""
    @Published var viewFire = false  // History and Restore list the Firebox instead of the backup folder
    private var job: Job?
    private var timer: Timer?

    init() {
        settings = EntropyCore.Settings.load()
        refresh()
        schedule = Schedule.status()
    }

    var running: Bool { state.running }
    var current: Snapshot? { snapshots.first { $0.dir == selected } }
    var fireboxSet: Bool { settings.fireOn && !settings.fire.isEmpty }
    var viewDest: String { viewFire && fireboxSet ? settings.fire : settings.to }
    /// Warning for the Firebox row: an invalid pair, or a Firebox on the same drive as the backup folder.
    var fireNote: String {
        guard fireboxSet, !settings.to.isEmpty else { return "" }
        if let why = Engine.fireboxConflict(settings.to, settings.fire) { return why }
        return Engine.sameVolume(settings.to, settings.fire) ? "Same drive as To \u{2014} not a real second copy" : ""
    }
    var executable: String { Bundle.main.executablePath ?? CommandLine.arguments[0] }

    func refresh() {
        if !fireboxSet { viewFire = false }
        snapshots = Snapshot.list(viewDest)
        if selected == nil || !snapshots.contains(where: { $0.dir == selected }) { selected = snapshots.first?.dir }
    }

    private func start(_ job: Job, _ work: @escaping () -> Void) {
        self.job = job
        state = job.state
        Thread { work() }.start()
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] t in
            guard let self = self, let job = self.job else { t.invalidate(); return }
            self.state = job.state
            if !self.state.running {
                t.invalidate()
                self.finished(job.state)
            }
        }
    }
    private func finished(_ st: Job.State) {
        refresh()
        guard st.phase == .done else { return }
        switch st.kind {
        case .backup:
            selected = snapshots.first?.dir
            desc = ""
            if settings.openAfter { reveal(st.resultPath) }
        case .restore: reveal(st.resultPath)
        case .verify: break
        }
    }

    func backup() {
        guard !running else { return }
        guard !settings.from.isEmpty, !settings.to.isEmpty else { state.message = "Choose both From and To folders first."; return }
        if settings.fireOn && settings.fire.isEmpty { state.message = "Choose a Firebox folder, or turn Firebox mode off."; return }
        let o = BackupOptions(settings, desc: desc)
        if !o.fire.isEmpty, let why = Engine.fireboxConflict(o.dst, o.fire) { state.message = why; return }
        let job = Job(.backup, title: "Backup \(normPath(o.src)) -> \(normPath(o.dst))" + (o.fire.isEmpty ? "" : " + Firebox \(normPath(o.fire))"))
        start(job) { Engine.backup(o, job: job) }
    }
    func restore() {
        guard !running, let s = current, !restoreTarget.isEmpty else { return }
        let job = Job(.restore, title: "Restore \(s.dir) -> \(restoreTarget)")
        let target = restoreTarget, overwrite = settings.overwrite
        start(job) { Engine.restore(s.dir, to: target, overwrite: overwrite, job: job) }
    }
    func verify() {
        guard !running, let s = current else { return }
        let job = Job(.verify, title: "Verify \(s.dir)")
        start(job) { Engine.verify(s.dir, job: job) }
    }
    func cancel() { job?.cancel() }

    func saveSchedule() {
        flash = Schedule.apply(settings, executable: executable)
        schedule = Schedule.status()
    }
    var commandLine: String {
        let q = { (s: String) in "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        return q(executable) + " backup --from " + q(settings.from.isEmpty ? "<project folder>" : settings.from)
            + " --to " + q(settings.to.isEmpty ? "<backup folder>" : settings.to)
            + (fireboxSet ? " --firebox " + q(settings.fire) : "")
    }
    func copyCommand() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(commandLine, forType: .string)
        flash = "Command copied."
    }

    func pickFolder(_ start: String, prompt: String) -> String? {
        let p = NSOpenPanel()
        p.canChooseDirectories = true
        p.canChooseFiles = false
        p.canCreateDirectories = true
        p.prompt = prompt
        if !start.isEmpty { p.directoryURL = URL(fileURLWithPath: start) }
        return p.runModal() == .OK ? p.url?.path : nil
    }
    func reveal(_ path: String) {
        guard !path.isEmpty else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }
}
