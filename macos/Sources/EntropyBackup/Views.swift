import EntropyCore
import SwiftUI

enum Theme {
    static let gold = Color(red: 0.92, green: 0.77, blue: 0.42)
    static let goldLight = Color(red: 0.98, green: 0.89, blue: 0.63)
    static let text = Color(red: 0.95, green: 0.96, blue: 0.98)
    static let dim = Color(red: 0.64, green: 0.71, blue: 0.79)
    static let bad = Color(red: 1.0, green: 0.61, blue: 0.54)
    static let field = Color.black.opacity(0.28)
}

extension View {
    /// Frosted section: Liquid Glass on macOS 26 and later, a translucent material before that.
    @ViewBuilder func glassPanel(radius: CGFloat = 22) -> some View {
        #if compiler(>=6.2)
        if #available(macOS 26.0, *) {
            padding(18).glassEffect(.regular, in: .rect(cornerRadius: radius))
        } else {
            materialPanel(radius)
        }
        #else
        materialPanel(radius)
        #endif
    }
    func materialPanel(_ radius: CGFloat) -> some View {
        padding(18)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(Color.white.opacity(0.16), lineWidth: 1))
    }
    func sectionTitle() -> some View { font(.title3.weight(.semibold)).foregroundStyle(Theme.gold) }
}

enum Page: Int, CaseIterable, Identifiable {
    case backup, history, restore, automation, settings, about
    var id: Int { rawValue }
    var name: String { ["Backup", "History", "Restore", "Automation", "Settings", "About"][rawValue] }
    var title: String { ["Project Backup", "Version History", "Restore", "Automation", "Settings", "About"][rawValue] }
    var icon: String { ["house", "clock.arrow.circlepath", "arrow.uturn.backward.circle", "calendar.badge.clock", "gearshape", "info.circle"][rawValue] }
    var subtitle: String {
        ["Incremental. Versioned. Simple.", "Every snapshot, ready when you need it.", "Bring any version back in one step.",
         "Schedules, retention and integrity checks.", "Tune how Entropy Backup works.", "Entropy Backup for macOS."][rawValue]
    }
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var page = Page.backup

    var body: some View {
        ZStack {
            Backdrop().ignoresSafeArea()
            HStack(alignment: .top, spacing: 14) {
                Sidebar(page: $page)
                    .frame(width: 236)
                    .frame(maxHeight: .infinity)
                    .glassPanel(radius: 24)
                VStack(spacing: 14) {
                    Header(page: page).glassPanel()
                    pageView
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                        .glassPanel()
                    StatusPanel().glassPanel()
                }
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 16)
            .padding(.top, 30)
        }
        .preferredColorScheme(.dark)
        .tint(Theme.gold)
        .foregroundStyle(Theme.text)
    }

