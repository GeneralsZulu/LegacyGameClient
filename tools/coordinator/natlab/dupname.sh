#!/bin/bash
# dupname.sh: duplicate-name join recovery. Host and guest both use the nick
# "alice", so the host denies the guest with RET_DUPLICATE_NAME after the
# coordinator has already handed off to TheLAN. Asserts the guest rebuilds its
# coordinator session (back to READY with a games list) instead of wedging in
# post-handoff limbo, which used to require leaving online play entirely.
set -uo pipefail

NATLAB="$(cd "$(dirname "$0")" && pwd)"
COORD_DIR="$(cd "$NATLAB/.." && pwd)"
REPO="$(cd "$COORD_DIR/../.." && pwd)"
RUN="$REPO/scratchpad/relaylab-runs/$(date +%Y%m%d-%H%M%S)-DUPNAME"
COORD_IP=10.99.0.1
COORD_TCP=37500
COORD_UDP=37501
COORD_STATUS=37502

source "$NATLAB/lib/winefleet.sh"
mkdir -p "$RUN"
exec > >(tee "$RUN/run.log") 2>&1
say() { echo "[$(date +%H:%M:%S)] $*"; }

say "building coord"
(cd "$COORD_DIR" && go build -o "$NATLAB/bin/coord" ./cmd/coord) || exit 1
fleet_kill
pkill -x coord 2>/dev/null || true
pkill -x cncstats 2>/dev/null || true
sleep 0.5
"$NATLAB/bin/coord" -tcp :$COORD_TCP -udp :$COORD_UDP -udp2 :37503 -status :$COORD_STATUS > "$RUN/coord.log" 2>&1 &

say "natlab up (2 clients, both prc)"
NCLIENTS=2 "$NATLAB/natlab-up.sh" > "$RUN/natlab.log"
for L in A B; do "$NATLAB/nat-shape.sh" $L prc $COORD_IP $COORD_UDP >> "$RUN/natlab.log"; done

fleet_setup 2
fleet_deploy 2

GAME="dupname-$RANDOM"
say "launching host alice"
fleet_launch 1 alice -coordautohost "$GAME" -coordautostart 4
sleep 25
say "launching guest ALSO named alice"
fleet_launch 2 alice -coordautojoin "$GAME" -coordautostart 4
sleep 60

for i in 1 2; do DISPLAY=:9$i import -window root "$RUN/client$i.png" 2>/dev/null || true; done
cp "$(fleet_releaselog_i 1)" "$RUN/host-ReleaseLog.txt" 2>/dev/null
cp "$(fleet_releaselog_i 2)" "$RUN/guest-ReleaseLog.txt" 2>/dev/null

echo
echo "=== HOST: duplicate-name deny ==="
grep -aE "duplicate|handoff|Coordinator (handoff|host)" "$RUN/host-ReleaseLog.txt" | tail -10
echo
echo "=== GUEST: handoff -> refusal -> rebuild ==="
grep -aE "Coordinator|refused" "$RUN/guest-ReleaseLog.txt" | tail -25

echo
GUEST_LOG="$RUN/guest-ReleaseLog.txt"
FAIL=0
grep -qa "Coordinator handoff to LAN done" "$GUEST_LOG" || { echo "FAIL: guest never reached the handoff"; FAIL=1; }
grep -qa "join refused after handoff; rebuilding the session" "$GUEST_LOG" || { echo "FAIL: recovery never fired"; FAIL=1; }
# The rebuild must climb all the way back to READY (state 4) AFTER the refusal.
sed -n '/join refused after handoff/,$p' "$GUEST_LOG" | grep -qa "state 3 -> 4" \
  || { echo "FAIL: session never returned to READY after the rebuild"; FAIL=1; }
[ $FAIL -eq 0 ] && echo "PASS: guest recovered to a browsable lobby without leaving online play"
echo "artifacts: $RUN"
exit $FAIL
