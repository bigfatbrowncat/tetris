#!/usr/bin/env bash
# Compile the .sc shaders to Metal and emit C-embedded headers (vs_quad.h / fs_quad.h).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BIN:-third-party/bgfx/.build/osx-arm64/bin/shadercRelease}"
BGFX="${BGFX:-third-party/bgfx}"

"$BIN" -f shaders/vs_quad.sc -o shaders/vs_quad.h \
    --type v --platform osx -p metal --bin2c vs_quad -i "$BGFX/src/"
"$BIN" -f shaders/fs_quad.sc -o shaders/fs_quad.h \
    --type f --platform osx -p metal --bin2c fs_quad -i "$BGFX/src/"

echo "wrote shaders/vs_quad.h and shaders/fs_quad.h"
