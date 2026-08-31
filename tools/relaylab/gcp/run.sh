#!/bin/bash
# run.sh [T2-FULL8|T2-HOST-BAD] [soak_seconds]
#
# Tier 2 entry point: one 8-player match across the provisioned fleet with
# a full per-pair prediction diff. T2-FULL8 hosts on c1 (cone, us-west);
# T2-HOST-BAD hosts on c6 (blk, EU): the worst-case host, every host pair
# relayed across an ocean. Collects ReleaseLogs, coordinator log and status
# polls into scratchpad/relaylab-runs/, then verifies:
#   - every predicted-relay pair shows relay engagement on both sides
#   - every predicted-direct pair shows none
#   - NETPATH lines prove sustained in-game traffic on all 8 clients
#   - the coordinator forwarded packets (and only for relay scenarios)
set -uo pipefail
cd "$(dirname "$0")" && source ./env.sh

SCEN=${1:-T2-FULL8}
SOAK=${2:-600}
case "$SCEN" in
  T2-FULL8)    HOSTC=c1;;
  T2-HOST-BAD) HOSTC=c6;;
  *) echo "unknown scenario $SCEN"; exit 1;;
esac

REPO="$(cd ../../.. && pwd)"
RUN="$REPO/scratchpad/relaylab-runs/$(date +%Y%m%d-%H%M%S)-$SCEN"
mkdir -p "$RUN"
exec > >(tee "$RUN/run.log") 2>&1
say() { echo "[$(date +%H:%M:%S)] $*"; }

ssh_c() { local NAME=$1 ZONE=$2 NET=$3; shift 3
  local IAP=""; [ "$NET" != relaylab-pub ] && IAP="--tunnel-through-iap"
  $GC compute ssh "relaylab-$NAME" --zone="$ZONE" $IAP --command="$*"; }
scp_from() { local NAME=$1 ZONE=$2 NET=$3 SRC=$4 DST=$5
  local IAP=""; [ "$NET" != relaylab-pub ] && IAP="--tunnel-through-iap"
  $GC compute scp --zone="$ZONE" $IAP "relaylab-$NAME:$SRC" "$DST"; }

say "starting the coordinator"
(cd "$REPO/tools/coordinator" && GOOS=linux GOARCH=amd64 go build -o /tmp/relaylab-coord ./cmd/coord)
# Kill any running coordinator BEFORE the copy: scp cannot overwrite a
# busy binary (text file busy), which silently shipped a stale build once.
ssh_c coord $COORD_ZONE relaylab-pub 'pkill -x coord 2>/dev/null; sleep 1; true' >/dev/null 2>&1 || true
$GC compute scp --zone=$COORD_ZONE /tmp/relaylab-coord relaylab-coord:coord >/dev/null
ssh_c coord $COORD_ZONE relaylab-pub \
  'pkill -x coord 2>/dev/null; sleep 1; chmod +x ~/coord; nohup ~/coord -tcp :'"$COORD_TCP"' -udp :'"$COORD_UDP"' -udp2 :37503 -status :'"$COORD_STATUS"' > ~/coord.log 2>&1 & sleep 1; pgrep -x coord'
COORD_IP=$($GC compute instances describe relaylab-coord --zone=$COORD_ZONE \
  --format='value(networkInterfaces[0].accessConfigs[0].natIP)')
say "coordinator at $COORD_IP"
curl -s --max-time 5 "http://$COORD_IP:$COORD_STATUS/status" > "$RUN/status-0.json" || { say "status endpoint unreachable"; exit 1; }

say "launching clients (host $HOSTC first)"
launch() { # launch <cname> <hostflag...>
  local row; row=$(client_row "$1"); set -- $row "${@:2}"
  local C=$1 ZONE=$2 NET=$3 NAT=$4; shift 4
  ssh_c "$C" "$ZONE" "$NET" "bash ~/client.sh $NAT $COORD_IP $C $*"
}
launch $HOSTC -coordautohost cloudgame -coordautostart 8
sleep 25
for row in "${CLIENTS[@]}"; do
  set -- $row
  [ "$1" = "$HOSTC" ] && continue
  launch "$1" -coordautojoin cloudgame -coordautostart 8
  sleep 4
done

say "waiting for match start"
DEADLINE=$(( $(date +%s) + 480 ))
STARTED=0
while [ "$(date +%s)" -lt $DEADLINE ]; do
  if curl -s --max-time 5 "http://$COORD_IP:$COORD_STATUS/status" | python3 -c '
import sys,json
st=json.load(sys.stdin); sys.exit(0 if any(g.get("in_progress") for g in st.get("games") or []) else 1)' 2>/dev/null; then
    STARTED=1; break
  fi
  sleep 10
done
curl -s "http://$COORD_IP:$COORD_STATUS/status" > "$RUN/status-start.json"
if [ $STARTED -ne 1 ]; then
  say "match never started; collecting evidence"
else
  say "match started; soaking ${SOAK}s (status polls every 30s)"
  END=$(( $(date +%s) + SOAK ))
  while [ "$(date +%s)" -lt $END ]; do
    curl -s --max-time 5 "http://$COORD_IP:$COORD_STATUS/status" >> "$RUN/status.jsonl"; echo >> "$RUN/status.jsonl"
    sleep 30
  done
fi

say "collecting artifacts"
curl -s "http://$COORD_IP:$COORD_STATUS/status" > "$RUN/status-final.json"
scp_from coord $COORD_ZONE relaylab-pub coord.log "$RUN/coord.log" >/dev/null 2>&1 || true
for row in "${CLIENTS[@]}"; do
  set -- $row
  # Stage to a space-free path first; gcloud scp mangles remote paths with
  # spaces (all eight ReleaseLogs came back missing on the first run).
  ssh_c "$1" "$2" "$3" 'cp "/opt/zh-pref/drive_c/users/'"$USER"'/Documents/Command and Conquer Generals Zero Hour Data/ReleaseLog.txt" /tmp/ReleaseLog.txt 2>/dev/null; true' >/dev/null 2>&1
  scp_from "$1" "$2" "$3" /tmp/ReleaseLog.txt "$RUN/ReleaseLog.$1.txt" >/dev/null 2>&1 || echo "  no ReleaseLog from $1"
  scp_from "$1" "$2" "$3" /tmp/game.log "$RUN/game.$1.log" >/dev/null 2>&1 || true
done

if [ "${SKIP_KILL:-0}" = 1 ]; then
  say "SKIP_KILL=1: leaving game processes alive for hang inspection"
else
  say "stopping game processes"
  for row in "${CLIENTS[@]}"; do
    set -- $row
    ssh_c "$1" "$2" "$3" 'pkill generalszh 2>/dev/null; true' >/dev/null 2>&1 &
  done
  wait 2>/dev/null || true
fi

say "verifying"
python3 "$(dirname "$0")/verify.py" "$SCEN" "$RUN" "$STARTED" "$HOSTC"
RC=$?
say "done: artifacts in $RUN"
exit $RC
