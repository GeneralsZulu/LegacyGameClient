# relaylab: the GCP relay-fallback lab

An 8-player, real-internet exercise of the coordinator relay fallback,
runnable end to end with one command. Design and pass criteria:
`scratchpad/relay_test_plan.md` (tier 2). Everything lives in its own GCP
project (`relaylab-zulu-1`) so nothing here can touch other setups, and
every resource carries the label `relaylab=1` so teardown is by label, not
by memory.

## Topology (9 VMs, ~$0.70/hr while up)

| VM | where | NAT the internet sees |
|----|-------|----------------------|
| relaylab-coord | us-central1, public IP | the test coordinator (tcp 37500 / udp 37501 / status 37502) |
| c1, c2 | us-west1, no external IP | real Cloud NAT, EIM enabled = cone; both share ONE static NAT IP (real same-public-IP pair with mutually reachable local addrs) |
| c3, c4 | us-east1, no external IP | real Cloud NAT, EIM disabled = symmetric |
| c5 | europe-west1, public IP | in-VM netns `prc` (port restricted cone) across a real ocean |
| c6 | europe-west1, public IP | in-VM netns `blk` (UDP dead except the coordinator = forced relay) |
| c7 | us-west2, public IP | in-VM netns `cgn` (home prc behind carrier sym) |
| c8 | us-central1, public IP | in-VM netns `fc` (full cone) |

The in-VM NAT shapes are the same implementations as the local netns lab
(`tools/coordinator/natlab/nat-shape.sh`), on a real substrate.

## One-time setup

```
./project.sh    # create project, link billing, upload the 1.8GB game once
./image.sh      # bake relaylab-client image (wine + game + prefix), ~15 min
```

Kept between runs (the reusable template): the project, the game bucket,
and the image family `relaylab-client` (about $1/mo combined).

## A full run

```
./up.sh                     # ~5-10 min: networks, Cloud NATs, 9 VMs, push build
./run.sh T2-FULL8 600       # host on c1, 10 min soak, verify per-pair predictions
./run.sh T2-HOST-BAD 600    # optional: host on c6 (blk, EU): worst case host
./down.sh                   # delete every billable resource (keeps image+bucket)
```

or all of that: `./relaylab-cloud.sh all`.

Artifacts per run land in `scratchpad/relaylab-runs/<stamp>-<scenario>/`:
every client's ReleaseLog, the coordinator log, 30s status polls, and
`verdict.json` with the 28-pair predicted-vs-observed table.

## What a PASS means

- every predicted-relay pair shows relay engagement on BOTH sides, every
  predicted-direct pair shows none (grant/sticky/silence breadcrumbs);
- NETPATH lines show sustained in-game traffic on all 8 clients for the
  whole soak (match start alone is explicitly not a pass);
- the coordinator's relay counters moved, and only for relay pairs.

## Notes

- A fresh build is pushed by `up.sh` from `build/docker-vc6/GeneralsMD/`
  and `build/installer-tmp/Zulu.big`; rebake the image only when wine or
  the base game install change.
- c1-c4 have no external IPs; ssh to them tunnels through IAP
  (`--tunnel-through-iap`), which the scripts do automatically.
- Cloud NAT is configured with the default 30s UDP idle timeout ON
  PURPOSE: surviving real mapping expiry is part of what is under test.
- `down.sh --everything` also deletes the image and bucket.