    @ViewBuilder var pageView: some View {
        switch page {
        case .backup: BackupPage()
        case .history: HistoryPage()
        case .restore: RestorePage()
        case .automation: AutomationPage()
        case .settings: SettingsPage()
        case .about: AboutPage()
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Chrome: backdrop, logo, sidebar, header, status
// ---------------------------------------------------------------------------------------------
struct Ridge: Shape {
    let heights: [CGFloat]  // fractions of the height, evenly spaced from left to right
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.minX, y: r.maxY))
        for (i, h) in heights.enumerated() {
            p.addLine(to: CGPoint(x: r.minX + r.width * CGFloat(i) / CGFloat(heights.count - 1), y: r.maxY - r.height * h))
        }
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}
/// Mountain dusk, echoing the Windows app's backdrop.
struct Backdrop: View {
    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.05, green: 0.09, blue: 0.20), Color(red: 0.19, green: 0.17, blue: 0.36), Color(red: 0.62, green: 0.40, blue: 0.42)],
                           startPoint: .top, endPoint: .bottom)
            Ridge(heights: [0.38, 0.52, 0.44, 0.63, 0.47, 0.58, 0.41, 0.55, 0.36])
                .fill(Color(red: 0.20, green: 0.20, blue: 0.36).opacity(0.75))
            Ridge(heights: [0.30, 0.22, 0.36, 0.27, 0.40, 0.25, 0.33, 0.21, 0.29])
                .fill(Color(red: 0.09, green: 0.11, blue: 0.22).opacity(0.92))
        }
    }
}
struct Hexagon: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        for i in 0..<6 {
            let a = CGFloat(i) * .pi / 3 - .pi / 2
            let pt = CGPoint(x: r.midX + r.width / 2 * cos(a), y: r.midY + r.height / 2 * sin(a))
            if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
        }
        p.closeSubpath()
        return p
    }
}
struct Logo: View {
    var size: CGFloat = 88
    var body: some View {
        ZStack {
            Hexagon().fill(LinearGradient(colors: [Color(red: 0.16, green: 0.36, blue: 0.78), Color(red: 0.05, green: 0.12, blue: 0.34)], startPoint: .top, endPoint: .bottom))
            Hexagon().stroke(Theme.gold.opacity(0.85), lineWidth: 2)
            Image(systemName: "externaldrive.fill.badge.timemachine")
                .font(.system(size: size * 0.34))
                .foregroundStyle(Theme.goldLight)
        }
        .frame(width: size, height: size)
        .shadow(color: Color.blue.opacity(0.5), radius: 14)
    }
}
struct Sidebar: View {
    @Binding var page: Page
    var body: some View {
        VStack(spacing: 10) {
            Logo().padding(.top, 8)
            (Text("Entropy ").foregroundColor(Theme.text) + Text("Backup").foregroundColor(Theme.gold))
                .font(.system(size: 26, weight: .bold))
            Text("Your Code. Always Within Reach.").font(.callout).foregroundStyle(Theme.dim)
            VStack(spacing: 6) {
                ForEach(Page.allCases) { p in
                    Button { page = p } label: {
                        HStack(spacing: 14) {
                            Image(systemName: p.icon).frame(width: 24)
                            Text(p.name)
                            Spacer()
                        }
                        .font(.system(size: 16, weight: page == p ? .semibold : .regular))
                        .padding(.horizontal, 14)
                        .padding(.vertical, 10)
                        .background(page == p ? Theme.gold.opacity(0.32) : Color.clear, in: RoundedRectangle(cornerRadius: 12))
                        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(page == p ? Theme.gold : Color.clear, lineWidth: 1.2))
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.top, 14)
            Spacer()
            VStack(alignment: .leading, spacing: 2) {
                Text("Developed by").font(.caption).foregroundStyle(Theme.dim)
                Text("Peter V. Odintsov")
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
struct Header: View {
    let page: Page
    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: page.icon).font(.system(size: 30)).foregroundStyle(Theme.gold)
            VStack(alignment: .leading, spacing: 2) {
                Text(page.title).font(.system(size: 26, weight: .semibold))
                Text(page.subtitle).foregroundStyle(Theme.dim)
            }
            Spacer()
            Text("Keep your projects safe.\nFocus on building.").font(.callout).foregroundStyle(Theme.dim)
        }
    }
}
struct StatusPanel: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let s = model.state
        HStack(spacing: 22) {
            ZStack {
                Circle().stroke(Color.white.opacity(0.1), lineWidth: 9)
                Circle()
                    .trim(from: 0, to: s.fraction)
                    .stroke(LinearGradient(colors: [Theme.goldLight, Theme.gold], startPoint: .topLeading, endPoint: .bottomTrailing), style: StrokeStyle(lineWidth: 9, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                Text("\(Int(s.fraction * 100 + (s.phase == .done ? 0.5 : 0)))%").font(.title2.weight(.semibold))
            }
            .frame(width: 88, height: 88)
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(title(s)).font(.title3.weight(.semibold)).foregroundStyle(problem(s) ? Theme.bad : Theme.text)
                    Spacer()
                    if s.running { Button("Cancel", role: .cancel) { model.cancel() } }
                }
                Text(line(s)).foregroundStyle(Theme.dim).lineLimit(2)
                ProgressView(value: s.fraction).tint(Theme.gold)
                HStack(spacing: 22) {
                    stat(s.kind == .verify ? "Files Checked" : "Files Scanned", Format.num(s.scanned), s)
                    stat(s.kind == .verify ? "Matched" : s.kind == .restore ? "Files Restored" : "Files Copied", Format.num(s.copied), s)
                    stat(s.kind == .verify ? "Problems" : s.kind == .restore ? "Skipped (Existing)" : "Unchanged (Linked)", Format.num(s.kind == .restore ? s.skipped : s.linked), s)
                    stat(s.kind == .verify ? "Size Read" : "Size Written", Format.bytes(s.bytesWritten), s)
                    stat("Elapsed", Format.duration(s.elapsed), s)
                }
            }
            VStack(spacing: 10) {
                Button { model.backup() } label: {
                    Label(model.running ? ["Backing Up...", "Restoring...", "Verifying..."][s.kind.rawValue] : "Back Up Now", systemImage: "externaldrive.fill.badge.plus")
                        .font(.headline)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 8)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(model.running)
                Text(subtitle).font(.caption).foregroundStyle(Theme.dim).multilineTextAlignment(.center).lineLimit(2)
            }
            .frame(width: 230)
        }
    }

