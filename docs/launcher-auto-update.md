# Launcher Auto-Update

## What it is

`ZuluLauncher.exe` is a small WIN32 process that ships alongside
`generalszh.exe` and becomes the new start-menu / desktop shortcut
target. On every cold start it:

1. Reads the installed game's `VS_VERSION_INFO` to get the
   currently installed Zulu version.
2. Fetches `https://storage.googleapis.com/zulu-installer/latest.json`
   to see what's been published.
3. If the published version differs from what's installed — in
   **either** direction — prompts the user, downloads that installer
   to `%TEMP%`, runs it silently (`/S /D=<install dir>`), waits for
   it to finish, then continues into `launchGame()` with the original
   launcher argv intact.
4. If versions match, the fetch fails, or the user declines, hands
   off directly to `generalszh.exe` with the launcher's argv.

## Rolling back a bad release

`latest.json` is authoritative, not a high-water mark. To pull
everyone off a broken build, republish `latest.json` pointing
`version` and `url` at the previous installer. The next launch of
every client that already took the bad build sees a version
mismatch and offers the older installer, with the prompt reading
"Zulu has been rolled back to an earlier release." instead of "A
newer Zulu release is available."

The installer has no version gate of its own — it overwrites in
place and rewrites the `DisplayVersion` uninstall key — so a
downgrade install is the same operation as an upgrade.

Two things this costs us. A release-variant build made locally that
is ahead of `latest.json` now gets a downgrade prompt on every
launch instead of being silently left alone; decline it, or use a
dev build, which reads `latest-dev.json` and gates on the exe hash
(below). And a rollback only reaches players who actually run the
launcher, so it is not instant.

## Dev builds gate on a hash, not a version

Dev builds don't bump semver between rebuilds, so step 3's `>`
comparison would never fire for them. Dev launchers (`latest-dev.json`)
instead compare hashes: the manifest's **`exe_sha256`** against the
SHA-256 of the installed `generalszh_zulu.exe`.

It has to be `exe_sha256`, not `sha256`. Those are two different
things and mixing them up is a bug we have already shipped once:

| field | hash of |
|---|---|
| `sha256` | the installer, `Zulu-Installer-Dev.exe` |
| `exe_sha256` | the game exe that installer lays down |

The gate originally compared the installed game exe against `sha256`
— the installer's hash. Two different files, so it never matched and
every single dev launch announced a new build. `make installer-dev`
now publishes both, taking `exe_sha256` from the staged exe before
the staging directory is cleaned.

A manifest with no `exe_sha256` (published before the field existed)
leaves the gate closed rather than prompting forever; the next
`make installer-dev` republishes it and updates resume. Note that a
launcher installed *before* this fix still reads the old field, so it
will prompt one last time — accepting that update replaces the
launcher and stops the loop.

## Why

Zulu ships often. Without auto-update, every player needs to
manually visit a download page when they want the new client,
which means LAN sessions split between a host running v1.1.5 and
players still on v1.1.3 — the exeCRC check then refuses to let
them play together. The launcher closes that gap: hit play, get
the latest, join the lobby.

The argv preservation work (PR — `2d129251f`) fixes a subtle
problem from the original launcher commit. The first version
handed control to the NSIS installer's silent post-install `Exec`
line, which used the start-menu shortcut's hardcoded `LAUNCHARGS`
("-mod Zulu.big"). Any extra args the user passed on the
launcher command line were dropped. The fix has the calling
launcher wait for the elevated installer (`ShellExecuteEx` with
`SEE_MASK_NOCLOSEPROCESS`), then fall through to the normal
`launchGame` path with the launcher's original argv. NSIS's silent
post-install `Exec` is removed so it doesn't race the calling
launcher.

## Code surface

- `launcher/ZuluLauncher.cpp` — ~380 lines. Manifest, version
  compare, JSON parsing (minimal — only the `version` and `url`
  fields are read), WinHTTP download with progress, ShellExecuteEx
  wait-handoff, fallthrough to `CreateProcess(generalszh.exe, ...)`
  with original argv.
- `launcher/CMakeLists.txt` / `launcher/ZuluLauncher.rc.in` /
  `launcher/ZuluLauncher.manifest` — build, version-resource, and
  manifest plumbing.
- `installer/Zulu.nsi` — removes the silent post-install `Exec`
  and adds the manifest-publishing step.
- `Makefile` — `installer-release` target publishes the manifest
  with no-cache headers; downloads pinned to the
  `zulu-installer` GCS bucket as defense-in-depth.
- `GeneralsMD/Code/Main/RTS.RC` and the build glue around it pull
  `APPVERSION` from `installer/Zulu.nsi` so the EXE
  `VS_VERSION_INFO` and the installed-version check always agree.

## Concerns

- **Backwards compatibility.** Players upgrading from a pre-launcher
  build to a launcher-shipping build need to run the new installer
  once (shortcut still points at `generalszh.exe`). After the first
  install, the shortcut points at `ZuluLauncher.exe` and the
  manifest check kicks in.
- **Update channel risk.** All installs follow the same
  `latest.json` channel. There is no opt-out and no per-user
  channel selection. Dev builds protect themselves by having a
  version higher than the published manifest.
- **Mismatch / replay / save.** None — the launcher is a separate
  process and never touches game state.
- **Bucket lock-in.** The download URL is currently hardcoded to
  the `zulu-installer` GCS bucket. Moving providers would require
  a new launcher build to ship before the migration. Worth noting
  rather than urgent.
- **Failure-to-launch path.** If `latest.json` fetch fails (offline
  / DNS / 503), the launcher logs and falls through to launching
  the installed binary. The player isn't blocked.
