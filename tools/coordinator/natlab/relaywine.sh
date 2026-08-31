#!/bin/bash
# relaywine.sh <scenario> [soak_sec]   (scenarios listed in the case below)
#
# Tier 1 of the relay-fallback test plan: real generalszh.exe clients under
# wine, each in its own natlab NAT netns, driven unattended by the
# -coordauto* flags. See scratchpad/relay_test_plan.md sections 2.5/4.
#
#   T1-BASELINE        3 clients, all prc. Everything punches; asserts the
#                      relay NEVER engages (no flips, zero forwarded pkts)
#                      and the match soaks with traffic flowing.
#   T1-FORCED-HOSTPAIR 2 clients, host prc, guest blk (UDP dead except the
#                      coordinator). Punch must fail, the pair must relay,
#                      the match must start and soak entirely relayed.
#   T1-MIDGAME-DEATH   3 clients, all prc, soak direct, then kill the
#                      guest-guest direct path mid-match. Asserts the
#                      silence trigger + sticky flip heal the pair onto the
#                      relay with traffic flowing again, well inside the
#                      60s disconnect timeout.
#   T1-SAMEWAN-ISOLATED 2 clients behind one router on separate subnets
#                      that CANNOT reach each other (guest wifi / VLAN /
#                      VM host-only nets / CGNAT across households). One
#                      public IP, distinct private IPs, no usable LAN path
#                      and no hairpin, so only the relay can carry it.
#                      Regression cover for the same-public-IP registry
#                      skip that broke same-network online play.
#   T1-SAMELAN-HOSTPAIR 2 clients behind one router on separate subnets:
#                      one public IP, distinct private IPs. Must play
#                      fully DIRECT on both channels: cover for the
#                      same-NAT peer rewrite and game_local_addr.
#
# Artifacts land in scratchpad/relaylab-runs/<stamp>-<scenario>/.
set -uo pipefail

SCEN=${1:?scenario}
SOAK=${2:-180}

NATLAB="$(cd "$(dirname "$0")" && pwd)"
COORD_DIR="$(cd "$NATLAB/.." && pwd)"
REPO="$(cd "$COORD_DIR/../.." && pwd)"
RUN="$REPO/scratchpad/relaylab-runs/$(date +%Y%m%d-%H%M%S)-$SCEN"
COORD_IP=10.99.0.1
COORD_TCP=37500
COORD_UDP=37501
COORD_STATUS=37502

source "$NATLAB/lib/winefleet.sh"
mkdir -p "$RUN"
exec > >(tee "$RUN/run.log") 2>&1

say() { echo "[$(date +%H:%M:%S)] $*"; }

status_json() { curl -s "localhost:$COORD_STATUS/status" 2>/dev/null; }
status_field() { status_json | python3 -c "import sys,json; print(json.load(sys.stdin).get('$1',0))" 2>/dev/null || echo 0; }

say "building coord"
(cd "$COORD_DIR" && go build -o "$NATLAB/bin/coord" ./cmd/coord)
fleet_kill
pkill -x coord 2>/dev/null || true
sleep 0.5
"$NATLAB/bin/coord" -tcp :$COORD_TCP -udp :$COORD_UDP -udp2 :37503 -status :$COORD_STATUS > "$RUN/coord.log" 2>&1 &
COORD_PID=$!

