#!/usr/bin/env bash
# Build third-party components (bx bimg bgfx shaderc) via the gmake project.
#
# The gmake project cannot tolerate concurrent `make` invocations: component
# recipes do `rm -f` + `ar`/`ld` on shared outputs, so parallel invocations
# corrupt each other's artifacts. CMake's Makefile generator also duplicates
# this custom command's rule into each dependent target's sub-make, so a
# parallel `cmake --build` can launch several invocations at once. Serialize
# with an atomic mkdir lock.
#
# Usage: tp_build.sh <lock-dir> <project-dir> <config> <goal> [goal ...]
set -euo pipefail

LOCK_DIR="$1"; shift
PROJ="$1"; shift
CFG="$1"; shift

while true; do
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        echo $$ > "$LOCK_DIR/pid"
        break
    fi
    # Steal the lock if its holder is gone (killed build).
    HOLDER="$(cat "$LOCK_DIR/pid" 2>/dev/null || true)"
    if [ -n "$HOLDER" ] && ! kill -0 "$HOLDER" 2>/dev/null; then
        rm -rf "$LOCK_DIR" 2>/dev/null || true
        continue
    fi
    sleep 1
done
# The EXIT trap removes the lock on EVERY exit path, so a stale lock can only
# ever come from a killed build (the steal above handles that). Do not `exec`
# make: exec replaces this shell and clears the trap, leaving a stale lock
# whose dead pid lets the next run steal the lock from its new holder.
trap 'rm -rf "$LOCK_DIR" 2>/dev/null' EXIT

# Local bgfx patches (kept in the superproject, outside the submodule): the
# submodule is expected to sit at a pristine upstream commit, so apply our
# patches to the working tree before building. The marker check keeps each
# step idempotent: no-op when already applied, re-applies after a
# `git submodule update` reset the tree. (The gmake project lives at
# <bgfx>/.build/projects/<name>, so the bgfx root is three levels up and the
# third-party dir one more.)
BGFX_ROOT="$(cd "$PROJ/../../.." && pwd)"
BGFX_PATCH_DIR="$(cd "$BGFX_ROOT/.." && pwd)/patches/bgfx"
# bgfx_apply_patch <patch file> <marker file, relative to bgfx root> <marker>
# Apply the patch unless its marker string is already present in the marker
# file (i.e. the patch — or a later state of the tree — already carries it).
bgfx_apply_patch() {
    local file="$1" marker_file="$2" marker="$3"
    if [ -f "$BGFX_PATCH_DIR/$file" ] && ! grep -q "$marker" "$BGFX_ROOT/$marker_file"; then
        echo "[tp] applying $file"
        patch -p1 -d "$BGFX_ROOT" --silent < "$BGFX_PATCH_DIR/$file"
    fi
}
# 0000 adds the getTextureDmaBuf() API (declared in bgfx.h).
bgfx_apply_patch "0000-dma-buf-texture-export.patch" "include/bgfx/bgfx.h" "getTextureDmaBuf"
# 0001 fixes the Vulkan dma-buf export on drivers that reject the queried
# format modifier: the modifier query must resolve through GIPA, and a failed
# tiled vkCreateImage falls back to linear (marked by the retry trace in
# renderer_vk.cpp).
bgfx_apply_patch "0001-dmabuf-tiled-fallback.patch" "src/renderer_vk.cpp" "tiled dma-buf image failed"

make -C "$PROJ" "config=$CFG" "$@"
