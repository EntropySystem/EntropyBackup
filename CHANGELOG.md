# Changelog

## 1.7.0
**Firebox mode: no single point of failure.** Even a RAID NAS is one machine in one place, so a single backup folder can still lose everything. Firebox mode backs up to a second location as well, named after the fireproof boxes that backups were once kept in, off site, in case of fire.
- **Backup page:** a **Firebox mode** switch next to *To*. Turning it on adds a **Firebox** folder row with a flame icon. The app warns when the Firebox is on the same drive as the backup folder, and refuses a Firebox that is the backup folder itself or sits inside the other destination's versions folder.
- **Independent copies:** each destination gets its own complete snapshot chain (manifest, hard links, checksums, retention), read straight from the project and never copied from the other. On Windows both come from one scan and one shadow copy, so they show the same moment, and each file is hashed only once.
- **No single point of failure:** if one destination cannot be reached (an unplugged drive, an offline NAS) or fails part-way, the other is still saved. The status panel reports "Backup incomplete — one destination failed" in red, `backup.log` records which one failed and why, and the command line exits with 1 (warnings). The run fails (exit 2) only if neither destination could be saved.
- **Progress:** the status panel shows which destination is being written ("Primary 1/2", "Firebox 2/2", "Copying to Firebox: ..."), and when both are saved it says "Backup complete · both copies saved", with a short summary for each.
- **Version History and Restore** have a **Primary | Firebox** switch, so you can browse, verify and restore from the Firebox when the primary is gone. The side panel is titled **Firebox History** while you are viewing it. Snapshots record `"firebox": true` in `backup.json`, and the Type row shows "Firebox".
- **Automation:** the scheduled task backs up to the Firebox too (`--firebox`), and its summary and the ready-to-copy command include it.
- **Command line:** `--firebox DIR` / `--no-firebox`. `prune` applies retention to both destinations. To list or verify the Firebox, pass its folder as `--to`.
- macOS: the same Firebox mode, backing up to the primary first and then to the Firebox. Cancelling during the Firebox leg keeps the primary snapshot that is already finished.
- CI: both platforms smoke-test Firebox mode, including a Firebox that cannot be reached.

## 1.6.1
- **Code signing through SignPath.io is on the way.** The project has applied to the SignPath Foundation open-source program (free code signing, certificate by SignPath Foundation). The release build is ready to sign `EntropyBackup.exe` and `EntropyBackupCLI.exe` once the application is approved. **This release is not signed yet.**
- Both executables now carry version information (Properties › Details): product name, description, company, version and copyright.
- Once signing is enabled, the release build signs the executables on GitHub Actions, checks each signature and refuses to publish if signing fails. The README has the code signing policy that SignPath Foundation requires.