case "$SCEN" in
  T1-BASELINE)        N=3; NATTYPES=(prc prc prc);;
  T1-FORCED-HOSTPAIR) N=2; NATTYPES=(prc blk);;
  T1-MIDGAME-DEATH)   N=3; NATTYPES=(prc prc prc);;
  # The T2-HOST-BAD reproduction: a fully relayed HOST with multiple
  # joiners. Cloud showed one joiner seating and the rest never landing.
  T1-HOST-RELAYED)    N=4; NATTYPES=(blk prc prc prc);;
  # Same, plus a same-public-IP guest pair (client 5 runs inside client
  # 2's netns): the cloud topology's c1/c2 ingredient, prime suspect for
  # the multi-join stall (c2's accepts landed on c1 in the cloud trace).
  T1-HR-SAMEIP)       N=5; NATTYPES=(blk prc prc prc prc);;
  # Scaled hang detector for the cloud T2-HOST-BAD freeze: blk host, five
  # routed guests plus a same-netns sixth. Pass = everyone reaches in-game.
  T1-HB6)             N=6; NATTYPES=(blk prc prc sym sym prc);;
  # Warning validation: a SYMMETRIC host. Asserts the NAT self-check
  # classifies it (NATCHECK symmetric=1), the host-warning breadcrumb
  # fires (dialog itself suppressed in auto flows), and the relayed-host
  # lobby notice posts once the guest's grant arrives.
  T1-SYMHOST)         N=2; NATTYPES=(sym prc);;
  # The PUNCH PAIR shares a public IP. Every other same-IP scenario above
  # pairs two GUESTS, which only ever meet over the mesh and never arm a
  # real punch, so the host<->guest case went untested and shipped broken.
  # Here bob is on his own subnet behind alice's router, but the router is
  # told not to forward between the two LANs -- the shape of split subnets
  # that cannot see each other, and of a CGNAT block shared by strangers.
  # The LAN candidate exists but goes nowhere and the public one needs
  # hairpin, so only the relay can carry this pair.
  #
  # Deliberately NOT the same-private-IP case (two copies on one machine):
  # sharing an internal IP breaks far more than this and is not a scenario
  # worth holding the netcode to.
  T1-SAMEWAN-ISOLATED) N=2; NATTYPES=(prc);;
  # Same public IP, DIFFERENT private IPs: bob sits on a second subnet
  # behind alice's router (two-subnet household / CGNAT block). Must play
  # fully DIRECT on both channels -- cover for the same-NAT peer address
  # rewrite plus the game channel's own LAN address (game_local_addr).
  T1-SAMELAN-HOSTPAIR) N=2; NATTYPES=(prc);;
  *) echo "unknown scenario $SCEN"; exit 1;;
esac

say "natlab up ($N clients) + shaping: ${NATTYPES[*]}"
# Same-netns scenarios: the last client shares client 2's netns; one
# fewer router exists than clients.
NROUTERS=$N
[ "$SCEN" = T1-HR-SAMEIP ] && NROUTERS=4
[ "$SCEN" = T1-HB6 ] && NROUTERS=5
# Host-pair-shares-an-IP scenarios: both clients live behind routerA, so
# there is exactly one router. SAMEIP puts them in one netns (same private
# IP); SAMELAN gives the joiner its own subnet off the same router.
[ "$SCEN" = T1-SAMEWAN-ISOLATED ] && NROUTERS=1
[ "$SCEN" = T1-SAMELAN-HOSTPAIR ] && NROUTERS=1
if [ "$SCEN" = T1-SAMELAN-HOSTPAIR ] || [ "$SCEN" = T1-SAMEWAN-ISOLATED ]; then
  SIBLING_ROUTER=A NCLIENTS=$NROUTERS "$NATLAB/natlab-up.sh" > "$RUN/natlab.log"
else
  NCLIENTS=$NROUTERS "$NATLAB/natlab-up.sh" > "$RUN/natlab.log"
fi
for ((i=1; i<=NROUTERS; i++)); do
  "$NATLAB/nat-shape.sh" "$(fleet_letter $i)" "${NATTYPES[$((i-1))]}" $COORD_IP $COORD_UDP >> "$RUN/natlab.log"
done
if [ "$SCEN" = T1-SAMEWAN-ISOLATED ]; then
  # Sever the two LAN subnets from each other; the WAN path is untouched,
  # so both still reach the coordinator and both still share one public IP.
  say "isolating the two LAN subnets behind routerA"
  sudo ip netns exec routerA iptables -I FORWARD -s 10.99.1.0/30 -d 10.99.101.0/30 -j DROP
  sudo ip netns exec routerA iptables -I FORWARD -s 10.99.101.0/30 -d 10.99.1.0/30 -j DROP
fi

say "wine fleet setup + deploy (first run takes a few minutes)"
fleet_setup $N
fleet_deploy $N

