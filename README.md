# Entropy Backup

**When Entropy is back UP, launch EntropyBackup!....exe** 🙃

Entropy Backup is a simple yet effective native Windows 11 app for incremental, versioned project backups, with a modern frosted-glass UI.

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

Grab `EntropyBackup.exe` from the [latest release](https://github.com/EntropySystem/EntropyBackup/releases/latest). No installer is needed. Requires Windows 10/11 x64.

The executable is not code-signed, so Windows SmartScreen may show a warning. You can also build it yourself from source (below).

## Build from source

Requirements: Visual Studio 2022 (or Build Tools) with **Desktop development with C++**, a Windows SDK, and CMake 3.24+.

```bat
build.cmd
```

or

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build\Release\EntropyBackup.exe` (`build.cmd` also copies it to `dist\`).

## Snapshot format

```
<To>\EntropyBackup_Versions\
    snapshot_2026-10-18_01-32-05\
        files\          complete copy of the project (unchanged files are hard links)
        manifest.tsv    size, timestamp and path of every file
        backup.json     description, source, type, counts, bytes, duration, skipped files
    mirror_latest\      used instead when "Create versioned snapshots" is off
```

A snapshot's `files` folder is a plain copy of your project. You can always restore by hand by copying it anywhere.

Settings, excludes and `runtime.log` are stored in `%APPDATA%\EntropyBackup`.

## License and disclaimer

Released under the [MIT License](LICENSE). The Software is provided **"AS IS", without warranty of any kind**, and the authors are **not liable** for any claim, damages or other liability arising from its use. See [DISCLAIMER.md](DISCLAIMER.md) for the full terms. By using Entropy Backup you accept full responsibility for your data and its backups.

Developed by **Peter V. Odintsov**.
