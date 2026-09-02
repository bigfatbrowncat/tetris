#!/usr/bin/env python3
"""Compile the .sc shaders and emit C-embedded headers.

macOS  -> Metal    headers in shaders/     (vs_quad.h / fs_quad.h)
Linux  -> OpenGL   headers in shaders/gl/  (staged into the build dir by CMake)
"""
import os
import subprocess
import sys

# Project root is the parent of this script's directory.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IS_MACOS = sys.platform == "darwin"
PLATFORM = "osx" if IS_MACOS else "linux"
# bgfx's GL renderer strips any #version and prepends its own (#version 430
# by default on Linux), so the GLSL profile must match that.
PROFILE = "metal" if IS_MACOS else "430"
OUT_DIR = "" if IS_MACOS else "gl/"
TP_BIN = "osx-arm64/bin" if IS_MACOS else "linux64_gcc/bin"

# BIN may be the build dir (as CMake sets it) or the shadercRelease
# binary path; normalise to the binary.
BIN = os.environ.get("BIN", os.path.join("third-party/bgfx/.build", TP_BIN))
if os.path.isdir(BIN):
    BIN = os.path.join(BIN, "shadercRelease")
BGFX = os.environ.get("BGFX", "third-party/bgfx")


def build(src, out, type_flag, name):
    cmd = [
        BIN,
        "-f", f"shaders/{src}", "-o", f"shaders/{OUT_DIR}{out}",
        "--type", type_flag, "--platform", PLATFORM, "-p", PROFILE,
        "--bin2c", name, "-i", f"{BGFX}/src/",
    ]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    try:
        os.makedirs(f"shaders/{OUT_DIR}", exist_ok=True)
        build("vs_quad.sc", "vs_quad.h", "v", "vs_quad")
        build("fs_quad.sc", "fs_quad.h", "f", "fs_quad")
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"build_shaders failed: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"wrote shaders/{OUT_DIR}vs_quad.h and shaders/{OUT_DIR}fs_quad.h")


if __name__ == "__main__":
    main()
