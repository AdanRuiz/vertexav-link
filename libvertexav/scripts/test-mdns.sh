#!/usr/bin/env bash
# DNS-SD interop: C++ responder -> Python zeroconf browser, and Python announcer -> C++ browser.
# Usage: libvertexav/scripts/test-mdns.sh <build-dir> <python>
set -u
BUILD=${1:-$HOME/vxbuild}
PY=${2:-python3}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT/tools"
echo "== C++ vxemit announces, Python discovers =="
"$BUILD/vxemit" --name "vxemit mdns test" --duration 12 > /tmp/vxemit_mdns.log 2>&1 &
sleep 1.5
timeout 20 $PY -m vertexav.inspector --discover --duration 4 --quiet 2>&1 | grep -E "→|No se encontró|estado conectado|errores"
wait
echo "== Python sim announces, C++ vxdump discovers =="
$PY -m vertexav.sim --fast --name "Python sim mdns" --duration 12 --quiet > /tmp/sim_mdns.log 2>&1 &
sleep 2
timeout 15 "$BUILD/vxdump" --discover --duration 5 --quiet 2>&1 | grep -E "dns-sd|status|summary"
wait