    private var subtitle: String {
        if model.running { return "A job is running." }
        if model.settings.from.isEmpty || model.settings.to.isEmpty { return "Set From and To on the Backup page first." }
        return "Snapshot \u{201C}\((model.settings.from as NSString).lastPathComponent)\u{201D} now."
    }
    private func problem(_ s: Job.State) -> Bool { s.phase == .failed || (s.phase == .done && s.kind == .verify && s.warnings > 0) }
    private func title(_ s: Job.State) -> String {
        let names: [[String]] = [
            ["Ready to back up", "Scanning project...", "Backing up files...", "Finalizing snapshot...", "Backup complete", "Backup failed", "Backup cancelled"],
            ["Ready to back up", "Preparing restore...", "Restoring files...", "Finalizing...", "Restore complete", "Restore failed", "Restore cancelled"],
            ["Ready to back up", "Preparing verification...", "Verifying checksums...", "Finalizing...", "Verification passed", "Verification failed", "Verification cancelled"],
        ]
        if s.phase == .done && s.kind == .verify && s.warnings > 0 { return "Verification found problems" }
        return names[s.kind.rawValue][s.phase.rawValue]
    }
    private func line(_ s: Job.State) -> String {
        switch s.phase {
        case .idle:
            return model.settings.from.isEmpty || model.settings.to.isEmpty ? s.message : "Folders are set. Press Back Up Now to create a snapshot."
        case .scanning: return "Scanning: " + s.current
        case .copying: return s.kind == .verify ? s.current : (s.kind == .restore ? "Restoring: " : "Copying: ") + s.current
        case .finalizing: return s.current.isEmpty ? "Writing manifest and metadata..." : s.current
        default: return s.message
        }
    }
    private func stat(_ label: String, _ value: String, _ s: Job.State) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.caption).foregroundStyle(Theme.dim)
            Text(s.phase == .idle ? "\u{2014}" : value).font(.system(.body, design: .rounded).weight(.medium)).monospacedDigit()
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------------------------
struct FolderRow: View {
    @EnvironmentObject var model: AppModel
    let icon: String, title: String, hint: String
    @Binding var path: String
    let prompt: String

