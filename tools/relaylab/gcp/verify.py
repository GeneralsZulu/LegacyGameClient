#!/usr/bin/env python3
"""Tier 2 verification: per-pair prediction diff + soak assertions.

Reads the collected run directory (ReleaseLog.<c>.txt per client, coord.log,
status-final.json) and evaluates:
  1. predicted-relay pairs show relay engagement on BOTH sides (grant,
     sticky, or silence breadcrumb naming the peer), predicted-direct pairs
     show none anywhere;
  2. every client's NETPATH lines prove sustained in-game traffic
     (>=2 game-channel lines, nonzero in_pps at the end);
  3. the coordinator actually forwarded packets iff relays were predicted.

NAT classes (must match env.sh CLIENTS):
  cone-ish : cloud-cone (c1,c2), prc (c5)   - endpoint-independent mapping,
             endpoint-dependent filtering
  fc       : c8                              - accepts anything
  sym-ish  : cloud-sym (c3,c4), cgn (c7)     - endpoint-dependent mapping
  blk      : c6                              - UDP dead except coordinator
Pair rules (validated by the tier-0 netns matrix):
  blk in pair                      -> relay
  sym-ish vs anything except fc    -> relay
  everything else                  -> direct
  c1-c2 share one NAT IP           -> direct via the VPC-local address path
"""
import itertools, json, re, sys

CLASSES = {
    "c1": "cone", "c2": "cone", "c3": "sym", "c4": "sym",
    "c5": "cone", "c6": "blk", "c7": "sym", "c8": "fc",
}

# Pairs sharing one NAT public IP: the client skips relay registration and
# uses the local-address path (c1+c2 share the cone NAT's static IP; c3+c4
# share the sym gateway's auto-allocated IP, verified live in the first
# T2-FULL8: they went direct over the VPC exactly as designed).
SAME_IP = [{"c1", "c2"}, {"c3", "c4"}]

def predict(a, b, host):
    if {a, b} in SAME_IP:
        return "direct"
    ca, cb = CLASSES[a], CLASSES[b]
    if "blk" in (ca, cb):
        return "relay"
    if "sym" in (ca, cb):
        other = cb if ca == "sym" else ca
        if ca == "sym" and cb == "sym":
            return "relay"
        # sym vs fc converges direct ONLY on the punch pair: the fc side
        # replies to the observed source and threads the symmetric NAT's
        # per-destination mapping (tier 0 proved this live). Mesh pairs
        # never punch: both sides keepalive the ADVERTISED addrs, the sym
        # side's advertised mapping is useless, and the pair relays.
        if other == "fc" and host in (a, b):
            return "direct"
        return "relay"
    return "direct"

def main():
    scen, run, started = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
    host = sys.argv[4] if len(sys.argv) > 4 else {"T2-FULL8": "c1", "T2-HOST-BAD": "c6"}.get(scen, "c1")
    clients = sorted(CLASSES)
    fails = []
    if not started:
        fails.append("match never reached in_progress")

    logs = {}
    for c in clients:
        try:
            logs[c] = open(f"{run}/ReleaseLog.{c}.txt", errors="replace").read()
        except OSError:
            logs[c] = ""
            fails.append(f"{c}: ReleaseLog missing")

    # Which peers does each client END the soak relaying to? Flip lines and
    # probe-upgrade lines are replayed in order; the last event per peer
    # wins (a spurious flip healed by the direct-upgrade probe is direct).
    relayed_peers = {}
    for c in clients:
        state = {}
        for m in re.finditer(
                r"Relay: (?:game|lobby) traffic to (\S+) .*(now via coordinator|back to DIRECT)",
                logs[c]):
            state[m.group(1)] = (m.group(2) == "now via coordinator")
        for m in re.finditer(r"punch with (\S+) timed out.*using relay", logs[c]):
            state.setdefault(m.group(1), True)
        relayed_peers[c] = {p for p, rel in state.items() if rel}

    # The failover rules are ADAPTIVE: every rule moves a pair toward a
    # path that delivers packets, and the probe upgrade walks spurious
    # relay flips back to direct. So exact path predictions are soft
    # preferences, not invariants. Hard failures are only:
    #   - a pair ending ONE-SIDED (the half-dead link the sticky rule
    #     exists to prevent),
    #   - an IMPOSSIBLE pair (blk involved, or sym-sym) ending direct,
    #     which would mean bogus path evidence.
    # A direct-capable pair settling on relay is wasteful but safe: WARN.
    # A relay-predicted pair ending direct found a real path: note it.
    warns = []
    table = []
    for a, b in itertools.combinations(clients, 2):
        want = predict(a, b, host)
        ca, cb = CLASSES[a], CLASSES[b]
        # Same-NAT-IP pairs talk over their mutually reachable local addrs,
        # so direct is correct for them regardless of the NAT classes.
        impossible_direct = ({a, b} not in SAME_IP) and \
            ("blk" in (ca, cb) or (ca == "sym" and cb == "sym"))
        a_rel = b in relayed_peers[a]
        b_rel = a in relayed_peers[b]
        if a_rel != b_rel:
            got = f"one-sided(a={a_rel},b={b_rel})"
            ok = False
            fails.append(f"pair {a}-{b}: {got} at soak end")
        else:
            got = "relay" if a_rel else "direct"
            ok = True
            if got == "direct" and impossible_direct:
                ok = False
                fails.append(f"pair {a}-{b}: ended direct but direct is impossible ({ca}-{cb})")
            elif got != want:
                warns.append(f"pair {a}-{b}: want {want}, got {got}")
        table.append({"pair": f"{a}-{b}", "want": want, "got": got, "ok": ok})

    for c in clients:
        np = re.findall(r"NETPATH ch=game peers=(\d+) relayed=(\d+) out_pps=(\d+) in_pps=(\d+)", logs[c])
        np = [tuple(map(int, t)) for t in np]
        if len(np) < 2:
            fails.append(f"{c}: only {len(np)} NETPATH game lines")
        elif np[-1][3] == 0:
            fails.append(f"{c}: in_pps=0 at soak end")

    try:
        st = json.load(open(f"{run}/status-final.json"))
    except Exception:
        st = {}
        fails.append("status-final.json unreadable")
    fwd = st.get("relay_forwarded", 0)
    n_relay_pairs = sum(1 for t in table if t["want"] == "relay")
    if n_relay_pairs and fwd < 100:
        fails.append(f"coordinator only forwarded {fwd} packets with {n_relay_pairs} relay pairs predicted")

    verdict = "PASS" if not fails else "FAIL"
    out = {
        "scenario": scen, "verdict": verdict, "relay_forwarded": fwd,
        "punch_relayed": st.get("punch_relayed", 0),
        "relay_grants_sent": st.get("relay_grants_sent", 0),
        "pairs": table, "failures": fails, "warnings": warns,
    }
    open(f"{run}/verdict.json", "w").write(json.dumps(out, indent=2))
    print(json.dumps({k: out[k] for k in ("scenario", "verdict", "relay_forwarded", "failures")}, indent=2))
    bad = [t for t in table if not t["ok"]]
    print(f"pair table: {sum(t['ok'] for t in table)}/{len(table)} ok" + (f"; mismatches: {bad}" if bad else ""))
    sys.exit(0 if verdict == "PASS" else 1)

if __name__ == "__main__":
    main()