GAME="lab-$SCEN-$RANDOM"
say "launching host (alice) + joiners"
fleet_launch 1 alice -coordautohost "$GAME" -coordautostart $N
sleep 20
NICKS=(alice bob carol dave eve fred gina hank)
for ((i=2; i<=N; i++)); do
  if { [ "$SCEN" = T1-HR-SAMEIP ] && [ $i -eq 5 ]; } || { [ "$SCEN" = T1-HB6 ] && [ $i -eq 6 ]; }; then
    # eve shares bob's netns/public IP; ephemeral-port fallback kicks in.
    FLEET_NETNS_OVERRIDE=B fleet_launch $i "${NICKS[$((i-1))]}" -coordautojoin "$GAME" -coordautostart $N
  elif [ "$SCEN" = T1-SAMEWAN-ISOLATED ] && [ $i -eq 2 ]; then
    # bob is the sibling netns off the host's router, with the LAN path
    # between them cut: one public IP, its own private subnet, no route.
    FLEET_NETNS_OVERRIDE=A2 fleet_launch $i "${NICKS[$((i-1))]}" -coordautojoin "$GAME" -coordautostart $N
  elif [ "$SCEN" = T1-SAMELAN-HOSTPAIR ] && [ $i -eq 2 ]; then
    # bob is the sibling netns off the host's router: one public IP, its
    # own private subnet, so a local candidate genuinely routes.
    FLEET_NETNS_OVERRIDE=A2 fleet_launch $i "${NICKS[$((i-1))]}" -coordautojoin "$GAME" -coordautostart $N
  else
    fleet_launch $i "${NICKS[$((i-1))]}" -coordautojoin "$GAME" -coordautostart $N
  fi
  sleep 5
done

say "waiting for the match to start (in_progress on the coordinator)"
START_DEADLINE=$(( $(date +%s) + 300 ))
STARTED=0
while [ "$(date +%s)" -lt $START_DEADLINE ]; do
  if status_json | python3 -c '
import sys, json
st = json.load(sys.stdin)
gs = st.get("games") or []
sys.exit(0 if any(g.get("in_progress") for g in gs) else 1)' 2>/dev/null; then
    STARTED=1
    break
  fi
  sleep 5
done
if [ $STARTED -ne 1 ]; then
  say "FAIL: match never started; screenshots + logs in $RUN"
  for ((i=1; i<=N; i++)); do DISPLAY=:9$i import -window root "$RUN/client$i.png" 2>/dev/null || true; done
  for ((i=1; i<=N; i++)); do cp "$(fleet_releaselog_i $i)" "$RUN/ReleaseLog.$i.txt" 2>/dev/null || true; done
  fleet_kill; kill $COORD_PID 2>/dev/null
  exit 1
fi
say "match started; soaking ${SOAK}s"
FWD_AT_START=$(status_field relay_forwarded)

CHAOS_NOTE=""
if [ "$SCEN" = T1-MIDGAME-DEATH ]; then
  sleep $(( SOAK / 3 ))
  say "CHAOS: killing the bob<->carol direct path (coordinator leg untouched)"
  CHAOS_NOTE="chaos injected at +$((SOAK/3))s"
  # B public = 10.99.20.2, C public = 10.99.30.2
  sudo ip netns exec routerB iptables -I FORWARD -p udp -d 10.99.30.2 -j DROP
  sudo ip netns exec routerB iptables -I FORWARD -p udp -s 10.99.30.2 -j DROP
  sudo ip netns exec routerC iptables -I FORWARD -p udp -d 10.99.20.2 -j DROP
  sudo ip netns exec routerC iptables -I FORWARD -p udp -s 10.99.20.2 -j DROP
  sudo ip netns exec routerB conntrack -F 2>/dev/null || true
  sudo ip netns exec routerC conntrack -F 2>/dev/null || true
  sleep $(( SOAK - SOAK / 3 ))
else
  sleep "$SOAK"
fi

say "collecting artifacts"
status_json > "$RUN/status-final.json"
for ((i=1; i<=N; i++)); do
  cp "$(fleet_releaselog_i $i)" "$RUN/ReleaseLog.$i.txt" 2>/dev/null || true
  DISPLAY=:9$i import -window root "$RUN/client$i.png" 2>/dev/null || true
done
FWD_AT_END=$(status_field relay_forwarded)
fleet_kill
kill $COORD_PID 2>/dev/null || true

say "verifying"
python3 - "$SCEN" "$RUN" "$N" "$FWD_AT_START" "$FWD_AT_END" <<'EOF'
import json, re, sys
scen, run, n, fwd0, fwd1 = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
fwd = fwd1 - fwd0
fails = []
logs = {}
for i in range(1, n + 1):
    try:
        logs[i] = open(f"{run}/ReleaseLog.{i}.txt", errors="replace").read()
    except OSError:
        logs[i] = ""
        fails.append(f"client{i}: ReleaseLog missing")

def netpath(i, ch="game"):
    out = []
    for m in re.finditer(r"NETPATH ch=%s peers=(\d+) relayed=(\d+) out_pps=(\d+) in_pps=(\d+)" % ch, logs[i]):
        out.append(tuple(int(x) for x in m.groups()))
    return out

def relay_lines(i):
    return [l for l in logs[i].splitlines() if "Relay:" in l or "using relay" in l]

