#!/usr/bin/env python3
"""Compile the .sc shaders and emit C-embedded headers.

Usage: build_shaders.py <out-dir> [shaderc-bin] [bgfx-root]

Writes ONLY to <out-dir> (a directory inside the CMake build tree):
  <out-dir>/vs_quad.h   <out-dir>/fs_quad.h

macOS  -> Metal shaders
Linux  -> SPIR-V (profile "spirv" = SPIR-V 1.0 / Vulkan 1.0,
           accepted by all drivers)
"""
import os
import subprocess
import sys

# Project root is the parent of this script's directory.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IS_MACOS = sys.platform == "darwin"
PLATFORM = "osx" if IS_MACOS else "linux"
PROFILE = "metal" if IS_MACOS else "spirv"

# Defaults for running the script standalone (no CMake).
DEFAULT_SHADERC = os.path.join(
    ROOT, "third-party/bgfx/.build",
    "osx-arm64" if IS_MACOS else "linux64_gcc", "bin", "shadercRelease")
DEFAULT_BGFX = os.path.join(ROOT, "third-party/bgfx")


def build(shaderc, bgfx, out_dir, src, out, type_flag, name):
    cmd = [
        shaderc,
        "-f", os.path.join(ROOT, "shaders", src),
        "-o", os.path.join(out_dir, out),
        "--type", type_flag, "--platform", PLATFORM, "-p", PROFILE,
        "--bin2c", name, "-i", os.path.join(bgfx, "src"),
    ]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    out_dir = os.path.abspath(sys.argv[1])
    shaderc = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_SHADERC
    bgfx = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_BGFX
    os.makedirs(out_dir, exist_ok=True)
    try:
        build(shaderc, bgfx, out_dir, "vs_quad.sc", "vs_quad.h", "v", "vs_quad")
        build(shaderc, bgfx, out_dir, "fs_quad.sc", "fs_quad.h", "f", "fs_quad")
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"build_shaders failed: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"wrote {os.path.join(out_dir, 'vs_quad.h')} and "
          f"{os.path.join(out_dir, 'fs_quad.h')}")


if __name__ == "__main__":
    main()
