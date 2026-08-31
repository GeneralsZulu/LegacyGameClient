#!/bin/bash
# winefleet.sh: scripted form of WINE-CLIENTS.md. Source this; it provides
#   fleet_setup N        overlays, Xvfb, wine prefixes for clients 1..N
#   fleet_deploy N       copy the freshly built exe + Zulu.big into overlays
#   fleet_launch L NICK ROLEARGS...   launch one client inside clientL netns
#   fleet_kill           stop every game client and per-prefix wineserver
#   fleet_releaselog L   echo the path of client L's ReleaseLog.txt
# Assumes natlab-up.sh has run. GAME_LOWER is the pristine game install.
# All proven-by-hand steps and gotchas come from WINE-CLIENTS.md; change
# that file first, this second.

FLEET_BASE=${FLEET_BASE:-/tmp/natlab}
GAME_LOWER=${GAME_LOWER:-/home/hrich/zrun/game}
FLEET_LETTERS=(A B C D E F G H)
COORD_ADDR=${COORD_ADDR:-10.99.0.1:37500}

fleet_letter() { echo "${FLEET_LETTERS[$(( $1 - 1 ))]}"; }

fleet_setup() {
  local N=$1 i L
  sudo mkdir -p "$FLEET_BASE" && sudo chown "$USER" "$FLEET_BASE"
  for ((i=1; i<=N; i++)); do
    L=$(fleet_letter $i)
    mkdir -p "$FLEET_BASE"/{upper$L,work$L,game$L,pref$L}
    if ! mountpoint -q "$FLEET_BASE/game$L"; then
      sudo mount -t overlay overlay \
        -o lowerdir="$GAME_LOWER",upperdir="$FLEET_BASE/upper$L",workdir="$FLEET_BASE/work$L" \
        "$FLEET_BASE/game$L"
    fi
    # Only ONE Zulu big may exist: the client auto-loads Zulu*.big, and the
    # pristine install carries stale versioned copies.
    rm -f "$FLEET_BASE/game$L"/Zulu_*.big
    if ! DISPLAY=:9$i xdpyinfo >/dev/null 2>&1; then
      Xvfb :9$i -screen 0 1024x768x24 >/dev/null 2>&1 &
      sleep 0.5
    fi
    if [ ! -d "$FLEET_BASE/pref$L/drive_c" ]; then
      WINEPREFIX="$FLEET_BASE/pref$L" wineboot -i >/dev/null 2>&1
      WINEPREFIX="$FLEET_BASE/pref$L" wine reg add \
        "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Generals" \
        /v InstallPath /t REG_SZ /d "Z:\\tmp\\natlab\\game$L\\" /f /reg:32 >/dev/null 2>&1
      WINEPREFIX="$FLEET_BASE/pref$L" wine reg add \
        "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour" \
        /v InstallPath /t REG_SZ /d "Z:\\tmp\\natlab\\game$L\\" /f /reg:32 >/dev/null 2>&1
      WINEPREFIX="$FLEET_BASE/pref$L" wineserver -k >/dev/null 2>&1 || true
      local U="$FLEET_BASE/pref$L/drive_c/users/$USER"
      local D="$HOME/Documents/Command and Conquer Generals Zero Hour Data"
      rm -f "$U/Documents"   # symlink to the real ~/Documents = shared state
      mkdir -p "$U/Documents/Command and Conquer Generals Zero Hour Data"
      cp -r "$D/Maps" "$U/Documents/Command and Conquer Generals Zero Hour Data/" 2>/dev/null || true
      cp "$D/options.ini" "$D/Skirmish.ini" "$U/Documents/Command and Conquer Generals Zero Hour Data/" 2>/dev/null || true
      sed -i 's/^Resolution = .*/Resolution = 1024 768/' \
        "$U/Documents/Command and Conquer Generals Zero Hour Data/options.ini" 2>/dev/null || true
    fi
  done
}

fleet_deploy() {
  local N=$1 i L
  local REPO
  REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
  for ((i=1; i<=N; i++)); do
    L=$(fleet_letter $i)
    cp "$REPO/build/docker-vc6/GeneralsMD/generalszh.exe" "$FLEET_BASE/game$L/generalszh.exe"
    cp "$REPO/build/installer-tmp/Zulu.big" "$FLEET_BASE/game$L/Zulu.big" 2>/dev/null || true
    rm -f "$FLEET_BASE/game$L"/Zulu_*.big
    # Fresh logs per scenario.
    rm -f "$(fleet_releaselog_i $i)" 2>/dev/null || true
    # Pin the host map to Death Valley: OFFICIAL and 8 players, so every
    # 2-8 client scenario fits its slots. (Alpine Assault turned out to be
    # a 2-slot map: joiner #2 was denied RET_GAME_FULL and the T1-BASELINE
    # start gate could never fire.) QuotedPrintable per WINE-CLIENTS.md.
    printf 'Map = maps_5Cdeath_20valley_5Cdeath_20valley_2Emap\n' \
      > "$FLEET_BASE/pref$L/drive_c/users/$USER/Documents/Command and Conquer Generals Zero Hour Data/Network.ini"
  done
}

fleet_releaselog_i() {
  local L
  L=$(fleet_letter $1)
  echo "$FLEET_BASE/pref$L/drive_c/users/$USER/Documents/Command and Conquer Generals Zero Hour Data/ReleaseLog.txt"
}

# fleet_launch <index> <nick> <extra flags...>  (extra: -coordautohost NAME
# -coordautostart N | -coordautojoin NAME -coordautostart N, -norelay, ...)
# FLEET_NETNS_OVERRIDE (a letter) launches this client inside ANOTHER
# client's netns: the same-public-IP household pattern (ephemeral-port
# fallback handles the port clash, per WINE-CLIENTS.md's eve).
fleet_launch() {
  local i=$1 NICK=$2; shift 2
  local L NSL
  L=$(fleet_letter $i)
  NSL=${FLEET_NETNS_OVERRIDE:-$L}
  # wineserver must respawn INSIDE the netns or every socket lands on the host.
  WINEPREFIX="$FLEET_BASE/pref$L" wineserver -k >/dev/null 2>&1 || true
  sleep 0.3
  sudo ip netns exec client$NSL sudo -u "$USER" env \
    DISPLAY=:9$i WINEPREFIX="$FLEET_BASE/pref$L" WINEDEBUG=-all WINEDLLOVERRIDES=d3d8=b \
    sh -c "cd $FLEET_BASE/game$L && nohup wine explorer /desktop=nat$L,1024x768 ./generalszh.exe \
      -win -quickstart -noaudio -mod 'Z:\\tmp\\natlab\\game$L\\Zulu.big' \
      -coordhost $COORD_ADDR -coordnick $NICK -coordpunchttl 2 $* \
      > $FLEET_BASE/$NICK.log 2>&1 &"
}

fleet_kill() {
  pkill generalszh 2>/dev/null || true
  sleep 1
  local L
  for L in "${FLEET_LETTERS[@]}"; do
    [ -d "$FLEET_BASE/pref$L" ] && WINEPREFIX="$FLEET_BASE/pref$L" wineserver -k >/dev/null 2>&1 || true
  done
}
