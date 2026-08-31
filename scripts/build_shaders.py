#!/usr/bin/env python3
"""Compile the .sc shaders to Metal and emit C-embedded headers (vs_quad.h / fs_quad.h)."""
import os
import subprocess
import sys

# Project root is the parent of this script's directory.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# BIN may be the build dir (as the Makefile sets it) or the shadercRelease
# binary path; normalise to the binary.
BIN = os.environ.get("BIN", "third-party/bgfx/.build/osx-arm64/bin")
if os.path.isdir(BIN):
    BIN = os.path.join(BIN, "shadercRelease")
BGFX = os.environ.get("BGFX", "third-party/bgfx")


def build(src, out, type_flag, name):
    cmd = [
        BIN,
        "-f", f"shaders/{src}", "-o", f"shaders/{out}",
        "--type", type_flag, "--platform", "osx", "-p", "metal",
        "--bin2c", name, "-i", f"{BGFX}/src/",
    ]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    try:
        build("vs_quad.sc", "vs_quad.h", "v", "vs_quad")
        build("fs_quad.sc", "fs_quad.h", "f", "fs_quad")
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"build_shaders failed: {e}", file=sys.stderr)
        sys.exit(1)
    print("wrote shaders/vs_quad.h and shaders/fs_quad.h")


if __name__ == "__main__":
    main()
