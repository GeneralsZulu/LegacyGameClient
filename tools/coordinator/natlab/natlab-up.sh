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
LETTERS=(A B C D E F G H)

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

# Optional same-subnet sibling: a second client netns sharing ONE LAN
# segment with an existing client, the way two machines in a house sit on
# one router. Needs a bridge in the router netns, since "same subnet" means
# same broadcast domain -- distinct from SIBLING_ROUTER below, which gives
# the second client its own subnet behind the same router.
#
#   SAMENET_ROUTER=A natlab-up.sh  ->  clientA(10.99.1.2) \
#                                      clientAb(10.99.1.3)  both on routerA br0
#
# The segment widens from /30 to /29 to fit the extra host. Addresses .1/.2
# are unchanged, so every existing scenario keeps working.
if [ -n "${SAMENET_ROUTER:-}" ]; then
  ML=$SAMENET_ROUTER
  MR=router$ML
  MC=client$ML
  MB=client${ML}b
  MI=1
  for ((i=1; i<=8; i++)); do
    [ "${LETTERS[$((i-1))]}" = "$ML" ] && MI=$i
  done
  MNET=10.99.$MI

  sudo ip netns add $MB 2>/dev/null || true
  sudo ip netns exec $MB ip link set lo up
  # Bridge the existing lan0 and a new lan_b onto one segment.
  sudo ip netns exec $MR ip link add br0 type bridge 2>/dev/null || true
  sudo ip netns exec $MR ip addr flush dev lan0 2>/dev/null || true
  sudo ip netns exec $MR ip link set lan0 master br0
  sudo ip -n $MR link del lanb 2>/dev/null || true
  sudo ip -n $MR link add lanb type veth peer name eth0 netns $MB
  sudo ip netns exec $MR ip link set lanb master br0
  sudo ip netns exec $MR ip link set lanb up
  sudo ip netns exec $MR ip addr replace $MNET.1/29 dev br0
  sudo ip netns exec $MR ip link set br0 up
  # Re-address the original client onto the wider mask.
  sudo ip netns exec $MC ip addr flush dev eth0
  sudo ip netns exec $MC ip addr replace $MNET.2/29 dev eth0
  sudo ip netns exec $MC ip route replace default via $MNET.1
  sudo ip netns exec $MB ip addr replace $MNET.3/29 dev eth0
  sudo ip netns exec $MB ip link set eth0 up
  sudo ip netns exec $MB ip route replace default via $MNET.1
  echo "  same-subnet: $MC ($MNET.2) + $MB ($MNET.3) share $MR br0 ($MNET.0/29)"
fi

# Optional sibling client: a SECOND client netns hanging off an existing
# router, on its own /30. It therefore shares that router's public IP while
# keeping a DISTINCT private address -- the shape a household with two
# subnets (guest wifi, VLANs, VM host-only nets) or a CGNAT block presents.
# Distinct from FLEET_NETNS_OVERRIDE, which puts two clients in ONE netns and
# so gives them the SAME private address.
#
#   SIBLING_ROUTER=A natlab-up.sh   ->  clientA2(10.99.101.2) - routerA
#
# The router already forwards and SNATs everything out wan0, so the sibling
# reaches its neighbour directly over the LAN and the internet through the
# same public address.
if [ -n "${SIBLING_ROUTER:-}" ]; then
  SL=$SIBLING_ROUTER
  SR=router$SL
  SC=client${SL}2
  # Index of the parent router, to derive a non-colliding /30.
  SI=1
  for ((i=1; i<=8; i++)); do
    [ "${LETTERS[$((i-1))]}" = "$SL" ] && SI=$i
  done
  SNET=10.99.$((100 + SI))

  sudo ip netns add $SC 2>/dev/null || true
  sudo ip netns exec $SC ip link set lo up
  sudo ip -n $SR link del lan1 2>/dev/null || true
  sudo ip -n $SR link add lan1 type veth peer name eth0 netns $SC
  sudo ip netns exec $SR ip addr replace $SNET.1/30 dev lan1
  sudo ip netns exec $SR ip link set lan1 up
  sudo ip netns exec $SC ip addr replace $SNET.2/30 dev eth0
  sudo ip netns exec $SC ip link set eth0 up
  sudo ip netns exec $SC ip route replace default via $SNET.1
  # Optionally NAT between the two LAN segments, the way VMware's vmnet
  # devices do (and many routers do between VLAN/guest networks). Without
  # this the peer's packets keep their own source address; with it they
  # arrive wearing the GATEWAY's address, which is what production showed
  # (a host addressing 172.16.232.135 saw replies from 172.16.28.1 and
  # credited the slot to the gateway). Opt-in so existing scenarios that
  # assume a plain routed path are unchanged.
  if [ -n "${SIBLING_SNAT:-}" ]; then
    MAINNET=10.99.$SI
    MAINMASK=29
    sudo ip netns exec $SR iptables -t nat -A POSTROUTING \
      -s $SNET.0/30 -d $MAINNET.0/$MAINMASK -j SNAT --to-source $MAINNET.1
    sudo ip netns exec $SR iptables -t nat -A POSTROUTING \
      -s $MAINNET.0/$MAINMASK -d $SNET.0/30 -j SNAT --to-source $SNET.1
    echo "  sibling SNAT: $SNET.0/30 <-> $MAINNET.0/$MAINMASK translated to the gateways"
  fi
  echo "  sibling: $SC ($SNET.2) behind $SR, shares its public IP"
fi

echo "natlab v4 up ($NCLIENTS clients):"
echo "  coordinator addr: 10.99.0.1 (host); publics 10.99.<10*i>.2"
echo "  run clients: sudo ip netns exec clientA|clientB|clientC <cmd>"
echo "  lab punch TTL: 2 (game flag: -coordpunchttl 2; stuntest: PUNCH_LOW_TTL=2)"