    var body: some View {
        HStack(alignment: .bottom, spacing: 12) {
            Image(systemName: icon).font(.title2).foregroundStyle(Theme.gold).frame(width: 30).padding(.bottom, 6)
            VStack(alignment: .leading, spacing: 5) {
                Text(title).font(.headline)
                HStack {
                    TextField(hint, text: $path)
                        .textFieldStyle(.plain)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 8)
                        .background(Theme.field, in: RoundedRectangle(cornerRadius: 9))
                    Button {
                        if let p = model.pickFolder(path, prompt: prompt) { path = p }
                    } label: { Label("Browse", systemImage: "folder") }
                }
            }
        }
    }
}

struct BackupPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            FolderRow(icon: "folder", title: "From", hint: "The project folder to protect", path: $model.settings.from, prompt: "Back Up This Folder")
            FolderRow(icon: "externaldrive", title: "To", hint: "A backup folder, ideally on another drive", path: $model.settings.to, prompt: "Store Backups Here")
                .onChange(of: model.settings.to) { model.refresh() }
            VStack(alignment: .leading, spacing: 6) {
                Label("Description (optional)", systemImage: "doc.text").font(.headline)
                TextEditor(text: $model.desc)
                    .scrollContentBackground(.hidden)
                    .padding(6)
                    .background(Theme.field, in: RoundedRectangle(cornerRadius: 9))
                    .frame(height: 64)
                    .onChange(of: model.desc) { if model.desc.count > 500 { model.desc = String(model.desc.prefix(500)) } }
                Text("\(model.desc.count) / 500").font(.caption).foregroundStyle(Theme.dim).frame(maxWidth: .infinity, alignment: .trailing)
            }
            HStack(alignment: .top, spacing: 36) {
                VStack(alignment: .leading, spacing: 10) {
                    Text("Backup Type").font(.headline)
                    Picker("Backup Type", selection: $model.settings.full) {
                        Text("Incremental (faster)").tag(false)
                        Text("Full Copy (complete snapshot)").tag(true)
                    }
                    .pickerStyle(.radioGroup)
                    .labelsHidden()
                }
                Divider().frame(height: 120)
                VStack(alignment: .leading, spacing: 8) {
                    Text("Options").font(.headline)
                    Toggle("Use hard links for unchanged files", isOn: $model.settings.hardLinks).disabled(model.settings.full)
                    Toggle("Preserve permissions, extended attributes and dates", isOn: $model.settings.preserve)
                    Toggle("Create versioned snapshots", isOn: $model.settings.versioned)
                    Toggle("Show the snapshot in Finder when done", isOn: $model.settings.openAfter)
                }
                .toggleStyle(.checkbox)
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.black.opacity(0.18), in: RoundedRectangle(cornerRadius: 14))
        }
    }
}

struct SnapshotCard: View {
    let s: Snapshot
    let selected: Bool
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: s.mirror ? "arrow.triangle.2.circlepath" : "cylinder.split.1x2.fill").font(.title2).foregroundStyle(Theme.gold)
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text(s.date).fontWeight(.semibold)
                    Text(s.time + (s.mirror ? "  \u{00B7}  Mirror" : "")).foregroundStyle(Theme.dim)
                }
                Text(s.desc.isEmpty ? "No description" : s.desc).font(.callout).foregroundStyle(Theme.dim).lineLimit(1)
            }
            Spacer()
            if s.sums {
                Image(systemName: !s.verified ? "number" : s.vBad > 0 ? "exclamationmark.triangle.fill" : "checkmark.seal.fill")
                    .foregroundStyle(s.vBad > 0 ? Theme.bad : Theme.dim)
            }
        }
        .padding(12)
        .contentShape(Rectangle())
        .background(selected ? Theme.gold.opacity(0.22) : Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(selected ? Theme.gold : Color.white.opacity(0.15), lineWidth: selected ? 1.5 : 1))
    }
}
struct SnapshotList: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        if model.snapshots.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                Text(model.settings.to.isEmpty ? "No backup folder selected." : "No snapshots yet.").font(.headline)
                Text(model.settings.to.isEmpty ? "Choose a backup folder on the Backup page to see its versions." : "Run your first backup and it will appear here.")
                    .foregroundStyle(Theme.dim)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        } else {
            ScrollView {
                LazyVStack(spacing: 8) {
                    ForEach(model.snapshots) { s in
                        SnapshotCard(s: s, selected: s.dir == model.selected).onTapGesture { model.selected = s.dir }
                    }
                }
            }
        }
    }
}

