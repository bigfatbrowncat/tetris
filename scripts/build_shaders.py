#!/usr/bin/env python3
"""Compile the .sc shaders and emit C-embedded headers.

Usage: build_shaders.py <out-dir> [shaderc-bin] [bgfx-root]

Writes ONLY to <out-dir> (a directory inside the CMake build tree):
  <out-dir>/vs_quad.h   <out-dir>/fs_quad.h

macOS   -> Metal shaders
Linux   -> GLSL 4.30 text (profile "430") — bgfx's OpenGL/EGL backend
            compiles the embedded GLSL; it needs a desktop GLSL source, not
            SPIR-V.
Windows -> HLSL 5.0 (profile "s_5_0") — bgfx's Direct3D11 backend compiles
            the embedded DXBC; it needs a shaderc --platform windows build.
"""
import os
import subprocess
import sys

# Project root is the parent of this script's directory.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IS_MACOS = sys.platform == "darwin"
IS_WINDOWS = sys.platform == "win32"
if IS_MACOS:
    PLATFORM, PROFILE = "osx", "metal"
elif IS_WINDOWS:
    PLATFORM, PROFILE = "windows", "s_5_0"
else:
    PLATFORM, PROFILE = "linux", "430"

# Defaults for running the script standalone (no CMake).
DEFAULT_SHADERC = os.path.join(
    ROOT, "third-party/bgfx/.build",
    ("osx-arm64" if IS_MACOS
     else "win64_vs2022" if IS_WINDOWS
     else "linux64_gcc"),
    "bin", "shadercRelease" + (".exe" if IS_WINDOWS else ""))
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
