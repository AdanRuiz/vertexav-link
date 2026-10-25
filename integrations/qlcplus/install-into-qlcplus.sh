#!/usr/bin/env bash
# Copies the VertexAV plugin (and libvertexav) into a QLC+ source tree and registers it in
# plugins/CMakeLists.txt. Then build QLC+ as usual.
# Usage: integrations/qlcplus/install-into-qlcplus.sh /path/to/qlcplus
set -eu
QLC=${1:?path to the QLC+ source tree}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DEST="$QLC/plugins/vertexav"

if [ ! -f "$QLC/plugins/interfaces/qlcioplugin.h" ]; then
    echo "$QLC does not look like a QLC+ source tree" >&2
    exit 1
fi
rm -rf "$DEST"
mkdir -p "$DEST/libvertexav"
cp "$HERE"/vertexav/*.cpp "$HERE"/vertexav/*.h "$HERE"/vertexav/CMakeLists.txt "$HERE"/vertexav/*.xml "$DEST/"
cp -r "$ROOT/libvertexav/CMakeLists.txt" "$ROOT/libvertexav/include" "$ROOT/libvertexav/src" "$DEST/libvertexav/"
if ! grep -q "add_subdirectory(vertexav)" "$QLC/plugins/CMakeLists.txt"; then
    sed -i 's/^add_subdirectory(os2l)$/add_subdirectory(os2l)\nadd_subdirectory(vertexav)/' "$QLC/plugins/CMakeLists.txt"
fi
grep -n "add_subdirectory(vertexav)" "$QLC/plugins/CMakeLists.txt"
echo "VertexAV plugin installed in $DEST"
