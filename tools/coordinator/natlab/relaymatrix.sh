#!/bin/bash
# relaymatrix.sh tier0 [pair-name ...]
#
# Tier 0 of the relay-fallback test plan (scratchpad/relay_test_plan.md):
# the stuntest protocol matrix. Two stuntest processes behind natlab NAT
# routers of every interesting type pairing, asserting whether the pair
# ends up direct, relayed, or failed. Runs in a few minutes with no wine.
#
# Each pair test:
#   - shapes routerA/routerB with nat-shape.sh
#   - hostA hosts a uniquely named game, clientB joins it by name
#   - both punch, then run the converge loop (a miniature of the client's
#     sticky/silence rules) and print VERDICT direct|relay|fail
#   - -expect makes each side exit nonzero on the wrong verdict
#
# Expectations encode the reply-to-source rekey behavior: a sym peer's
# packets reach a cone peer from an unadvertised port, the cone peer
# replies to the OBSERVED source, and that reply threads back through the
# symmetric NAT's per-destination mapping, so sym-fc and sym-rc pairs
# converge DIRECT. Only pairs with endpoint-dependent behavior on BOTH
# relevant sides (sym/cgn vs sym/cgn/prc) or a blocked path relay.
set -uo pipefail

NATLAB="$(cd "$(dirname "$0")" && pwd)"
COORD_DIR="$(cd "$NATLAB/.." && pwd)"
REPO="$(cd "$COORD_DIR/../.." && pwd)"
RUNROOT="$REPO/scratchpad/relaylab-runs/$(date +%Y%m%d-%H%M%S)-tier0"
COORD_TCP=37500
COORD_UDP=37501
COORD_STATUS=37502
COORD_IP=10.99.0.1

mkdir -p "$RUNROOT" "$NATLAB/bin"

echo "[tier0] building coord + stuntest"
(cd "$COORD_DIR" && go build -o "$NATLAB/bin/coord" ./cmd/coord && go build -o "$NATLAB/bin/stuntest" ./cmd/stuntest)

echo "[tier0] starting coordinator"
pkill -x coord 2>/dev/null || true
sleep 0.3
"$NATLAB/bin/coord" -tcp :$COORD_TCP -udp :$COORD_UDP -udp2 :37503 -status :$COORD_STATUS \
  > "$RUNROOT/coord.log" 2>&1 &
COORD_PID=$!
trap 'kill $COORD_PID 2>/dev/null || true' EXIT
sleep 0.5

echo "[tier0] bringing up natlab (2 clients)"
NCLIENTS=2 "$NATLAB/natlab-up.sh" > "$RUNROOT/natlab.log"

status_snap() { curl -s "localhost:$COORD_STATUS/status" 2>/dev/null; }

PASS=0
FAIL=0
declare -a FAILED_NAMES=()

