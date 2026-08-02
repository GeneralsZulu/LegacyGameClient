# natlab

Simulates two clients behind two **different** NAT routers on one Linux box,
with a middle "internet" hop, so hole punching can be tested without two real
ISP connections. Needs root (network namespaces + iptables).

```
./natlab-up.sh
# coordinator (e.g. cncstats with the embedded coordinator) listens on 10.99.0.1
sudo ip netns exec clientA sudo -u $USER env PUNCH_LOW_TTL=2 ./stuntest -nick alice -host -coord 10.99.0.1:37500
sudo ip netns exec clientB sudo -u $USER env PUNCH_LOW_TTL=2 ./stuntest -nick bob  -join <id> -coord 10.99.0.1:37500
./natlab-down.sh
```

Topology: `clientA - routerA(SNAT) - inet - routerB(SNAT) - clientB`, host on
the 10.99.0.0/24 segment via the inet namespace. Each router namespace has its
own conntrack table (port-preserving SNAT = port-restricted cone, a typical
home router).

`PUNCH_LOW_TTL=2` because the peer NAT is only 3 hops away here; the real
internet default is TTL 4. The lab deterministically reproduces the NAT
mapping-poisoning race (run with `PUNCH_LOW_TTL=0` to see the punch deadlock)
that the low-TTL first volley fixes.

Both scripts are idempotent; `natlab-down.sh` removes everything.
