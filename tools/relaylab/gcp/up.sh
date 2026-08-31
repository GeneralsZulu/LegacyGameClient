#!/bin/bash
# up.sh: provision the relaylab fleet: 3 VPCs, 2 Cloud NAT gateways (one
# EIM-enabled = cone, one EIM-disabled = symmetric), the coordinator VM and
# 8 client VMs from the relaylab-client image, then wait for ssh and push
# the fresh build + remote scripts. Idempotent: every create is skipped if
# the resource exists. Everything carries label relaylab=1 inside the
# dedicated project, so down.sh can never touch anything else.
set -euo pipefail
cd "$(dirname "$0")" && source ./env.sh

exists() { $GC compute "$1" describe "$2" "${@:3}" >/dev/null 2>&1; }

echo "[up] networks + subnets"
exists networks relaylab-pub  || $GC compute networks create relaylab-pub  --subnet-mode=custom >/dev/null
exists networks relaylab-cone || $GC compute networks create relaylab-cone --subnet-mode=custom >/dev/null
exists networks relaylab-sym  || $GC compute networks create relaylab-sym  --subnet-mode=custom >/dev/null
sn() { $GC compute networks subnets describe "$2" --region="$3" >/dev/null 2>&1 || $GC compute networks subnets create "$2" --network="$1" --region="$3" --range="$4" >/dev/null; }
sn relaylab-pub  pub-usc  us-central1  10.10.1.0/24
sn relaylab-pub  pub-euw  europe-west1 10.10.2.0/24
sn relaylab-pub  pub-usw2 us-west2     10.10.3.0/24
sn relaylab-cone cone-usw us-west1     10.20.1.0/24
sn relaylab-sym  sym-use  us-east1     10.30.1.0/24

echo "[up] firewall"
fw() { exists firewall-rules "$1" || $GC compute firewall-rules create "$@" >/dev/null; }
fw relaylab-pub-ssh    --network=relaylab-pub --allow=tcp:22 --source-ranges=0.0.0.0/0
fw relaylab-pub-coord  --network=relaylab-pub --allow=tcp:$COORD_TCP,tcp:$COORD_STATUS,udp:$COORD_UDP,udp:37503 --source-ranges=0.0.0.0/0 --target-tags=relaylab-coord
# The in-VM netns NATs are the NAT under test; GCP's firewall must admit
# raw punch traffic so only the simulated NAT decides.
fw relaylab-pub-udp    --network=relaylab-pub --allow=udp --source-ranges=0.0.0.0/0 --target-tags=relaylab-client
fw relaylab-cone-iap   --network=relaylab-cone --allow=tcp:22 --source-ranges=35.235.240.0/20
fw relaylab-cone-int   --network=relaylab-cone --allow=tcp,udp,icmp --source-ranges=10.20.1.0/24
fw relaylab-sym-iap    --network=relaylab-sym --allow=tcp:22 --source-ranges=35.235.240.0/20
fw relaylab-sym-int    --network=relaylab-sym --allow=tcp,udp,icmp --source-ranges=10.30.1.0/24

echo "[up] Cloud NAT (cone: EIM on, shared static IP; sym: EIM off)"
exists addresses relaylab-cone-natip --region=us-west1 || \
  $GC compute addresses create relaylab-cone-natip --region=us-west1 >/dev/null
exists routers relaylab-router-cone --region=us-west1 || \
  $GC compute routers create relaylab-router-cone --network=relaylab-cone --region=us-west1 >/dev/null
$GC compute routers nats describe relaylab-nat-cone --router=relaylab-router-cone --region=us-west1 >/dev/null 2>&1 || \
  $GC compute routers nats create relaylab-nat-cone --router=relaylab-router-cone --region=us-west1 \
    --nat-all-subnet-ip-ranges --nat-external-ip-pool=relaylab-cone-natip \
    --enable-endpoint-independent-mapping --udp-idle-timeout=30s --min-ports-per-vm=64 >/dev/null
exists routers relaylab-router-sym --region=us-east1 || \
  $GC compute routers create relaylab-router-sym --network=relaylab-sym --region=us-east1 >/dev/null
