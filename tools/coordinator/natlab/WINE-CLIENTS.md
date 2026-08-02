# Running real game clients in the natlab (wine)

Full 3-player coordinator flow with real `generalszh.exe` clients, each behind
its own simulated NAT. Everything below was proven working 2026-08-01 up to the
LAN game-options lobby with host + 2 joiners punched and joined.

## One-time setup (after reboot: all of /tmp is gone, redo everything)

```bash
cd tools/coordinator/natlab && ./natlab-up.sh          # 3 clients + NATs + inet hop

BASE=/tmp/natlab; sudo mkdir -p $BASE && sudo chown $USER $BASE
for L in A B C; do
  mkdir -p $BASE/upper$L $BASE/work$L $BASE/game$L $BASE/pref$L
  sudo mount -t overlay overlay -o lowerdir=/home/hrich/zrun/game,upperdir=$BASE/upper$L,workdir=$BASE/work$L $BASE/game$L
done

for i in 1 2 3; do Xvfb :9$i -screen 0 1024x768x24 & done

# Wine prefixes: create ON THE HOST first (fast), then FIX THE GOTCHAS:
for L in A B C; do WINEPREFIX=$BASE/pref$L wineboot -i; done
# Registry install paths (32-bit view! the exe is 32-bit; without /reg:32 the
# keys land in the 64-bit hive and the game never sees them). Missing keys =
# "Be 1337! Go install Generals!" assert in releaselog builds.
for L in A B C; do
  WINEPREFIX=$BASE/pref$L wine reg add "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Generals" /v InstallPath /t REG_SZ /d "Z:\\tmp\\natlab\\game$L\\" /f /reg:32
  WINEPREFIX=$BASE/pref$L wine reg add "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour" /v InstallPath /t REG_SZ /d "Z:\\tmp\\natlab\\game$L\\" /f /reg:32
done
for L in A B C; do WINEPREFIX=$BASE/pref$L wineserver -k; done   # MUST kill: server must respawn inside the netns
D=~/Documents/"Command and Conquer Generals Zero Hour Data"
for L in A B C; do
  U=$BASE/pref$L/drive_c/users/hrich
  rm -f "$U/Documents"                       # symlink to real ~/Documents = all clients share data!
  mkdir -p "$U/Documents/Command and Conquer Generals Zero Hour Data"
  cp -r "$D/Maps" "$U/Documents/Command and Conquer Generals Zero Hour Data/"
  cp "$D/options.ini" "$D/Skirmish.ini" "$U/Documents/Command and Conquer Generals Zero Hour Data/"
  sed -i 's/^Resolution = .*/Resolution = 1024 768/' "$U/Documents/Command and Conquer Generals Zero Hour Data/options.ini"  # else 4K
done

# Coordinator (cncstats build; 27500 clashes with passimd on Arch):
PORT=18080 CNC_AUTH_REQUIRED=false COORD_TCP_ADDR=:37500 COORD_UDP_ADDR=:37501 ./cncstats -no-stores &
```

## Deploy a freshly built exe

```bash
for L in A B C; do cp build/docker-vc6/GeneralsMD/generalszh.exe /tmp/natlab/game$L/generalszh.exe; done
```

## Launch (host, then joiners; wineserver inherits the netns)

```bash
BASE=/tmp/natlab
sudo ip netns exec clientA sudo -u $USER env DISPLAY=:91 WINEPREFIX=$BASE/prefA WINEDEBUG=-all WINEDLLOVERRIDES=d3d8=b \
  sh -c "cd $BASE/gameA && nohup wine explorer /desktop=natA,1024x768 ./generalszh.exe -win -quickstart -noaudio -mod 'Z:\\tmp\\natlab\\gameA\\Zulu_152.big' \
  -coordhost 10.99.0.1:37500 -coordnick alice -coordautohost labgame -coordautostart 3 -coordpunchttl 2 > $BASE/alice.log 2>&1 &"
# joiners: clientB/C, DISPLAY :92/:93, prefB/C, gameB/C, -coordnick bob|carol -coordautojoin labgame
```