for i in range(1, n + 1):
    np = netpath(i)
    if len(np) < 2:
        fails.append(f"client{i}: only {len(np)} NETPATH game lines (no sustained soak)")
    elif np[-1][3] == 0:
        fails.append(f"client{i}: final NETPATH in_pps=0 (traffic dead at soak end)")
    if "DISCONNECT dropped" in logs[i]:
        fails.append(f"client{i}: a player was DROPPED mid-game (DISCONNECT breadcrumb)")

if scen == "T1-BASELINE":
    if fwd != 0:
        fails.append(f"relay forwarded {fwd} packets in an all-direct scenario")
    for i in range(1, n + 1):
        if relay_lines(i):
            fails.append(f"client{i}: unexpected relay engagement: {relay_lines(i)[:2]}")
        if any(r for (_, r, _, _) in netpath(i)):
            fails.append(f"client{i}: NETPATH shows relayed>0 in baseline")

elif scen == "T1-FORCED-HOSTPAIR":
    if fwd <= 100:
        fails.append(f"relay forwarded only {fwd} packets across the soak (expected a relayed match)")
    for i in range(1, n + 1):
        if not relay_lines(i):
            fails.append(f"client{i}: no relay engagement breadcrumb")
        np = netpath(i)
        if not np or np[-1][1] < 1:
            fails.append(f"client{i}: final NETPATH relayed={np[-1][1] if np else 'none'} (want >=1)")

elif scen == "T1-SYMHOST":
    if fwd <= 100:
        fails.append(f"relay forwarded only {fwd} packets (sym host pair should relay)")
    if "NATCHECK" not in logs[1] or "symmetric=1" not in logs[1]:
        fails.append("host: NAT self-check did not classify the sym NAT")
    if ("NATCHECK host warning" not in logs[1] and "warning suppressed" not in logs[1]
            and "detected after hosting" not in logs[1]):
        fails.append("host: no host-warning breadcrumb")
    if "Relay host notice shown" not in logs[1]:
        fails.append("host: relayed-host lobby notice never posted")
    if "symmetric=1" in logs[2]:
        fails.append("guest behind prc misclassified as symmetric")
    if "symmetric=0" not in logs[2]:
        fails.append("guest: NAT self-check never completed")

elif scen == "T1-SAMEWAN-ISOLATED":
    # One public IP, distinct private IPs, and no route between the two
    # subnets: the LAN candidate is advertised but goes nowhere and the
    # public one needs hairpin, so the relay is the only way this pair
    # plays. Before the registry fix a peer behind our own public IP was
    # never registered, so hasPeer() was false, the punch-timeout fallback
    # could not flip the pair AND the server's relay_grant was discarded:
    # the join died with "punch: no inbound packet within timeout" and no
    # match ever started. This is the shape Bill hit on two VMs.
    if fwd <= 100:
        fails.append(f"relay forwarded only {fwd} packets (isolated same-WAN pair must relay)")
    for i in (1, 2):
        if not relay_lines(i):
            fails.append(f"client{i}: no relay engagement breadcrumb")
        np = netpath(i)
        if not np or np[-1][1] < 1:
            fails.append(f"client{i}: final NETPATH relayed={np[-1][1] if np else 'none'} (want >=1)")
    for i in (1, 2):
        if "punch: no inbound packet within timeout" in logs[i]:
            fails.append(f"client{i}: punch failed outright instead of falling back to relay")
    # The rewrite must still have fired: they share a public IP and each
    # has a distinct LAN addr, so the peer is addressed by it even though
    # it turns out not to route.
    for i in (1, 2):
        if "shares our public IP" not in logs[i]:
            fails.append(f"client{i}: never rewrote the same-NAT peer to its LAN addr")

elif scen == "T1-SAMELAN-HOSTPAIR":
    # Two players on one network must play DIRECTLY, not pay a round trip
    # to the relay to reach the machine next to them.
    #
    # Both halves of that are load-bearing. The host hands off to TheLAN
    # before any joiner exists, so it never punches: it only ever sends to
    # whatever peer address it was handed, and a shared public address
    # needs hairpin. Rewriting a same-NAT peer's punched addrs to its LAN
    # addrs at peer_info time is what lets the host reach it, and the
    # joiner then rekeys onto that source. The game leg needs its own LAN
    # address (game_local_addr) or 8088 stays stuck on the public one and
    # this pair would lobby fine and then relay in-game.
    if fwd != 0:
        fails.append(f"relay forwarded {fwd} packets; a same-LAN pair must be fully direct")
    for i in (1, 2):
        if relay_lines(i):
            fails.append(f"client{i}: unexpected relay engagement: {relay_lines(i)[:2]}")
        if any(r for (_, r, _, _) in netpath(i)):
            fails.append(f"client{i}: NETPATH shows relayed>0 on a same-LAN pair")
    # The rewrite must actually be what carried it, on BOTH channels.
    for i in (1, 2):
        if "shares our public IP" not in logs[i]:
            fails.append(f"client{i}: never rewrote the same-NAT peer to its LAN addr")
        if "for game" not in logs[i]:
            fails.append(f"client{i}: game channel never took the peer's LAN addr")

