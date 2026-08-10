# Replay data versions / Replay Theater

Playing back an old replay needs the game data that replay was recorded
against, not just an engine that behaves the way the old engine did. This
doc covers why, and the machinery that makes it work.

## Why data matters

A `.rep` is a command log. Playback re-runs the simulation and replays the
commands into it, so anything that changes what the simulation does with a
command breaks fidelity. There are two halves to that:

- **Engine behavior.** Handled by the replay determinism epochs in
  `Recorder.h`: each divergent code path is gated on the epoch derived from
  the replay's recorded version, so a modern binary reproduces the old
  behavior bit-exactly.
- **Data.** Not gateable. If `Weapon.ini` says a shell does 60 damage now
  and said 50 then, the sim diverges on the first shot and no amount of
  engine gating helps.

Zulu's data lives in `Zulu.big` and has changed across releases. It changed
drastically in 1.5.5, which folded in the TheSuperHackers community balance
patch. That patch does not just retune values: it blanks every retail
`Data/INI/Object/*.ini` and redefines all objects across 2,146 nested files.
That reorders NameKey and ThingTemplate registration wholesale, which is the
same class of problem as the 1.5.2 `staleRefs` regression, on top of the
balance changes themselves.

So from 1.5.5 on, no pre-1.5.5 replay can re-simulate correctly against the
installed data.

## What we do about it

Ship every historical `Zulu.big` and mount the matching one.

The choice has to be made before the process starts.
`ArchiveFileSystem::loadMods()` runs before any INI is parsed, and once
parsing is done the engine has handed out `ThingTemplate` and `Upgrade`
pointers everywhere; there is no swapping data under a running game. That is
why this is a relaunch and not a button in the replay menu.

### Pieces

| Piece | What it does |
|---|---|
| `installer/replay_data_versions.csv` | The map: replay version (or iniCRC) to archive, plus the git ref each archive is built from. Ships to `<install dir>\ReplayData\versions.csv`. |
| `scripts/build_replay_bigs.py` | Rebuilds each archive from its release's `assets/` tree straight out of git history. Run by `make replay-bigs`. |
| `ZuluLauncher.exe -replaytheater` | Replay Theater: picks a replay, reads its header, resolves the archive, launches the game with it, waits, repeats. Runs as instance 1 so the player's own `Options.ini` (and therefore their resolution) is used - see the note below. |
| `-watchReplay <file>` | Plays one replay in the normal game window. Distinct from `-replay`, which runs the headless batch re-simulation and exits. |
| `-replaytheater` | Marks a process as existing only to play that replay: it quits when playback ends rather than dropping to the shell. |
| `ReplayDataVersions` (engine) | Same lookup inside the game, used by the replay menu to detect "this needs other data" and point at Replay Theater. |

The archives are **not** checked in. They are ~95% identical to one another,
so storing 16 of them would add ~71 MB of near-duplicate binaries to the
repo for something git can regenerate exactly.

### Why the launcher duplicates the parsing

`ZuluLauncher` links none of the engine, which is what keeps it small and
lets it start before the game does its heavy init. It therefore has its own
small reader for the replay header and for `versions.csv`. Both readers are
straightforward and the formats are frozen, so the duplication is cheap; the
alternative is linking the engine into the launcher, which is not.

### Cost

71 MB installed. About 470 KB of installer, because the archives are nearly
identical and solid LZMA collapses them - but only with
`SetCompressorDictSize 64` in `Zulu.nsi`. At NSIS's 8 MB default they do not
all fit in the window at once and cost ~2.9 MB instead.

### Where it lives, and why the fence matters

`ReplayData` sits next to the game exe in the install directory - **not**
under a user's Documents. It is program data, one set per machine; under
Documents it was invisible to every other account on the box, so Replay
Theater came up empty for anyone who had not personally run the installer.
The launcher therefore passes an **absolute** `-mod` path, because a relative
one is resolved against the user data directory (`parseMod`).

That makes the sweep guard load-bearing rather than belt-and-braces: the
install-dir `*.big` scan recurses, so without it all 16 archives would be
mounted at once on every launch. `isInReplayDataFolder()` in
`ArchiveFileSystem.cpp` skips the folder, and both the Win32 and Std BIG
backends call it. It matches `ReplayData` only as a whole path component, so
`ReplayDataExtra\x.big` and `MyReplayData\x.big` are still loaded normally.

