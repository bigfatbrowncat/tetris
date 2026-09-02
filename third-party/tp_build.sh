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
trap 'rmdir "$LOCK_DIR" 2>/dev/null' EXIT

exec make -C "$PROJ" "config=$CFG" "$@"
