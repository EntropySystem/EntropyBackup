import Darwin
import XCTest
@testable import EntropyCore

final class EngineTests: XCTestCase {
    private var tmp = ""

    override func setUpWithError() throws {
        tmp = NSTemporaryDirectory() + "entropy-tests-" + UUID().uuidString
        try FileManager.default.createDirectory(atPath: tmp + "/src/sub", withIntermediateDirectories: true)
        setenv("ENTROPYBACKUP_HOME", tmp + "/home", 1)
        CLI.quiet = true
    }
    override func tearDown() { removeTree(tmp) }

    private func write(_ rel: String, _ s: String) {
        XCTAssertTrue(FileManager.default.createFile(atPath: tmp + "/src/" + rel, contents: Data(s.utf8)))
    }
    private func options() -> BackupOptions {
        var s = Settings()
        s.from = tmp + "/src"
        s.to = tmp + "/dst"
        s.checksums = true
        s.verifyAfter = true
        return BackupOptions(s)
    }
    private func backup(_ o: BackupOptions) -> Job {
        let j = Job(.backup, title: "test")
        Engine.backup(o, job: j)
        return j
    }

    func testExcludePatterns() {
        XCTAssertTrue(Excludes.match("*.TMP", "notes.tmp"))
        XCTAssertTrue(Excludes.match("node_modules", "Node_Modules"))
        XCTAssertTrue(Excludes.match("src/gen/*", "src/gen/a/b.swift"))
        XCTAssertFalse(Excludes.match("*.o", "main.obj"))
        XCTAssertEqual(Excludes.patterns("# comment\nbuild/\nsrc\\gen\n\n  *.log  "), ["build", "src/gen", "*.log"])
    }

    func testManifestReadsBothVersions() throws {
        let p = tmp + "/m.tsv"
        XCTAssertTrue(writeText(p, "# v2\n5\t10\tabc\ta.txt\n6\t11\t-\tsub/b c.txt\n7\t12\tlegacy/v1.txt\n"))
        let m = try XCTUnwrap(loadManifest(p))
        XCTAssertEqual(m.map(\.rel), ["a.txt", "sub/b c.txt", "legacy/v1.txt"])
        XCTAssertEqual(m.map(\.sha), ["abc", "", ""])
        XCTAssertEqual(m[2].size, 7)
    }

    func testBackupIncrementalVerifyRestoreAndRetention() throws {
        write("a.txt", "hello\n")
        write("sub/b.txt", "world")
        write(".DS_Store", "junk")

        let first = backup(options())
        XCTAssertEqual(first.state.phase, .done, first.state.message)
        XCTAssertEqual(first.exitCode, 0)
        var snaps = Snapshot.list(tmp + "/dst")
        XCTAssertEqual(snaps.count, 1)
        XCTAssertTrue(snaps[0].sums && snaps[0].verified && snaps[0].vBad == 0)
        XCTAssertEqual(snaps[0].files, 2, ".DS_Store is excluded by default")
        let man = try XCTUnwrap(loadManifest(snaps[0].dir + "/manifest.tsv"))
        XCTAssertEqual(man.first { $0.rel == "a.txt" }?.sha, "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03")

        sleep(1)  // snapshot IDs have one-second resolution
        write("a.txt", "changed")
        let second = backup(options())
        XCTAssertEqual(second.state.copied, 1)
        XCTAssertEqual(second.state.linked, 1)
        snaps = Snapshot.list(tmp + "/dst")
        XCTAssertEqual(snaps.count, 2)
        var st = stat()
        XCTAssertEqual(lstat(snaps[0].dir + "/files/sub/b.txt", &st), 0)
        XCTAssertEqual(st.st_nlink, 2, "unchanged files are hard links to the previous snapshot")

        let restored = tmp + "/restored"
        let r = Job(.restore, title: "test")
        Engine.restore(snaps[1].dir, to: restored, overwrite: false, job: r)
        XCTAssertEqual(r.state.phase, .done, r.state.message)
        XCTAssertEqual(try String(contentsOfFile: restored + "/a.txt"), "hello\n")

        XCTAssertTrue(writeText(snaps[0].dir + "/files/a.txt", "tampered"))
        let v = Job(.verify, title: "test")
        let result = Engine.verify(snaps[0].dir, job: v)
        XCTAssertEqual(result.bad, 1)
        XCTAssertEqual(v.exitCode, 1)

        sleep(1)
        var keepOne = options()
        keepOne.keep = 1
        XCTAssertEqual(backup(keepOne).state.phase, .done)
        snaps = Snapshot.list(tmp + "/dst")
        XCTAssertEqual(snaps.count, 1)
        XCTAssertEqual(try String(contentsOfFile: snaps[0].dir + "/files/sub/b.txt"), "world", "pruning leaves linked files intact")
    }

    func testSecondWriterIsRefused() throws {
        let root = tmp + "/dst/EntropyBackup_Versions"
        try FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
        write("a.txt", "x")
        let lock = try VersionsLock(root)
        let j = backup(options())
        XCTAssertEqual(j.state.phase, .failed)
        XCTAssertTrue(j.state.message.contains("already"))
        withExtendedLifetime(lock) {}
    }

    func testCommandLineExitCodes() {
        write("a.txt", "x")
        XCTAssertEqual(CLI.run(["help"]), 0)
        XCTAssertEqual(CLI.run(["backup", "--bogus"]), 3)
        XCTAssertEqual(CLI.run(["prune", "--to", tmp + "/dst"]), 3)
        XCTAssertEqual(CLI.run(["backup", "--from", tmp + "/missing", "--to", tmp + "/dst"]), 2)
        XCTAssertEqual(CLI.run(["backup", "--from", tmp + "/src", "--to", tmp + "/dst", "--verify"]), 0)
        XCTAssertEqual(CLI.run(["list", "--to", tmp + "/dst"]), 0)
        XCTAssertEqual(CLI.run(["verify", "--to", tmp + "/dst"]), 0)
    }
}
