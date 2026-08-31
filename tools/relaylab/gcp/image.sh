#!/bin/bash
# One-time bake of the relaylab client image: Ubuntu 24.04 + wine(i386) +
# Xvfb + the game install from $BUCKET/game.tgz unpacked at /opt/zh, plus a
# ready wine prefix at /opt/zh-pref (registry install paths, private
# Documents with maps/options, 1024x768). VMs created from this image can
# launch the game within seconds of boot; per run only the exe/Zulu.big
# delta is pushed.
#
# Produces image family "relaylab-client" (relaylab-client-vN). Rerun to
# bake a new version; up.sh always uses the newest in the family.
set -euo pipefail
cd "$(dirname "$0")" && source ./env.sh

BAKE=relaylab-bake
ZONE=us-central1-b
VER=v$(date +%Y%m%d%H%M)

echo "[image] creating bake VM"
$GC compute instances create $BAKE --zone=$ZONE \
  --machine-type=e2-standard-2 --image-family=ubuntu-2404-lts-amd64 \
  --image-project=ubuntu-os-cloud --boot-disk-size=20GB \
  --labels=$LABEL --scopes=storage-ro >/dev/null

echo "[image] waiting for ssh"
for i in $(seq 30); do
  $GC compute ssh $BAKE --zone=$ZONE --command=true 2>/dev/null && break
  sleep 10
done

echo "[image] installing packages + game (takes ~10 min)"
$GC compute ssh $BAKE --zone=$ZONE --command='set -e
sudo dpkg --add-architecture i386
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  wine wine32:i386 xvfb xdotool imagemagick tcpdump conntrack iptables iproute2 python3 curl > /dev/null
echo "[bake] packages done"
gcloud storage cp '"$BUCKET"'/game.tgz /tmp/game.tgz
sudo mkdir -p /opt && cd /tmp && tar -xzf game.tgz
sudo mv /tmp/game /opt/zh
sudo mkdir -p /opt/zh-userdata
sudo mv "/tmp/Command and Conquer Generals Zero Hour Data"/* /opt/zh-userdata/ 2>/dev/null || true
sudo rm -f /opt/zh/Zulu_*.big
sudo chown -R $USER /opt/zh /opt/zh-userdata
echo "[bake] game unpacked"

# Wine prefix template, used directly as the runtime prefix.
# Xvfb must be fully detached or it holds this ssh session open forever
# after the script body finishes (learned the hard way on the first bake).
nohup Xvfb :91 -screen 0 1024x768x24 >/dev/null 2>&1 </dev/null & sleep 2
export DISPLAY=:91 WINEPREFIX=/opt/zh-pref WINEDEBUG=-all
sudo mkdir -p /opt/zh-pref && sudo chown $USER /opt/zh-pref
wineboot -i >/dev/null 2>&1
wine reg add "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Generals" /v InstallPath /t REG_SZ /d "Z:\\opt\\zh\\" /f /reg:32
wine reg add "HKLM\\SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour" /v InstallPath /t REG_SZ /d "Z:\\opt\\zh\\" /f /reg:32
wineserver -k || true
U=/opt/zh-pref/drive_c/users/$USER
rm -f "$U/Documents"
D="$U/Documents/Command and Conquer Generals Zero Hour Data"
mkdir -p "$D"
cp -r /opt/zh-userdata/Maps "$D/" || true
cp /opt/zh-userdata/options.ini /opt/zh-userdata/Skirmish.ini "$D/" || true
sed -i "s/^Resolution = .*/Resolution = 1024 768/" "$D/options.ini" || true
# 8-player official map (Death Valley), QuotedPrintable per the wine runbook.
printf "Map = maps_5Cdeath_20valley_5Cdeath_20valley_2Emap\n" > "$D/Network.ini"
pkill Xvfb || true
echo "[bake] wine prefix ready"'

echo "[image] stopping + imaging"
$GC compute instances stop $BAKE --zone=$ZONE >/dev/null
$GC compute images create relaylab-client-$VER \
  --source-disk=$BAKE --source-disk-zone=$ZONE \
  --family=$IMAGE_FAMILY --labels=$LABEL >/dev/null
$GC compute instances delete $BAKE --zone=$ZONE --quiet >/dev/null
echo "[image] baked relaylab-client-$VER (family $IMAGE_FAMILY)"
