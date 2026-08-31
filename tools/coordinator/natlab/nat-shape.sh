#!/bin/bash
# nat-shape.sh <LETTER> <fc|rc|prc|sym|cgn|blk> [coordIP] [coordUDPport]
#
# Reprograms one natlab router netns (created by natlab-up.sh) to behave as
# a specific NAT type. Used by the relay-fallback test matrix; see
# scratchpad/relay_test_plan.md section 2.2 for the type definitions.
#
#   prc  port restricted cone (natlab default: port-preserving SNAT)
#   fc   full cone: prc + static DNAT of all inbound UDP to the client
#   rc   restricted cone: fc + only admit inbound UDP from IPs the client
#        has previously sent to (xt_recent dynamic list)
#   sym  symmetric: SNAT with random port allocation per destination
#   cgn  CGNAT: this router becomes a home prc NAT in 100.64/10 behind a
#        new carrierX netns doing symmetric NAT on the original public IP
#   blk  UDP blocked except to/from the coordinator (forced relay)
#
# Safe to re-run with a different type; switching away from cgn restores
# the plain router<->inet uplink. All rules live inside netns, never on the
# host.
set -euo pipefail

L=${1:?letter A..H}
TYPE=${2:?fc|rc|prc|sym|cgn|blk}
COORD_IP=${3:-10.99.0.1}
COORD_UDP=${4:-37501}

LETTERS=ABCDEFGH
idx=${LETTERS%%$L*}
i=$(( ${#idx} + 1 ))
if [ $i -gt 8 ]; then echo "bad letter $L" >&2; exit 1; fi

R=router$L
CAR=carrier$L
WANNET=10.99.$((i * 10))
PUB=$WANNET.2
CLIENT=10.99.$i.2
CGNNET=100.64.$i

run() { sudo ip netns exec "$R" "$@"; }

# If a previous shape made this router a CGN client, restore the plain
# router<->inet uplink first (unless we are shaping cgn again, which
# rebuilds it anyway).
restore_plain_uplink() {
  if sudo ip netns list | grep -qw "$CAR"; then
    sudo ip netns del "$CAR" 2>/dev/null || true
  fi
  # (Re)create routerX <-> inet on the WAN subnet, exactly as natlab-up.sh.
  sudo ip -n inet link del wan$R 2>/dev/null || true
  sudo ip -n "$R" link del wan0 2>/dev/null || true
  sudo ip -n inet link add wan$R type veth peer name wan0 netns "$R"
  sudo ip netns exec inet ip addr replace $WANNET.1/24 dev wan$R
  sudo ip netns exec inet ip link set wan$R up
  run ip addr replace $PUB/24 dev wan0
  run ip link set wan0 up
  run ip route replace default via $WANNET.1
}

reset_rules() {
  run iptables -F FORWARD
  run iptables -t nat -F POSTROUTING
  run iptables -t nat -F PREROUTING
  # Old flows from a previous shape must not linger and fake connectivity.
  run conntrack -F >/dev/null 2>&1 || true
}

case "$TYPE" in
prc)
  restore_plain_uplink
  reset_rules
  run iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB
  ;;
fc)
  restore_plain_uplink
  reset_rules
  run iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB
  # Any inbound UDP, any source, forwarded to the client (dport preserved,
  # which matches the port-preserving SNAT). One client per netns only.
  run iptables -t nat -A PREROUTING -i wan0 -p udp -j DNAT --to-destination $CLIENT
  ;;
rc)
  restore_plain_uplink
  reset_rules
  run iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB
  run iptables -t nat -A PREROUTING -i wan0 -p udp -j DNAT --to-destination $CLIENT
  # Address-restricted filtering: remember every IP the client sends to,
  # admit inbound UDP only from those IPs (any port), drop the rest.
  run iptables -A FORWARD -i lan0 -o wan0 -m recent --name peers --rdest --set -j ACCEPT
  run iptables -A FORWARD -i wan0 -o lan0 -m state --state ESTABLISHED,RELATED -j ACCEPT
  run iptables -A FORWARD -i wan0 -o lan0 -p udp -m recent --name peers --rsource --rcheck --seconds 120 -j ACCEPT
  run iptables -A FORWARD -i wan0 -o lan0 -p udp -j DROP
  ;;
sym)
  restore_plain_uplink
  reset_rules
  # Random port allocation = endpoint dependent mapping = the advertised
  # STUN port is useless to peers. Non-UDP keeps the plain SNAT.
  run iptables -t nat -A POSTROUTING -o wan0 -p udp -j SNAT --to-source $PUB:20000-40000 --random
  run iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB
  ;;
blk)
  restore_plain_uplink
  reset_rules
  run iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB
  run iptables -A FORWARD -p udp -d $COORD_IP --dport $COORD_UDP -j ACCEPT
  # The NAT-check second STUN port must stay reachable too.
  run iptables -A FORWARD -p udp -d $COORD_IP --dport 37503 -j ACCEPT
  run iptables -A FORWARD -i wan0 -p udp -m state --state ESTABLISHED,RELATED -j ACCEPT
  run iptables -A FORWARD -p udp -j DROP
  ;;
cgn)
  # client -> routerX (home prc NAT, CGN space) -> carrierX (sym NAT on the
  # public IP) -> inet. The world still sees $PUB, now symmetric.
  sudo ip netns add "$CAR" 2>/dev/null || true
  sudo ip netns exec "$CAR" ip link set lo up
  sudo ip netns exec "$CAR" sysctl -q -w net.ipv4.ip_forward=1

  sudo ip -n inet link del wan$R 2>/dev/null || true
  sudo ip -n "$R" link del wan0 2>/dev/null || true
  sudo ip -n "$CAR" link del wan0 2>/dev/null || true
  sudo ip -n "$CAR" link del lan0 2>/dev/null || true

  sudo ip -n inet link add wan$R type veth peer name wan0 netns "$CAR"
  sudo ip netns exec inet ip addr replace $WANNET.1/24 dev wan$R
  sudo ip netns exec inet ip link set wan$R up
  sudo ip netns exec "$CAR" ip addr replace $PUB/24 dev wan0
  sudo ip netns exec "$CAR" ip link set wan0 up
  sudo ip netns exec "$CAR" ip route replace default via $WANNET.1

  sudo ip -n "$CAR" link add lan0 type veth peer name wan0 netns "$R"
  sudo ip netns exec "$CAR" ip addr replace $CGNNET.1/24 dev lan0
  sudo ip netns exec "$CAR" ip link set lan0 up
  run ip addr replace $CGNNET.2/24 dev wan0
  run ip link set wan0 up
  run ip route replace default via $CGNNET.1

  reset_rules
  run iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $CGNNET.2
  sudo ip netns exec "$CAR" iptables -t nat -F POSTROUTING
  sudo ip netns exec "$CAR" iptables -t nat -A POSTROUTING -o wan0 -p udp -j SNAT --to-source $PUB:20000-40000 --random
  sudo ip netns exec "$CAR" iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source $PUB
  sudo ip netns exec "$CAR" conntrack -F >/dev/null 2>&1 || true
  ;;
*)
  echo "unknown NAT type: $TYPE" >&2
  exit 1
  ;;
esac

echo "router$L shaped as $TYPE (public $PUB, client $CLIENT)"
