#!/bin/bash
# natlab-down.sh v4: tear down everything natlab-up.sh created.
set -uo pipefail
for L in A B C D E F; do
  sudo ip netns del client$L 2>/dev/null
  sudo ip netns del router$L 2>/dev/null
done
sudo ip netns del inet 2>/dev/null
sudo ip link del vethinet 2>/dev/null
for i in 1 2 3 4 5 6; do sudo ip route del 10.99.$((i*10)).0/24 2>/dev/null; done
echo "natlab down"