elif scen == "T1-HB6":
    # Hang detector: the pass signal is simply that EVERY client got
    # in-game and stayed there (the generic NETPATH checks above). Mixed
    # sym guests make extra relayed mesh pairs legitimate, so no relayed
    # count assertions here.
    if fwd <= 100:
        fails.append(f"relay forwarded only {fwd} packets (host pairs not relayed?)")

elif scen in ("T1-HOST-RELAYED", "T1-HR-SAMEIP"):
    # Host (client1) is blk: all three host pairs must relay; the guest
    # mesh pairs (prc-prc) must stay direct. The match starting AT ALL is
    # the regression gate for the relayed-host multi-join bug.
    if fwd <= 100:
        fails.append(f"relay forwarded only {fwd} packets (host pairs not relayed?)")
    for i in range(1, n + 1):
        np = netpath(i)
        if not np or np[-1][1] < 1:
            fails.append(f"client{i}: final NETPATH relayed={np[-1][1] if np else 'none'} (host pair must relay)")
    # Guests: apart from the host pair, the prc-prc mesh must stay direct.
    #
    # eve (client5) is excluded, and so is every pair involving her. She
    # runs inside bob's netns, which means she shares his PRIVATE ip as
    # well as his public one -- two copies on one machine, not two players
    # in a house. That collides on the game port, forces an ephemeral bind
    # and breaks her direct paths to everyone, and other guests then flip
    # toward her only via the sticky rule. Sharing an internal address
    # breaks more than netcode and is not a case worth asserting on
    # (Bill, 2026-08-31). The household shape -- one public IP, distinct
    # private IPs -- is covered properly by T1-SAMELAN-HOSTPAIR (direct)
    # and T1-SAMEWAN-ISOLATED (relayed).
    # The same-netns client is always the last one launched.
    same_netns_idx  = n
    same_netns_nick = ["alice", "bob", "carol", "dave", "eve", "fred"][n - 1]
    for i in range(2, n + 1):
        if i == same_netns_idx:
            continue                    # her own paths are out of scope too
        peers = set(re.findall(r"Relay: game traffic to (\w+)", logs[i]))
        peers.discard(same_netns_nick)
        peers.discard("alice")          # the blk host: expected to relay
        if peers:
            fails.append(f"client{i}: unexpected relayed mesh peers {sorted(peers)}")

elif scen == "T1-MIDGAME-DEATH":
    # The killed pair is guest-guest, and lockstep routes most traffic via
    # the host (packet router), so the healed mesh path carries only ~0.3
    # pps of keepalive/ack traffic. Any sustained forwarding proves the
    # heal; the flip breadcrumbs + NETPATH relayed prove the rest.
    if fwd <= 10:
        fails.append(f"relay forwarded only {fwd} packets (heal never engaged)")
    trig = [i for i in range(1, n + 1) if "silence trigger" in logs[i]]
    stick = [i for i in range(1, n + 1) if "sticky" in logs[i]]
    if not trig:
        fails.append("no client fired the silence trigger")
    if not (set(trig) | set(stick)) >= {2, 3}:
        fails.append(f"pair did not converge: silence on {trig}, sticky on {stick}")
    for i in (2, 3):
        np = netpath(i)
        if not np or np[-1][1] < 1:
            fails.append(f"client{i}: final NETPATH relayed={np[-1][1] if np else 'none'} after path kill")

verdict = "PASS" if not fails else "FAIL"
result = {"scenario": scen, "verdict": verdict, "relay_forwarded_delta": fwd, "failures": fails}
print(json.dumps(result, indent=2))
open(f"{run}/verdict.json", "w").write(json.dumps(result, indent=2))
sys.exit(0 if verdict == "PASS" else 1)
EOF
RC=$?
say "done ($SCEN): artifacts in $RUN ${CHAOS_NOTE:+($CHAOS_NOTE)}"
exit $RC