## 1.6.0
Automation and integrity features for IT pros, on a new **Automation** page.
- **Scheduled backups**: Off / Hourly / Daily / Weekly at a chosen time, registered as the Windows Task Scheduler task "Entropy Backup". It runs while you are signed in, even with the app closed, and missed runs start as soon as possible. The page shows the next and last run and its result.
- **Command line / silent mode**: new `EntropyBackupCLI.exe` with `backup`, `verify`, `prune`, `list` and `help`, plus options for every setting. The exit codes are 0 OK, 1 warnings, 2 failed, 3 invalid arguments and 4 cancelled. `EntropyBackup.exe <command>` does the same without a window. The page shows a ready-to-copy command.
- **Open and locked files** via the Volume Shadow Copy service (copy-type backup, so other backup tools' history is untouched). This needs administrator rights; without them the backup reads the live files and reports a warning. A schedule with shadow copy on runs with highest privileges.
- **Retention**: keep the newest N snapshots and/or delete snapshots older than N days, applied after each backup or with `prune`. The newest snapshot is always kept.
- **SHA-256 checksums** recorded in `manifest.tsv` (v2; v1 manifests still work). **Verify** a snapshot from Version History, after every backup, or with `verify`. Verification reads from disk, bypassing the cache, and saves `verify.json`. History shows the checksum status, and the Type row notes shadow copies.
- `backup.log` records every finished backup, restore, verification and prune.
- A lock file in the versions folder stops two backups to the same destination from running at once, and interrupted `incomplete_*` folders are cleaned up on the next run.
- Fix: two backups started within the same second no longer fail with "Could not finalize the snapshot".
- Fix: deleting a snapshot (cancel, mirror refresh, retention) no longer clears the read-only flag on hard-linked files that older snapshots share.
- New slogan: "When Entropy is back UP, launch EntropyBackup!....exe", shown in the README and on the About page.
- **New: Entropy Backup for macOS** (macOS 14 or later, including macOS 27; universal). It is a native Swift/SwiftUI app with the same features and snapshot format: incremental hard-linked snapshots with APFS clones, history, restore, excludes, scheduled backups through launchd, retention, SHA-256 checksums and verification, and the same command line and exit codes. It uses Liquid Glass on macOS 26 and later. The About page says "....app".
- GitHub Actions builds and tests both apps on every push (MSVC on Windows, including a real shadow-copy backup; Swift on macOS) and publishes them on a `v*` tag.

## 1.5.0
- New **Section Outline** settings: one switch outlines every section with a two-colour gradient.
  - Pick both colours with the Windows colour picker; a swap button and a live preview bar are included.
  - 6 gradient styles: Horizontal, Vertical, Diagonal, Reverse diagonal, Mirror (A-B-A) and Radial.
  - **Flow across window** stretches one gradient across all sections instead of repeating it in each.
  - **Thickness** and **Glow** sliders.
- Removed the gold glow outline from the banner and the sidebar.
- The data folder path moved from Settings to the About page.
- **Back Up Now** moved to a compact panel at the bottom right, replacing Restore / Explore; the Restore page and the Version History "..." menu cover those functions. The Backup page's options box gets the freed space.
- The About page shows an "AS IS / no warranty" notice; DISCLAIMER.md was added.

## 1.4.0
- Logo: the gold time cube (clock hands on its inner edges) replaces the external drive inside the hexagon, and the sync arrow now flows from the project down into it.
- The header banner layout is now the default. It shows the current page title and subtitle, so it also works as a "you are here" bar. You can still turn it off in Settings > Appearance.

## 1.3.2
- Settings > Appearance: **Glass tint** and **Frost (blur)** sliders update the panels live and are saved. **Reset Glass Look** returns them to the defaults (55% / 100%).
- Settings > Appearance: **Show page header banner** switches to an alternate layout for A/B testing. The page header returns at the same height as Version History, the page panel and Version History share a bottom edge, and the status panel sits next to a compact Restore / Explore panel.

## 1.3.1
- New logo: the project's arrow now flows into a gold backup-target cube. The cube's three inner edges are the hour, minute and second hands, and its outer corners mark 12, 4 and 8 o'clock. The window and taskbar icon use it too.
- Removed the page header; the active page is shown by the sidebar.
- Sidebar, center panel and Version History now share the same top edge. The status panel lines up with Restore / Explore and is taller, with a larger progress ring.

## 1.3.0
UI rebuilt to match the Entropy Glass concept art, and every control now works.

**Look**
- Frameless window with custom minimize / maximize / close, drag regions, resize borders and the native Windows 11 shadow and rounded corners.
- Rendered mountain-dusk backdrop (layered sunlit ridges, snow, clouds, low fog) or any image you choose in Settings.
- Frosted glass panels that blur the backdrop, with gold-edged sidebar and header and soft drop shadows.
- Vector icon set and hexagonal Entropy logo, also used as the window and taskbar icon.
- The whole layout scales with the window, so resizing or maximizing keeps the proportions.
- Dark, borderless input fields with placeholder text, a description character counter and dark scrollbars.

**Backup**
- Backup Type: *Incremental* (reuses unchanged files from the previous snapshot) or *Full Copy* (reads every file from the source again).
- Options: hard links for unchanged files, preserve attributes and timestamps (including creation and folder times), versioned snapshots or a single `mirror_latest`, open the destination when finished.
- Each snapshot gets a `manifest.tsv` (size and timestamp per file), so unchanged files are recognized from the manifest instead of from the stored copies. Older snapshots without a manifest still work.
- Snapshots are built in `incomplete_*` and renamed only when complete. Cancelled or failed runs leave nothing behind.
- Live progress: ring and bar, current file, files scanned / copied / unchanged, size written, elapsed time and estimated time remaining. Cancel works mid-file.
- Locked or unreadable files are skipped and listed in `backup.json` instead of failing the whole backup.
- Excludes are applied during the scan, and the versions folder is skipped automatically if it sits inside the project.
- Long-path (`\?\`) support, and junctions are not followed.
- `backup.json` is now UTF-8 (v3), with type, duration, totals and the list of skipped files.

**Pages**
- *History*: browse every snapshot with full details, open its folder, or jump to Restore.
- *Restore*: restore any version into a folder you pick, with an optional overwrite and a confirmation dialog. Uses the same progress panel and Cancel.
- *Settings*: remember last folders, skip hidden/system files, choose or reset the background image, open the settings folder.
- *Excludes*: edit the patterns (wildcards, path patterns, `#` comments), save them or reset to defaults.
- *About*: version, how snapshots work, and a link to the data folder.
- Version History panel: scrolls with the mouse wheel, highlights the selected version, and its "..." menu offers Refresh, Show All Versions and Open Versions Folder.
- Settings, excludes and `runtime.log` are stored in `%APPDATA%\EntropyBackup`.

**Developer credit**
- The credit stays typeset. To show a real signature, put a transparent `signature.png` next to the exe and it is drawn under the credit.

## 1.1.0
- Rebuilt the native UI around the approved Entropy Glass concept.
- Added layered translucent navy glass panels, pale-gold active states, gradients and refined borders.
- Direct2D render target now uses premultiplied alpha and grayscale anti-aliased DirectWrite text for translucent surfaces.
- Added per-monitor DPI-aware layout handling.
- Added left navigation presentation, project header, real version-history cards, progress area and Restore / Explore panel.
- Preserved the functional From / To / Description workflow and snapshot engine.
- Added Open Versions Folder action.
- Developer credit is typeset only: "Developed by Peter V. Odintsov". No fake signature graphic.
- Preserved v1.0.4 Direct2D factory compatibility fix and diagnostic build.cmd.
