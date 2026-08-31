#!/bin/bash
# vm-nat.sh <prc|fc|rc|sym|cgn|blk> <coordIP>
#
# Runs ON a relaylab client VM. Puts the game into a "gns" netns behind an
# in-VM NAT of the requested type, mirroring tools/coordinator/natlab/
# nat-shape.sh but with the VM's root namespace as the router. GCP's own
# 1:1 external NAT is port-preserving and transparent, so the shape the
# internet sees is the shape programmed here.
set -euo pipefail
TYPE=${1:?nat type}
COORD_IP=${2:?coordinator ip}
COORD_UDP=${3:-37501}

NIC=$(ip route get 8.8.8.8 | grep -oP 'dev \K\S+')
NICIP=$(ip route get 8.8.8.8 | grep -oP 'src \K\S+')

sudo sysctl -q -w net.ipv4.ip_forward=1

# Fresh namespaces each apply.
sudo ip netns del gns 2>/dev/null || true
sudo ip netns del cgnh 2>/dev/null || true
sudo ip link del v0 2>/dev/null || true

sudo ip netns add gns
sudo ip netns exec gns ip link set lo up

if [ "$TYPE" = cgn ]; then
  # gns(10.79.0.2) <-> cgnh home prc NAT <-> root ns sym NAT <-> internet
  sudo ip netns add cgnh
  sudo ip netns exec cgnh ip link set lo up
  sudo ip netns exec cgnh sysctl -q -w net.ipv4.ip_forward=1
  sudo ip link add v0 type veth peer name wan0 netns cgnh
  sudo ip addr add 10.78.0.1/30 dev v0
  sudo ip link set v0 up
  sudo ip netns exec cgnh ip addr add 10.78.0.2/30 dev wan0
  sudo ip netns exec cgnh ip link set wan0 up
  sudo ip netns exec cgnh ip route add default via 10.78.0.1
  sudo ip -n cgnh link add lan0 type veth peer name eth0 netns gns
  sudo ip netns exec cgnh ip addr add 10.79.0.1/30 dev lan0
  sudo ip netns exec cgnh ip link set lan0 up
  sudo ip netns exec gns ip addr add 10.79.0.2/30 dev eth0
  sudo ip netns exec gns ip link set eth0 up
  sudo ip netns exec gns ip route add default via 10.79.0.1
  sudo ip netns exec cgnh iptables -t nat -F POSTROUTING
  sudo ip netns exec cgnh iptables -t nat -A POSTROUTING -o wan0 -j SNAT --to-source 10.78.0.2
  SRC=10.78.0.0/30
else
  sudo ip link add v0 type veth peer name eth0 netns gns
  sudo ip addr add 10.77.0.1/30 dev v0
  sudo ip link set v0 up
  sudo ip netns exec gns ip addr add 10.77.0.2/30 dev eth0
  sudo ip netns exec gns ip link set eth0 up
  sudo ip netns exec gns ip route add default via 10.77.0.1
  SRC=10.77.0.0/30
fi
CLIENT=10.77.0.2

# Root-namespace rules: clear only OUR chains/rules (never a blanket -F on
# a VM that also carries ssh).
sudo iptables -t nat -N RELAYLAB_POST 2>/dev/null || sudo iptables -t nat -F RELAYLAB_POST
sudo iptables -t nat -N RELAYLAB_PRE 2>/dev/null || sudo iptables -t nat -F RELAYLAB_PRE
sudo iptables -N RELAYLAB_FWD 2>/dev/null || sudo iptables -F RELAYLAB_FWD
sudo iptables -t nat -C POSTROUTING -j RELAYLAB_POST 2>/dev/null || sudo iptables -t nat -A POSTROUTING -j RELAYLAB_POST
sudo iptables -t nat -C PREROUTING -j RELAYLAB_PRE 2>/dev/null || sudo iptables -t nat -A PREROUTING -j RELAYLAB_PRE
sudo iptables -C FORWARD -j RELAYLAB_FWD 2>/dev/null || sudo iptables -I FORWARD -j RELAYLAB_FWD
sudo conntrack -F >/dev/null 2>&1 || true

post() { sudo iptables -t nat -A RELAYLAB_POST "$@"; }
pre()  { sudo iptables -t nat -A RELAYLAB_PRE "$@"; }
fwd()  { sudo iptables -A RELAYLAB_FWD "$@"; }

case "$TYPE" in
prc)
  post -s $SRC -o "$NIC" -j SNAT --to-source "$NICIP"
  ;;
fc)
  post -s $SRC -o "$NIC" -j SNAT --to-source "$NICIP"
  pre -i "$NIC" -p udp -j DNAT --to-destination $CLIENT
  ;;
rc)
  post -s $SRC -o "$NIC" -j SNAT --to-source "$NICIP"
  pre -i "$NIC" -p udp -j DNAT --to-destination $CLIENT
  fwd -i v0 -o "$NIC" -m recent --name peers --rdest --set -j ACCEPT
  fwd -i "$NIC" -o v0 -m state --state ESTABLISHED,RELATED -j ACCEPT
  fwd -i "$NIC" -o v0 -p udp -m recent --name peers --rsource --rcheck --seconds 120 -j ACCEPT
  fwd -i "$NIC" -o v0 -p udp -j DROP
  ;;
sym)
  post -s $SRC -o "$NIC" -p udp -j SNAT --to-source "$NICIP:20000-40000" --random
  post -s $SRC -o "$NIC" -j SNAT --to-source "$NICIP"
  ;;
blk)
  post -s $SRC -o "$NIC" -j SNAT --to-source "$NICIP"
  fwd -p udp -d "$COORD_IP" --dport "$COORD_UDP" -j ACCEPT
  # NAT-check second STUN port.
  fwd -p udp -d "$COORD_IP" --dport 37503 -j ACCEPT
  fwd -i "$NIC" -p udp -m state --state ESTABLISHED,RELATED -j ACCEPT
  fwd -s $SRC -p udp -j DROP
  fwd -d $CLIENT -p udp -j DROP
  ;;
cgn)
  post -s $SRC -o "$NIC" -p udp -j SNAT --to-source "$NICIP:20000-40000" --random
  post -s $SRC -o "$NIC" -j SNAT --to-source "$NICIP"
  ;;
*)
  echo "unknown NAT type $TYPE" >&2; exit 1;;
esac

echo "vm-nat: $TYPE applied (nic=$NIC ip=$NICIP client netns=gns)"