- `-mod 'Z:\...\gameX\Zulu_152.big'` is REQUIRED. Production launches through
  ZuluLauncher which passes `-mod Zulu.big`; without it the plain archive scan
  loads bigs alphabetically FIRST-WINS, so retail WindowZH.big shadows the
  fork's updated .wnd files. Symptom: fork-added gadgets (ButtonRandomize) are
  missing -> every JOINER crashed in LanGameOptionsMenuInit ("Uncaught
  Exception in GameEngine::update" 1s after handoff) while the host looked
  fine. Joiner-branch derefs are now null-guarded, but the UI is still wrong
  without -mod.
- Host default map: with no Network.ini "Map" pref the host now picks the
  first OFFICIAL map. To pin a specific one, e.g. 4-player Alpine Assault:
  `printf 'Map = maps_5Calpine_20assault_5Calpine_20assault_2Emap\n' >> <prefA user data>/Network.ini`
  (QuotedPrintable: `_XX` hex per non-alphanumeric byte.)

## Where to look

- Per-client `ReleaseLog.txt`:
  `$BASE/pref<X>/drive_c/users/hrich/Documents/Command and Conquer Generals Zero Hour Data/ReleaseLog.txt`
  — coordinator connect/state/handoff/teardown breadcrumbs are release-logged.
- Coordinator: `curl -s localhost:18080/coordinator/status` (sessions, games, punch_ok/fail).
- Screenshots: `DISPLAY=:91 import -window root /tmp/shot.png`.

## Observer (relay) test

A 4th client (dave) can watch the in-progress game through the coordinator
relay. Gotchas discovered building this:

- One netns supports ONE game client: the coordinator binds UDP 8086/8088 at
  connect, so a second client in the same netns fails with "udp bind failed
  on port 8086". Run the observer from the HOST netns (the relay is pure
  outbound TCP, NAT plays no part) or add a clientD netns.
- prefD setup: same wineboot + /reg:32 registry keys + per-prefix Documents
  dance as A-C, plus its own Xvfb (:94). Verify the display exists before
  launching (`DISPLAY=:94 xdpyinfo`): a missing Xvfb surfaces as a bogus
  "Please make sure you have DirectX 8.1 or higher installed" crash.
- Launch: same flags as a joiner (`-coordnick dave -coordautojoin labgame`).
  If the game is already in progress, auto-join automatically becomes
  auto-observe (list row shows IN PROGRESS).
- Watch `ObserverLog.txt` in dave's user data dir: relay adopt, buffering,
  then `Recorder::update tick mode=5` lines = live playback running.
- Server side: `grep relay /tmp/natlab/cncstats.log` shows attach/pair.

## Hard-won gotchas

- `wine explorer /desktop=...` is REQUIRED: bare Xvfb has no WM, the game gets
  no input focus; virtual desktop fixes it.
- `WINEDLLOVERRIDES=d3d8=b`: the install dir ships a native d3d8.dll wrapper
  that renders everything magenta under wine.
- Wine sockets are created by wineserver: if wineserver is running in the host
  netns, ALL game sockets land there. Kill wineserver per prefix, then launch
  the game inside the netns so its wineserver inherits it.
- `-quickstart` (= -nologo -noshellmap -noShellAnim) skips intro movies, which
  otherwise never end without input.
- The -coord* flags MUST be in the release (unconditional) section of the
  CommandLine.cpp param table, not the RTS_DEBUG block.
- pkill -f <pattern> from the agent shell kills its own wrapper (the pattern
  matches the shell's cmdline): use plain `pkill generalszh` (comm match).
- The lobby's purple grid background at a 4K window size looks like a "missing
  texture" screen when viewed through a 1024x768 desktop; check options.ini
  Resolution before chasing rendering ghosts.
