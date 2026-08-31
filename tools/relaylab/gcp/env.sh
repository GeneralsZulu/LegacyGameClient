#!/bin/bash
# Shared configuration for the relaylab GCP scripts. Everything lives in its
# own project so the lab can never touch other GCP setups; down.sh deletes
# by label inside that project only.

export PROJECT=${RELAYLAB_PROJECT:-relaylab-zulu-1}
export BILLING_ACCOUNT=${RELAYLAB_BILLING:-01A214-9716B8-5958BA}   # same account as cncstats
export BUCKET="gs://$PROJECT-zh"
export IMAGE_FAMILY=relaylab-client
export LABEL=relaylab=1

# One gcloud invocation per resource; every resource is labeled.
export GC="gcloud --project=$PROJECT"

# Machine types: wine + software rendering needs about one busy core and
# ~1.5 GB; e2-standard-2 keeps soaks stall-free. Coordinator is tiny.
export CLIENT_MACHINE=e2-standard-2
export COORD_MACHINE=e2-small

# Topology (test plan section 3.2). "cloud" NAT classes are delivered by
# Cloud NAT gateways; the rest are netns NATs inside the VM (vm-nat.sh).
#   name  zone            network        nat
export CLIENTS=(
  "c1 us-west1-b     relaylab-cone  cloud-cone"
  "c2 us-west1-b     relaylab-cone  cloud-cone"
  "c3 us-east1-b     relaylab-sym   cloud-sym"
  "c4 us-east1-b     relaylab-sym   cloud-sym"
  "c5 europe-west1-b relaylab-pub   prc"
  "c6 europe-west1-b relaylab-pub   blk"
  "c7 us-west2-b     relaylab-pub   cgn"
  "c8 us-central1-b  relaylab-pub   fc"
)
export COORD_ZONE=us-central1-b
export COORD_NET=relaylab-pub
export COORD_TCP=37500
export COORD_UDP=37501
export COORD_STATUS=37502

client_row() { # client_row c3 -> echoes its row
  local want=$1 row
  for row in "${CLIENTS[@]}"; do
    set -- $row
    [ "$1" = "$want" ] && { echo "$row"; return 0; }
  done
  return 1
}
