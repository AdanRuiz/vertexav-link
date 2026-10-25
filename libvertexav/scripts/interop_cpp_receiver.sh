#!/usr/bin/env bash
# Interop check: C++ receiver (vxdump) against the Python simulator in auth mode.
# Usage: scripts/interop_cpp_receiver.sh <build-dir> <python>
set -u
BUILD=${1:-/tmp/vxbuild}
PY=${2:-python3}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE/tools"
PSK=$($PY -m vertexav.sim --gen-psk)
$PY -m vertexav.sim --fast --host 127.0.0.1 --no-announce --security auth --psk "$PSK" \
    --start-beat 56 --duration 50 > /tmp/simw.log 2>&1 &
sleep 1.5
timeout 60 "$BUILD/vxdump" --psk "$PSK" --duration 45 --quiet > /tmp/vxdump.log 2>&1
wait
grep -E "status|summary" /tmp/vxdump.log
echo "beats: $(grep -c '\[beat\]' /tmp/vxdump.log)"
grep -E "\[event\]|\[section\]|\[trigger\]" /tmp/vxdump.log | cut -c1-150
awk '/\[beat\]/{ if (prev != "" && $2 != prev + 1 && $0 !~ /change/) print "BEAT GAP after", prev, ":", $0; prev = $2 }' /tmp/vxdump.log
