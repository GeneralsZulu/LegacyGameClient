#!/bin/bash
# down.sh [--everything]
#
# Tear down every billable relaylab resource: VMs, Cloud NATs, routers, the
# static NAT IP, firewall rules, subnets and networks. All of it lives in
# the dedicated relaylab project, so nothing outside it can be touched.
# The custom image and the game bucket are KEPT by default (the reusable
# template, ~$1/mo); --everything deletes those too.
set -uo pipefail
cd "$(dirname "$0")" && source ./env.sh

echo "[down] instances"
mapfile -t VMS < <($GC compute instances list --filter="labels.relaylab=1" --format="value(name,zone)" 2>/dev/null)
for row in "${VMS[@]}"; do
  [ -z "$row" ] && continue
  set -- $row
  echo "  deleting $1 ($2)"
  $GC compute instances delete "$1" --zone="$2" --quiet >/dev/null &
done
wait 2>/dev/null || true

echo "[down] Cloud NAT + routers + NAT IP"
$GC compute routers nats delete relaylab-nat-cone --router=relaylab-router-cone --region=us-west1 --quiet 2>/dev/null || true
$GC compute routers nats delete relaylab-nat-sym --router=relaylab-router-sym --region=us-east1 --quiet 2>/dev/null || true
$GC compute routers delete relaylab-router-cone --region=us-west1 --quiet 2>/dev/null || true
$GC compute routers delete relaylab-router-sym --region=us-east1 --quiet 2>/dev/null || true
$GC compute addresses delete relaylab-cone-natip --region=us-west1 --quiet 2>/dev/null || true

echo "[down] firewall rules"
for fw in $($GC compute firewall-rules list --filter="name~^relaylab-" --format="value(name)" 2>/dev/null); do
  $GC compute firewall-rules delete "$fw" --quiet >/dev/null 2>&1 &
done
wait 2>/dev/null || true

echo "[down] subnets + networks"
$GC compute networks subnets delete pub-usc  --region=us-central1  --quiet 2>/dev/null || true
$GC compute networks subnets delete pub-euw  --region=europe-west1 --quiet 2>/dev/null || true
$GC compute networks subnets delete pub-usw2 --region=us-west2     --quiet 2>/dev/null || true
$GC compute networks subnets delete cone-usw --region=us-west1     --quiet 2>/dev/null || true
$GC compute networks subnets delete sym-use  --region=us-east1     --quiet 2>/dev/null || true
for net in relaylab-pub relaylab-cone relaylab-sym; do
  $GC compute networks delete "$net" --quiet 2>/dev/null || true
done

if [ "${1:-}" = "--everything" ]; then
  echo "[down] images + bucket (--everything)"
  for img in $($GC compute images list --filter="family=$IMAGE_FAMILY" --format="value(name)" 2>/dev/null); do
    $GC compute images delete "$img" --quiet 2>/dev/null || true
  done
  gcloud storage rm -r "$BUCKET" 2>/dev/null || true
fi

echo "[down] remaining billable resources in $PROJECT:"
$GC compute instances list 2>/dev/null || true
$GC compute images list --filter="family=$IMAGE_FAMILY" --format="value(name,diskSizeGb)" 2>/dev/null || true
echo "[down] done"