struct HistoryPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        HStack(alignment: .top, spacing: 20) {
            SnapshotList().frame(width: 330)
            Divider()
            if let s = model.current { details(s) } else { Spacer() }
        }
    }
    private func details(_ s: Snapshot) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("\(s.date)   \(s.time)").font(.title2.weight(.semibold))
            Text(s.mirror ? "Mirror (latest only)" : "Snapshot \(s.sid)").foregroundStyle(Theme.gold)
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 8) {
                row("Description", s.desc)
                row("Source", s.source)
                row("Type", s.type.isEmpty ? "" : s.type == "full" ? "Full copy" : "Incremental")
                row("Files", s.files > 0 ? Format.num(s.files) : "")
                row("Copied", Format.num(s.copied))
                row("Unchanged", Format.num(s.reused) + " (linked)")
                row("Data written", Format.bytes(s.bytes) + (s.bytesTotal > 0 ? " of " + Format.bytes(s.bytesTotal) : ""))
                row("Duration", s.seconds > 0 ? Format.duration(s.seconds) : "")
                row("Skipped", s.failed > 0 ? Format.num(s.failed) + " unreadable files" : "")
                row("Checksums", !s.sums ? "Not recorded" : !s.verified ? "Recorded, not verified yet"
                    : s.vBad > 0 ? Format.num(s.vBad) + " problems found " + s.vWhen : "Verified " + s.vWhen)
            }
            Spacer()
            HStack {
                Button { model.reveal(s.dir) } label: { Label("Show in Finder", systemImage: "folder") }
                Button { model.verify() } label: { Label("Verify", systemImage: "checkmark.seal") }
                    .disabled(!s.sums || model.running)
            }
        }
    }
    private func row(_ k: String, _ v: String) -> some View {
        GridRow {
            Text(k).foregroundStyle(Theme.dim)
            Text(v.isEmpty ? "\u{2014}" : v).textSelection(.enabled).lineLimit(3)
        }
    }
}

struct RestorePage: View {
    @EnvironmentObject var model: AppModel
    @State private var confirm = false
    var body: some View {
        HStack(alignment: .top, spacing: 20) {
            SnapshotList().frame(width: 330)
            Divider()
            VStack(alignment: .leading, spacing: 12) {
                Text("Selected version").foregroundStyle(Theme.dim)
                if let s = model.current {
                    Text("\(s.date)   \(s.time)").font(.title3.weight(.semibold))
                    Text(s.desc.isEmpty ? "No description" : s.desc).foregroundStyle(Theme.dim)
                } else {
                    Text("None").font(.title3)
                }
                FolderRow(icon: "folder", title: "Restore to", hint: "An empty folder gives an exact copy", path: $model.restoreTarget, prompt: "Restore Here")
                    .padding(.top, 10)
                Toggle("Overwrite existing files", isOn: $model.settings.overwrite).toggleStyle(.checkbox)
                Text(model.settings.overwrite ? "Files that exist in the target are replaced with the version from this snapshot."
                                              : "Files that already exist in the target are left untouched.")
                    .font(.callout).foregroundStyle(Theme.dim)
                Text("Restoring never modifies the snapshot itself \u{2014} every version stays intact.").font(.callout).foregroundStyle(Theme.dim)
                Spacer()
                HStack {
                    Button {
                        if let s = model.current { model.reveal((s.dir as NSString).appendingPathComponent("files")) }
                    } label: { Label("Explore Version", systemImage: "folder") }
                    .disabled(model.current == nil)
                    Spacer()
                    Button { confirm = true } label: { Label("Restore Now", systemImage: "arrow.uturn.backward") }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.running || model.current == nil || model.restoreTarget.isEmpty)
                }
            }
        }
        .confirmationDialog("Restore this version?", isPresented: $confirm) {
            Button("Restore") { model.restore() }
        } message: {
            Text("Files go into \(model.restoreTarget). " + (model.settings.overwrite ? "Existing files with the same name will be overwritten." : "Existing files are left untouched."))
        }
    }
}

