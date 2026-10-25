#!/usr/bin/env bash
# Builds libvertexav, its tests and the QLC+ plugin with Qt 5 (QLC+ 4 is a Qt 5 application).
# Usage: libvertexav/scripts/build-qt5.sh <qlcplus-source>
set -eu
QLC=${1:?path to the QLC+ source checkout}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cmake -S "$ROOT/libvertexav" -B "$HOME/vxbuild5" -G Ninja -DVERTEXAV_QT_MAJOR=5 -DVERTEXAV_BUILD_TESTS=ON -DVERTEXAV_BUILD_TOOLS=ON > /dev/null
cmake --build "$HOME/vxbuild5" 2>&1 | grep -E "error|warning:" || true
"$HOME/vxbuild5/vertexav_tests" 2>&1 | grep -E "Totals|FAIL"
mkdir -p "$HOME/qlcbuild5"
cmake -S "$ROOT/integrations/qlcplus/vertexav" -B "$HOME/qlcbuild5" -G Ninja -DQLCPLUS_SOURCE_DIR="$QLC" \
    -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt5 -DQt6_DIR=/nonexistent > /dev/null
cmake --build "$HOME/qlcbuild5" 2>&1 | grep -E "error|warning:|Linking CXX shared" || true