$GC compute routers nats describe relaylab-nat-sym --router=relaylab-router-sym --region=us-east1 >/dev/null 2>&1 || \
  $GC compute routers nats create relaylab-nat-sym --router=relaylab-router-sym --region=us-east1 \
    --nat-all-subnet-ip-ranges --auto-allocate-nat-external-ips \
    --udp-idle-timeout=30s --min-ports-per-vm=64 >/dev/null

echo "[up] VMs"
subnet_for() { case "$1" in us-west1-*) echo cone-usw;; us-east1-*) echo sym-use;; europe-west1-*) echo pub-euw;; us-west2-*) echo pub-usw2;; us-central1-*) echo pub-usc;; esac; }
region_of() { echo "${1%-*}"; }

exists instances relaylab-coord --zone=$COORD_ZONE || \
  $GC compute instances create relaylab-coord --zone=$COORD_ZONE \
    --machine-type=$COORD_MACHINE --image-family=$IMAGE_FAMILY --image-project=$PROJECT \
    --subnet=projects/$PROJECT/regions/$(region_of $COORD_ZONE)/subnetworks/pub-usc \
    --tags=relaylab-coord --labels=$LABEL >/dev/null &

for row in "${CLIENTS[@]}"; do
  set -- $row
  NAME=relaylab-$1 ZONE=$2 NET=$3
  SUB=$(subnet_for "$ZONE")
  ADDR_FLAG=""
  [ "$NET" != relaylab-pub ] && ADDR_FLAG="--no-address"
  exists instances $NAME --zone=$ZONE || \
    $GC compute instances create $NAME --zone=$ZONE \
      --machine-type=$CLIENT_MACHINE --image-family=$IMAGE_FAMILY --image-project=$PROJECT \
      --subnet=projects/$PROJECT/regions/$(region_of $ZONE)/subnetworks/$SUB $ADDR_FLAG \
      --tags=relaylab-client --labels=$LABEL >/dev/null &
done
wait

ssh_c() { # ssh_c <name> <zone> <net> -- command
  local NAME=relaylab-$1 ZONE=$2 NET=$3; shift 3
  local IAP=""
  [ "$NET" != relaylab-pub ] && IAP="--tunnel-through-iap"
  $GC compute ssh "$NAME" --zone="$ZONE" $IAP --command="$*"
}

echo "[up] waiting for ssh on all 9 VMs"
for row in "coord $COORD_ZONE relaylab-pub" "${CLIENTS[@]}"; do
  set -- $row
  NAME=relaylab-$1 ZONE=$2 NET=$3
  for i in $(seq 40); do
    ssh_c "$1" "$ZONE" "$NET" true >/dev/null 2>&1 && { echo "  $NAME up"; break; }
    [ $i -eq 40 ] && { echo "  $NAME NEVER came up"; exit 1; }
    sleep 8
  done &
done
wait 2>/dev/null || true

echo "[up] pushing build delta + remote scripts"
REPO="$(cd ../../.. && pwd)"
scp_c() { # scp_c <name> <zone> <net> <src...> <dst>
  local NAME=relaylab-$1 ZONE=$2 NET=$3; shift 3
  local IAP=""
  [ "$NET" != relaylab-pub ] && IAP="--tunnel-through-iap"
  local DST=${!#}
  local SRCS=("${@:1:$(($#-1))}")
  $GC compute scp --zone="$ZONE" $IAP "${SRCS[@]}" "$NAME:$DST"
}
for row in "${CLIENTS[@]}"; do
  set -- $row
  (
    scp_c "$1" "$2" "$3" "$REPO/build/docker-vc6/GeneralsMD/generalszh.exe" /opt/zh/generalszh.exe >/dev/null
    scp_c "$1" "$2" "$3" "$REPO/build/installer-tmp/Zulu.big" /opt/zh/Zulu.big >/dev/null
    scp_c "$1" "$2" "$3" remote/client.sh remote/vm-nat.sh "~/" >/dev/null
    ssh_c "$1" "$2" "$3" "chmod +x ~/client.sh ~/vm-nat.sh" >/dev/null
    echo "  $1 pushed"
  ) &
done
wait
echo "[up] done"