struct AutomationPage: View {
    @EnvironmentObject var model: AppModel
    private var time: Binding<Date> {
        Binding(
            get: { Calendar.current.date(bySettingHour: model.settings.schedHour, minute: model.settings.schedMin, second: 0, of: Date()) ?? Date() },
            set: {
                let c = Calendar.current.dateComponents([.hour, .minute], from: $0)
                model.settings.schedHour = c.hour ?? 2
                model.settings.schedMin = c.minute ?? 0
            })
    }
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Scheduled Backups").sectionTitle()
                HStack(spacing: 18) {
                    Picker("Run", selection: $model.settings.schedMode) {
                        Text("Off").tag(0)
                        Text("Hourly").tag(1)
                        Text("Daily").tag(2)
                        Text("Weekly").tag(3)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 330)
                    if model.settings.schedMode == 1 {
                        Stepper("At minute :" + String(format: "%02ld", model.settings.schedMin), value: $model.settings.schedMin, in: 0...59)
                    } else if model.settings.schedMode > 1 {
                        DatePicker("At", selection: time, displayedComponents: .hourAndMinute).fixedSize()
                    }
                    if model.settings.schedMode == 3 {
                        Picker("on", selection: $model.settings.schedDay) {
                            ForEach(0..<7, id: \.self) { d in Text(Calendar.current.weekdaySymbols[d]).tag(d) }
                        }
                        .fixedSize()
                    }
                }
                Text(model.schedule.summary).font(.callout)
                if model.schedule.exists {
                    Text("Next run: " + (model.schedule.next.map { SnapshotID.display($0) } ?? "\u{2014}") + "     Last run: " + (model.schedule.last.isEmpty ? "never" : model.schedule.last))
                        .font(.callout).foregroundStyle(Theme.dim)
                }
                HStack(spacing: 14) {
                    Button { model.saveSchedule() } label: { Label("Save Schedule", systemImage: "checkmark") }
                    if !model.flash.isEmpty { Text(model.flash).foregroundStyle(Theme.gold) }
                }
                Text("Runs in the background while you are signed in, even with the app closed. A run missed while the Mac slept starts when it wakes.")
                    .font(.caption).foregroundStyle(Theme.dim)
                Divider()
                Text("Retention").sectionTitle()
                HStack {
                    Text("Keep the newest").frame(width: 120, alignment: .leading)
                    TextField("", value: $model.settings.keepCount, format: .number).frame(width: 70)
                    Stepper("", value: $model.settings.keepCount, in: 0...99999).labelsHidden()
                    Text("snapshots   (0 = keep all)").foregroundStyle(Theme.dim)
                }
                HStack {
                    Text("Delete after").frame(width: 120, alignment: .leading)
                    TextField("", value: $model.settings.keepDays, format: .number).frame(width: 70)
                    Stepper("", value: $model.settings.keepDays, in: 0...99999).labelsHidden()
                    Text("days   (0 = never; the newest snapshot always stays)").foregroundStyle(Theme.dim)
                }
                Divider()
                Text("Integrity").sectionTitle()
                Toggle("Record a SHA-256 checksum for every file", isOn: $model.settings.checksums)
                Toggle("Verify each new snapshot against its checksums", isOn: $model.settings.verifyAfter).disabled(!model.settings.checksums)
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: "lock.shield").foregroundStyle(Theme.gold)
                    Text("Scheduled runs need Full Disk Access to read Documents, Desktop, external drives and other protected folders.")
                        .font(.callout).foregroundStyle(Theme.dim)
                    Button("Open Privacy Settings") {
                        if let u = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles") { NSWorkspace.shared.open(u) }
                    }
                }
                Divider()
                HStack {
                    Text("Command Line").sectionTitle()
                    Spacer()
                    Button { model.copyCommand() } label: { Label("Copy", systemImage: "doc.on.doc") }
                }
                Text(model.commandLine).font(.system(.callout, design: .monospaced)).foregroundStyle(Theme.dim).textSelection(.enabled)
            }
            .toggleStyle(.checkbox)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear { model.flash = ""; model.schedule = Schedule.status() }
    }
}

