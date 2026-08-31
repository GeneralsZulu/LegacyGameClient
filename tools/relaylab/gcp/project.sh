#!/bin/bash
# One-time: create the isolated relaylab project, link billing, enable the
# compute API, create the game-asset bucket, and upload the game install
# (only when the bucket copy is missing or stale). Idempotent.
set -euo pipefail
cd "$(dirname "$0")" && source ./env.sh

if ! gcloud projects describe "$PROJECT" >/dev/null 2>&1; then
  echo "[project] creating $PROJECT"
  gcloud projects create "$PROJECT" --name="relaylab"
else
  echo "[project] $PROJECT exists"
fi

echo "[project] linking billing $BILLING_ACCOUNT"
gcloud billing projects link "$PROJECT" --billing-account="$BILLING_ACCOUNT" >/dev/null

echo "[project] enabling APIs (compute, iap: first time takes a minute)"
$GC services enable compute.googleapis.com iap.googleapis.com >/dev/null

if ! gcloud storage buckets describe "$BUCKET" --project="$PROJECT" >/dev/null 2>&1; then
  echo "[project] creating bucket $BUCKET"
  gcloud storage buckets create "$BUCKET" --project="$PROJECT" --location=US \
    --uniform-bucket-level-access
fi

# Game assets: the pristine install plus the per-user data (maps + inis).
GAME_DIR=${GAME_DIR:-/home/hrich/zrun/game}
USERDATA_DIR="$HOME/Documents/Command and Conquer Generals Zero Hour Data"
STAMP=/tmp/relaylab-game-tar.stamp
if ! gcloud storage ls "$BUCKET/game.tgz" >/dev/null 2>&1; then
  echo "[project] packing + uploading the game install (~1.8 GB, one time)"
  TAR=/tmp/relaylab-game.tgz
  tar -czf "$TAR" \
    --exclude='Zulu_*.big' --exclude='*.exe.bak' \
    -C "$(dirname "$GAME_DIR")" "$(basename "$GAME_DIR")" \
    -C "$HOME/Documents" "Command and Conquer Generals Zero Hour Data/options.ini" \
       "Command and Conquer Generals Zero Hour Data/Skirmish.ini" \
       "Command and Conquer Generals Zero Hour Data/Maps"
  gcloud storage cp "$TAR" "$BUCKET/game.tgz"
  rm -f "$TAR"
else
  echo "[project] $BUCKET/game.tgz already present"
fi
echo "[project] done"
