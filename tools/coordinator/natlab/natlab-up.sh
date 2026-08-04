#!/bin/bash
# natlab-up.sh v4: N clients (default 3) behind N different NAT routers,
# with a middle "internet" router, so hole punching and multi-joiner flows
# can be tested without real ISP connections.
#
#   clientA(10.99.1.2) - routerA[SNAT->10.99.10.2] -\
#   clientB(10.99.2.2) - routerB[SNAT->10.99.20.2] -- inet ns (forwards all)
#   clientC(10.99.3.2) - routerC[SNAT->10.99.30.2] -/     |
#                                                    10.99.0.0/24
#                                                    host 10.99.0.1 = coordinator
#
# Each NAT router is its own netns with its own conntrack table.
# SNAT is port-preserving (endpoint-independent mapping, endpoint-dependent
# filtering = port-restricted cone, the typical home router).
# Hop count client->peer-NAT is 3 (routerX, inet, routerY): punch packets
# with TTL=2 create the local NAT mapping and die at inet.
set -euo pipefail

NCLIENTS=${NCLIENTS:-3}
LETTERS=(A B C D E F)

sudo ip netns add inet 2>/dev/null || true
sudo ip netns exec inet ip link set lo up
sudo ip netns exec inet sysctl -q -w net.ipv4.ip_forward=1

# host <-> inet (coordinator segment 10.99.0.0/24)
sudo ip link del vethinet 2>/dev/null || true
sudo ip link add vethinet type veth peer name eth0 netns inet
sudo ip addr replace 10.99.0.1/24 dev vethinet
sudo ip link set vethinet up
sudo ip netns exec inet ip addr replace 10.99.0.254/24 dev eth0
sudo ip netns exec inet ip link set eth0 up

for ((i=1; i<=NCLIENTS; i++)); do
  L=${LETTERS[$((i-1))]}
  R=router$L
  C=client$L
  WANNET=10.99.$((i * 10))
  PUB=$WANNET.2

  sudo ip netns add $R 2>/dev/null || true
  sudo ip netns add $C 2>/dev/null || true
  sudo ip netns exec $R ip link set lo up
  sudo ip netns exec $C ip link set lo up

  sudo ip -n inet link del wan$R 2>/dev/null || true
  sudo ip -n inet link add wan$R type veth peer name wan0 netns $R
  sudo ip netns exec inet ip addr replace $WANNET.1/24 dev wan$R
  sudo ip netns exec inet ip link set wan$R up
  sudo ip netns exec $R ip addr replace $PUB/24 dev wan0
  sudo ip netns exec $R ip link set wan0 up
  sudo ip netns exec $R ip route replace default via $WANNET.1

  sudo ip -n $R link del lan0 2>/dev/null || true
  sudo ip -n $R link add lan0 type veth peer name eth0 netns $C
  sudo ip netns exec $R ip addr replace 10.99.$i.1/30 dev lan0
  sudo ip netns exec $R ip link set lan0 up
  sudo ip netns exec $C ip addr replace 10.99.$i.2/30 dev eth0
  sudo ip netns exec $C ip link set eth0 up
  sudo ip netns exec $C ip route replace default via 10.99.$i.1

  sudo ip netns exec $R sysctl -q -w net.ipv4.ip_forward=1
  sudo ip netns exec $R iptables -t nat -F POSTROUTING
  sudo ip netns exec $R iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB

  # host reaches this NAT's wan segment via inet
  sudo ip route replace $WANNET.0/24 via 10.99.0.254
done

echo "natlab v4 up ($NCLIENTS clients):"
echo "  coordinator addr: 10.99.0.1 (host); publics 10.99.<10*i>.2"
echo "  run clients: sudo ip netns exec clientA|clientB|clientC <cmd>"
echo "  lab punch TTL: 2 (game flag: -coordpunchttl 2; stuntest: PUNCH_LOW_TTL=2)"