struct SettingsPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("General").sectionTitle()
            Toggle("Remember last used folders", isOn: $model.settings.remember)
            Toggle("Skip hidden files (names starting with a dot); the versions folder is always skipped", isOn: $model.settings.skipHidden)
            Divider()
            HStack {
                Text("Excludes").sectionTitle()
                Spacer()
                Button("Reset to Defaults") { model.settings.excludes = EntropyCore.Settings.defaultExcludes }
            }
            Text("One pattern per line. A plain name such as node_modules matches at any depth; * and ? are wildcards; patterns containing / match the path relative to the project (e.g. src/generated/*). Lines starting with # are comments.")
                .font(.callout).foregroundStyle(Theme.dim)
            TextEditor(text: $model.settings.excludes)
                .font(.system(.body, design: .monospaced))
                .scrollContentBackground(.hidden)
                .padding(8)
                .background(Theme.field, in: RoundedRectangle(cornerRadius: 10))
            Text("\(Excludes.patterns(model.settings.excludes).count) active patterns.  Settings and backup.log live in \(EntropyCore.Settings.directory).")
                .font(.caption).foregroundStyle(Theme.dim)
        }
        .toggleStyle(.checkbox)
    }
}

struct AboutPage: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 18) {
                Logo(size: 76)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Entropy Backup").font(.system(size: 30, weight: .semibold))
                    Text("Version \(appVersion)  \u{00B7}  Native macOS  \u{00B7}  Swift, SwiftUI").foregroundStyle(Theme.gold)
                }
            }
            Text("When Entropy is back UP, launch EntropyBackup!....app").font(.title3.weight(.semibold))
            Text("Entropy Backup keeps immutable, timestamped snapshots of your projects. Each snapshot is a complete, browsable copy, while files that did not change are hard-linked to the previous snapshot so they take no additional space.")
                .foregroundStyle(Theme.text.opacity(0.85))
            Text("How it works").font(.headline).foregroundStyle(Theme.gold)
            VStack(alignment: .leading, spacing: 6) {
                bullet("Each backup is written to a temporary folder and renamed to snapshot_YYYY-MM-DD_HH-MM-SS only once it is complete.")
                bullet("manifest.tsv records every file's size, timestamp and SHA-256, so the next backup recognises unchanged files instantly and verification can prove a snapshot is intact.")
                bullet("New copies are APFS clones where possible, and the snapshot format matches the Windows app.")
                bullet("A snapshot's files folder is a plain copy of your project \u{2014} copy it anywhere to restore by hand.")
            }
            Spacer()
            Text("Provided \"AS IS\" under the MIT License, without warranty of any kind. You are solely responsible for your data and for verifying your backups; the author is not liable for any data loss or damage. Keep more than one backup.")
                .font(.caption).foregroundStyle(Theme.dim)
            HStack {
                Text("Developed by Peter V. Odintsov").fontWeight(.semibold)
                Spacer()
                Button { model.reveal(EntropyCore.Settings.directory) } label: { Label("Open Data Folder", systemImage: "folder") }
            }
        }
    }
    private func bullet(_ t: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Circle().fill(Theme.gold).frame(width: 5, height: 5)
            Text(t).foregroundStyle(Theme.text.opacity(0.8))
        }
    }
}
