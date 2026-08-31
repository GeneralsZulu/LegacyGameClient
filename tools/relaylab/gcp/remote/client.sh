#!/bin/bash
# client.sh <nat-type> <coordIP> <nick> [extra game flags...]
#
# Runs ON a relaylab client VM (baked by image.sh). Applies the in-VM NAT
# shape when asked (cloud-cone/cloud-sym mean "Cloud NAT is the NAT; run in
# the root namespace"), then launches the game unattended with the
# -coordauto* flags passed through. Logs to /tmp/game.log; ReleaseLog under
# /opt/zh-pref.
set -euo pipefail
TYPE=${1:?nat type}
COORD_IP=${2:?coord ip}
NICK=${3:?nick}
shift 3

cd "$(dirname "$0")"

pkill generalszh 2>/dev/null || true
WINEPREFIX=/opt/zh-pref wineserver -k 2>/dev/null || true
sleep 1

if ! DISPLAY=:91 xdpyinfo >/dev/null 2>&1; then
  Xvfb :91 -screen 0 1024x768x24 >/dev/null 2>&1 &
  sleep 1
fi

# User-data setup on EVERY launch, never trusted to bake-time state: the
# game rewrites Network.ini on exit (wiping the map pin), and a partially
# failed bake left the prefix Documents empty on the first fleet image.
D="/opt/zh-pref/drive_c/users/$USER/Documents/Command and Conquer Generals Zero Hour Data"
mkdir -p "$D"
[ -d "$D/Maps" ] || cp -r /opt/zh-userdata/Maps "$D/" 2>/dev/null || true
cp /opt/zh-userdata/options.ini /opt/zh-userdata/Skirmish.ini "$D/" 2>/dev/null || true
sed -i 's/^Resolution = .*/Resolution = 1024 768/' "$D/options.ini" 2>/dev/null || true
# Official 8-player map (Death Valley), QuotedPrintable.
printf 'Map = maps_5Cdeath_20valley_5Cdeath_20valley_2Emap\n' > "$D/Network.ini"
rm -f "$D/ReleaseLog.txt"

NSWRAP=()
case "$TYPE" in
  cloud-cone|cloud-sym)
    ;; # Cloud NAT provides the shape; run in the root namespace
  *)
    ./vm-nat.sh "$TYPE" "$COORD_IP"
    NSWRAP=(sudo ip netns exec gns sudo -u "$USER" env)
    ;;
esac

# Real internet: default punch TTL (no -coordpunchttl override).
"${NSWRAP[@]}" env DISPLAY=:91 WINEPREFIX=/opt/zh-pref WINEDEBUG=-all WINEDLLOVERRIDES=d3d8=b \
  sh -c "cd /opt/zh && nohup wine explorer /desktop=lab,1024x768 ./generalszh.exe \
    -win -quickstart -noaudio -mod 'Z:\\opt\\zh\\Zulu.big' \
    -coordhost $COORD_IP:37500 -coordnick $NICK $* \
    > /tmp/game.log 2>&1 &"
echo "client.sh: launched $NICK behind $TYPE"
