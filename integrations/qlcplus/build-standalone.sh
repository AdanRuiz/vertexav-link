#!/usr/bin/env bash
# Builds the VertexAV plugin and its test harness against a QLC+ source checkout.
# Usage: integrations/qlcplus/build-standalone.sh <qlcplus-source> [build-dir]
set -eu
QLC=${1:?path to the QLC+ source checkout}
BUILD=${2:-$HOME/qlcbuild}
HERE=$(cd "$(dirname "$0")" && pwd)
cmake -S "$HERE/vertexav" -B "$BUILD" -G Ninja -DQLCPLUS_SOURCE_DIR="$QLC" > "$BUILD.cmake.log" 2>&1 || { tail -30 "$BUILD.cmake.log"; exit 1; }
cmake --build "$BUILD"
