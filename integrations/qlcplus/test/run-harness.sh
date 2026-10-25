#!/usr/bin/env bash
# Runs the Python simulator and the plugin harness against it.
# Usage: integrations/qlcplus/test/run-harness.sh <build-dir> <python> [seconds]
set -u
BUILD=${1:-$HOME/qlcbuild}
PY=${2:-python3}
SECS=${3:-35}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT/tools"
$PY -m vertexav.sim --fast --host 127.0.0.1 --no-announce --start-beat 20 --quiet --duration $((SECS + 5)) &
SIM=$!
sleep 1.5
QT_QPA_PLATFORM=offscreen "$BUILD/vertexav_plugin_harness" 127.0.0.1 9470 "$SECS" 0
CODE=$?
kill $SIM 2>/dev/null
wait 2>/dev/null
exit $CODE
