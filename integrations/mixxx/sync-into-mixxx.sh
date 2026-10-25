#!/usr/bin/env bash
# Copies libvertexav into a Mixxx checkout (lib/vertexav), where the Mixxx build picks it up
# when configured with -DVERTEXAV=ON.
# Usage: integrations/mixxx/sync-into-mixxx.sh /path/to/mixxx
set -eu
MIXXX=${1:?path to the Mixxx checkout}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST="$MIXXX/lib/vertexav"
if [ ! -f "$MIXXX/src/coreservices.cpp" ]; then
    echo "$MIXXX does not look like a Mixxx checkout" >&2
    exit 1
fi
rm -rf "$DEST"
mkdir -p "$DEST"
cp -r "$ROOT/libvertexav/CMakeLists.txt" "$ROOT/libvertexav/include" "$ROOT/libvertexav/src" "$DEST/"
cp "$ROOT/LICENSE" "$DEST/LICENSE"
REV=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
cat > "$DEST/README.md" <<TXT
# libvertexav (bundled)

Reference C++/Qt implementation of the VertexAV Link protocol, copied from the
vertexav-link repository (libvertexav/, revision $REV) by
integrations/mixxx/sync-into-mixxx.sh. Do not edit here: change it upstream and sync.
TXT
echo "libvertexav copied to $DEST"
