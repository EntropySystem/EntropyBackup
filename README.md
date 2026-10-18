# Entropy Backup

**When Entropy is back UP, launch EntropyBackup!....exe** 🙃

Entropy Backup is a simple yet effective native app for incremental, versioned project backups on **Windows 11** and **macOS**, with a modern frosted-glass UI.

![Entropy Backup — backup page](docs/screenshots/backup.png)

> ⚠️ **Use at your own risk.** Entropy Backup is provided "as is", without warranty of any kind. You are solely responsible for your data and for verifying your backups. The author is not liable for any data loss or damage. Read the full [Disclaimer](DISCLAIMER.md) before use.

## Features

**Backups**
- **From → To → Description → Back Up Now.** That's the whole workflow. The Back Up Now button lives in the bottom-right panel on every page.
- **Incremental** or **Full Copy** snapshots. Every snapshot is a complete, browsable copy of your project.
- **Hard links for unchanged files**, so a new version costs almost no extra space on NTFS.
- A per-snapshot `manifest.tsv` detects unchanged files instantly, and `backup.json` stores the description, counts and duration.
- Snapshots are written to `incomplete_*` first and renamed only once they finish, so a failed or cancelled backup never leaves a half-finished snapshot.
- Live progress: ring and bar, current file, files scanned / copied / unchanged, size written, elapsed time, estimated time remaining, and **Cancel** at any time.
- Locked or unreadable files are skipped and listed instead of failing the whole run.
- Long paths (`\\?\`) are supported, and junction loops are avoided.

**Automation and integrity** (the Automation page)
- **Scheduled backups**: hourly, daily or weekly through Windows Task Scheduler, so they also run while the app is closed. Missed runs start as soon as possible, and the page shows the next and last run. The task points at the exe's current location, so save the schedule again if you move it.
- **Command line and silent mode** for scripts. See [Command line](#command-line).
- **Open and locked files** are copied from a Volume Shadow Copy when that option is on and the backup runs as administrator. Without administrator rights the backup still runs, reads the live files and reports a warning.
- **Retention**: keep the newest *N* snapshots and/or delete snapshots older than *N* days. The newest snapshot is always kept, and because unchanged files are hard links, removing a snapshot never touches the others.
- **SHA-256 checksums** for every file, recorded in the manifest. **Verify** any snapshot from Version History or the command line, or automatically after each backup. Verification reads the files from disk (not the cache) and writes the result to `verify.json`.
- Every finished backup, verification and prune is appended to `backup.log`. A lock file stops a scheduled run and the app from writing to the same destination at once, and leftovers of interrupted runs are cleaned up.

**Versions and restore**
- **Version History** panel and page with full details for every snapshot.
- **Restore** any version into a folder you choose, with an optional overwrite and a confirmation step.
- **Excludes** keep things like `node_modules`, `.vs` and `*.obj` out of snapshots. Wildcards, path patterns and `#` comments are supported.

**Make it yours**
- Custom **background image**, or the built-in rendered mountain dusk.
- **Glass tint** and **Frost (blur)** sliders that update live.
- **Section Outline**: a two-colour gradient around every section.
  - Colour pickers and a swap button.
  - Horizontal, Vertical, Diagonal, Reverse, Mirror and Radial styles.
  - Optional single gradient that flows across the whole window.
  - Thickness and glow sliders.
- Optional **page header banner** layout, frameless window, per-monitor DPI, and a layout that scales with the window.

| History | Settings |
|---|---|
| ![History](docs/screenshots/history.png) | ![Settings](docs/screenshots/settings.png) |

## Download

From the [latest release](https://github.com/EntropySystem/EntropyBackup/releases/latest):

- **Windows 10/11 x64:** `EntropyBackup.exe` (and `EntropyBackupCLI.exe` for scripts). No installer is needed.
- **macOS 14 or later** (including macOS 27, Apple silicon and Intel): `EntropyBackup-macOS.zip`. Unzip it and move *Entropy Backup* to Applications. See [macOS](#macos).

From version 1.6.1 the Windows executables are code-signed (see [Code signing policy](#code-signing-policy)); earlier ones are not, so SmartScreen may warn. The macOS app is not notarized yet, so macOS asks you to allow it once in System Settings › Privacy & Security › **Open Anyway**. You can also build from source (below). Every push is built and tested on GitHub Actions; the downloads of any run are under its *Artifacts*.

## macOS

The macOS app (in [`macos/`](macos)) is a native Swift/SwiftUI app with the same features and the same snapshot format, so a backup folder can be browsed by either app. It uses Liquid Glass on macOS 26 and later.

- **Copies** are APFS clones where possible (instant, no extra space on the same volume), and permissions, extended attributes and dates are preserved.
- **Scheduled backups** are a per-user launchd agent (`~/Library/LaunchAgents/com.entropysystem.entropybackup.scheduled.plist`). A run missed while the Mac slept starts when it wakes.
- **Full Disk Access:** to back up Documents, Desktop or external drives from a scheduled run, add *Entropy Backup* in System Settings › Privacy & Security › Full Disk Access (the Automation page has a button). Re-add it after installing a new build.
- **Open files:** macOS does not lock open files, so they are read as they are. Volume Shadow Copy is Windows-only.
- **Command line:** the app's executable takes the same commands as `EntropyBackupCLI.exe` (without `--vss`):

```sh
alias entropybackup="/Applications/Entropy Backup.app/Contents/MacOS/EntropyBackup"
entropybackup backup --from ~/Projects/MyApp --to /Volumes/Backup --verify --keep 30
```

Build it with Xcode 15 or later: `cd macos && swift test && scripts/make-app.sh` (output in `macos/dist/`).

## Command line

`EntropyBackupCLI.exe` is the same program built for the console, so shells wait for it and get its exit code. `EntropyBackup.exe` accepts the same commands and runs them without a window, which is what the scheduled task uses.

```bat
EntropyBackupCLI backup --from "C:\Projects\MyApp" --to "E:\Backups" --desc "Nightly"
EntropyBackupCLI backup --from "C:\Projects\MyApp" --to "E:\Backups" --vss --verify --keep 30
EntropyBackupCLI verify --to "E:\Backups"                       (newest snapshot)
EntropyBackupCLI verify 2026-10-18_01-32-05 --to "E:\Backups"
EntropyBackupCLI prune  --to "E:\Backups" --keep 30 --keep-days 90
EntropyBackupCLI list   --to "E:\Backups"
```

Options you don't pass come from the app's saved settings (backup type, excludes, checksums and so on). Run `EntropyBackupCLI help` for the full list.

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | Success with warnings (skipped files, checksum problems, shadow copy unavailable) |
| 2 | Failed |
| 3 | Invalid arguments |
| 4 | Cancelled (Ctrl+C) |

To schedule it with your own tooling instead of the Automation page:

```bat
schtasks /Create /TN "Nightly project backup" /SC DAILY /ST 02:00 /RL HIGHEST ^
  /TR "\"C:\Tools\EntropyBackup.exe\" backup --scheduled --from \"C:\Projects\MyApp\" --to \"E:\Backups\" --vss"
```

## Build from source (Windows)

Requirements: Visual Studio 2022 (or Build Tools) with **Desktop development with C++**, a Windows SDK, and CMake 3.24+.

```bat
build.cmd
```

or

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build\Release\EntropyBackup.exe` and `build\Release\EntropyBackupCLI.exe` (`build.cmd` also copies both to `dist\`).

## Snapshot format

```
<To>\EntropyBackup_Versions\
    snapshot_2026-10-18_01-32-05\
        files\          complete copy of the project (unchanged files are hard links)
        manifest.tsv    size, timestamp, SHA-256 (when enabled) and path of every file
        backup.json     description, source, type, counts, bytes, duration, skipped files
        verify.json     result of the last verification (if any)
    mirror_latest\      used instead when "Create versioned snapshots" is off
```

A snapshot's `files` folder is a plain copy of your project. You can always restore by hand by copying it anywhere.

Settings, excludes, `backup.log` and `runtime.log` are stored in `%APPDATA%\EntropyBackup`.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

- Signed files: `EntropyBackup.exe` and `EntropyBackupCLI.exe` in the [releases](https://github.com/EntropySystem/EntropyBackup/releases) of this repository.
- Every signed file is built from this repository's source by GitHub Actions ([`build.yml`](.github/workflows/build.yml)). Nothing built anywhere else is signed.
- Committers and reviewers: the [members of this repository](https://github.com/EntropySystem/EntropyBackup/graphs/contributors).
- Approver for each signing request: Peter V. Odintsov.

**Privacy policy:** this program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it. Entropy Backup only reads and writes the folders you choose; settings and logs stay on your computer.

## License and disclaimer

Released under the [MIT License](LICENSE). The Software is provided **"AS IS", without warranty of any kind**, and the authors are **not liable** for any claim, damages or other liability arising from its use. See [DISCLAIMER.md](DISCLAIMER.md) for the full terms. By using Entropy Backup you accept full responsibility for your data and its backups.

Developed by **Peter V. Odintsov**.