# run_pair NAME typeA typeB expectA expectB [extraA] [extraB]
run_pair() {
  local NAME=$1 TA=$2 TB=$3 EA=$4 EB=$5 XA=${6:-} XB=${7:-}
  local G="t0-$NAME-$RANDOM"
  local DIR="$RUNROOT/$NAME"
  mkdir -p "$DIR"

  "$NATLAB/nat-shape.sh" A "$TA" $COORD_IP $COORD_UDP > "$DIR/shapeA.log"
  "$NATLAB/nat-shape.sh" B "$TB" $COORD_IP $COORD_UDP > "$DIR/shapeB.log"

  # cgn adds a hop; the low-TTL punch volley must still clear our own NATs.
  local TTLA=2 TTLB=2
  [ "$TA" = cgn ] && TTLA=3
  [ "$TB" = cgn ] && TTLB=3

  local FWD_BEFORE
  FWD_BEFORE=$(status_snap | python3 -c 'import sys,json; print(json.load(sys.stdin).get("relay_forwarded",0))' 2>/dev/null || echo 0)

  sudo ip netns exec clientA sudo -u "$USER" env PUNCH_LOW_TTL=$TTLA \
    "$NATLAB/bin/stuntest" -coord $COORD_IP:$COORD_TCP -nick "hA-$NAME" \
    -host -game-name "$G" -expect "$EA" $XA > "$DIR/hostA.log" 2>&1 &
  local APID=$!
  sleep 1
  sudo ip netns exec clientB sudo -u "$USER" env PUNCH_LOW_TTL=$TTLB \
    "$NATLAB/bin/stuntest" -coord $COORD_IP:$COORD_TCP -nick "jB-$NAME" \
    -join-name "$G" -expect "$EB" $XB > "$DIR/joinB.log" 2>&1 &
  local BPID=$!

  local RCA=0 RCB=0
  wait $APID || RCA=$?
  wait $BPID || RCB=$?
  status_snap > "$DIR/status.json"

  local FWD_AFTER
  FWD_AFTER=$(python3 -c 'import sys,json; print(json.load(sys.stdin).get("relay_forwarded",0))' < "$DIR/status.json" 2>/dev/null || echo 0)
  local FWD_DELTA=$((FWD_AFTER - FWD_BEFORE))

  # Relay exclusivity: direct pairs must move ZERO relayed packets.
  local EXCL_OK=1
  if [ "$EA" = direct ] && [ "$EB" = direct ] && [ "$FWD_DELTA" -gt 0 ]; then
    EXCL_OK=0
  fi
  if { [ "$EA" = relay ] || [ "$EB" = relay ]; } && [ "$FWD_DELTA" -eq 0 ]; then
    EXCL_OK=0
  fi

  local VA VB
  VA=$(grep -o 'VERDICT .*' "$DIR/hostA.log" | tail -1 || true)
  VB=$(grep -o 'VERDICT .*' "$DIR/joinB.log" | tail -1 || true)
  if [ $RCA -eq 0 ] && [ $RCB -eq 0 ] && [ $EXCL_OK -eq 1 ]; then
    printf '  PASS %-18s %s-%s: A=%s B=%s relayed_pkts=%d\n' "$NAME" "$TA" "$TB" "${VA#VERDICT }" "${VB#VERDICT }" "$FWD_DELTA"
    PASS=$((PASS+1))
  else
    printf '  FAIL %-18s %s-%s: A=[%s rc=%d want %s] B=[%s rc=%d want %s] relayed_pkts=%d excl=%d\n' \
      "$NAME" "$TA" "$TB" "${VA:-none}" "$RCA" "$EA" "${VB:-none}" "$RCB" "$EB" "$FWD_DELTA" "$EXCL_OK"
    FAIL=$((FAIL+1))
    FAILED_NAMES+=("$NAME")
  fi
}

# name typeA typeB expectA expectB [extraA] [extraB]
MATRIX=(
  "prc-prc      prc prc direct direct"
  "fc-prc       fc  prc direct direct"
  "rc-prc       rc  prc direct direct"
  "fc-rc        fc  rc  direct direct"
  "fc-fc        fc  fc  direct direct"
  "rc-rc        rc  rc  direct direct"
  "sym-fc       sym fc  direct direct"
  "sym-rc       sym rc  direct direct"
  "sym-sym      sym sym relay  relay"
  "sym-prc      sym prc relay  relay"
  "blk-prc      blk prc relay  relay"
  "blk-sym      blk sym relay  relay"
  "blk-blk      blk blk relay  relay"
  "cgn-prc      cgn prc relay  relay"
  "cgn-sym      cgn sym relay  relay"
  "cgn-cgn      cgn cgn relay  relay"
  "norelay-ok   prc prc direct direct -relay=false"
  "norelay-fail sym sym fail   fail   -relay=false"
)

FILTER=("${@:2}")
echo "[tier0] running matrix into $RUNROOT"
for row in "${MATRIX[@]}"; do
  # shellcheck disable=SC2086
  set -- $row
  if [ ${#FILTER[@]} -gt 0 ]; then
    keep=0
    for f in "${FILTER[@]}"; do [ "$f" = "$1" ] && keep=1; done
    [ $keep -eq 1 ] || continue
  fi
  run_pair "$@"
done

status_snap > "$RUNROOT/final-status.json"
echo
echo "[tier0] $PASS passed, $FAIL failed. Logs: $RUNROOT"
if [ $FAIL -gt 0 ]; then
  echo "[tier0] failed: ${FAILED_NAMES[*]}"
  exit 1
fi