The installer creates its shortcuts in the all-users context for the same
reason - a machine-wide install should be visible to every account, not just
whoever ran setup.

## Adding a release

1. Add a `version` row to `installer/replay_data_versions.csv` for the
   release you just cut, pointing at a new `Zulu_v<x.y.z>.big` and the ref it
   was built from.
2. Point the *new* release's own row at `@default`.
3. `make replay-bigs` and check the archive appears.

If a release shipped data identical to an earlier one, point both rows at
the same archive rather than building a duplicate - `1.2.8` reuses `1.2.7`,
`1.5.4` reuses `1.5.2`, and so on.

### Verifying a row

Build the archive from its `git_ref` and compare against the one inside that
release's shipped installer (`7z x Zulu_Setup_v<x.y.z>.exe`). They are not
byte-identical - entry order differs from the retired per-file `big add`
loop - but the file set and every file's contents must match. Order is inert
because the engine enumerates archive contents through sorted `std::map` /
`std::set` (`ArchiveFileSystem.h`, `FilenameList`). Confirmed this way for
1.4.0 and 1.5.2.

The `1.0.0`-`1.3.3` refs are the assets tree oids from
`tools/replay-reparse/VERSION_MAP.csv`, identified by matching the loaded
iniCRC against the iniCRC recorded in real replays from those releases.
Prefer that empirical evidence over "the commit that bumped APPVERSION": the
two disagree for some releases, because a release was not always built from
its bump commit.

## Gotchas already paid for

- **Do not call `ClientInstance::skipPrimaryInstance()` for `-watchReplay`.**
  Instance ids above 1 read `Options_Instance<NN>.ini` instead of
  `Options.ini` (`OptionPreferences.cpp`), so a forced secondary id silently
  ignores the player's display settings and opens the replay at the default
  800x600. `setMultiInstance(TRUE)` alone is right: it still allows running
  alongside a live client, and the index only steps up if one is running.
- **Do not start playback from `GameEngine::init()`.** That function ends
  with `resetSubsystems()`, which tears down a game started inline: the map
  survives but the player list does not, so you get a loaded map with nobody
  on it and a `Match start` line reading `multiplayer=0 crcInterval=0`.
  Playback starts from `Shell::showShell()` on the first frame of the main
  loop instead - the same moment the replay menu would have started it. The
  `.map` command-line path dodges the same trap by posting `MSG_NEW_GAME`
  rather than starting a game inline.
- **"Version not in the map" and "map file missing" are different failures.**
  The launcher reports them separately and names the path it consulted;
  collapsing them into one message cost a debugging round trip.

## Known gaps

- **1.2.5** has no clear git ref and no archive. A 1.2.5 replay resolves to
  nothing; Replay Theater asks whether to watch it on current data anyway.
- **1.2.3** was never released as an installer, but its assets tree is known,
  so the archive is built.
- **Dev builds** report a non-semantic version like `Version 1.04` and are
  matched on iniCRC instead. Only the four dev iniCRCs already identified in
  `VERSION_MAP.csv` are mapped.
- **1.5.5 replays** watched on 1.5.6+ rely on 1.5.6 shipping the same assets
  as 1.5.5. If that stops being true, give 1.5.5 its own archive row.

## The 1.5.4 / 1.5.5 mislabelling

Worth recording, because it is what surfaced all of this.

1.5.4 was cut at `9e68b663e` on 2026-08-04. The commit that added the
stable-upgrade-id change and the community patch (`cf7f96285`, 2026-08-08)
landed *after* it, and bumped `APPVERSION` straight from 1.5.2 to 1.5.4 -
so it labelled its own epoch `V154` and gated it at "1.5.4+". But 1.5.4 had
already shipped without any of it, and the changes actually first shipped in
1.5.5.

The result: a real 1.5.4 replay played on 1.5.5 got the *new* engine
behavior applied to it, and desynced. Corrected in 1.5.6 - the epoch is now
`REPLAY_EPOCH_V155`, gated at 1.5.5+, and 1.3.0-1.5.4 map to `V130`. There
was never a 1.5.3.

Two lessons baked into the process now: bump `APPVERSION` in its own commit
at the point a release is cut, and name an epoch after the version it will
actually ship in, not the version currently in the tree.
