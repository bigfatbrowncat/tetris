# ============================================================================
#  Tetris — macOS (bgfx/Metal + Cocoa)
#  ---------------------------------------------------------------------------
#  Modules:
#    backend/   game logic, no UI / no OS          -> libtetrisback.a
#    frontend/  bgfx renderer + game loop          -> libtetrisfront.a
#    macos/     Cocoa window + input (OS-specific) -> tetris executable
#    third-party/  bgfx / bx / bimg build targets  (see its CMakeLists.txt)
#
#  Build:
#    cmake -S . -B build
#    cmake --build build            # ./tetris
#    cmake --build build --target shaders   # regenerate embedded shader headers
#
#  Threading contract (bgfx): the game thread owns bgfx (init/frame/shutdown);
#  the Cocoa thread only pumps events and must never call bgfx::renderFrame().
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(tetris C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Keep the executable next to the sources, like the old Makefile did.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# Third-party bgfx build (targets: tp_bx, tp_bimg, tp_bgfx, tp_shaderc, third-party)
add_subdirectory(third-party)

# ----------------------------------------------------------------------------
#  tetrisback — game logic (no UI, no OS)
# ----------------------------------------------------------------------------
add_library(tetrisback STATIC
    backend/tetris_backend.cpp)
target_include_directories(tetrisback PUBLIC
    ${CMAKE_SOURCE_DIR}/backend)

# ----------------------------------------------------------------------------
#  tetrisfront — bgfx renderer + game loop (links the backend)
# ----------------------------------------------------------------------------
add_library(tetrisfront STATIC
    frontend/renderer.cpp
    frontend/tetris_frontend.cpp)
target_include_directories(tetrisfront PUBLIC
    ${CMAKE_SOURCE_DIR}            # shaders/vs_quad.h etc.
    ${CMAKE_SOURCE_DIR}/frontend   # font.h
    ${BGFX_ROOT}/include
    ${BX_ROOT}/include)
target_link_libraries(tetrisfront PUBLIC
    tetrisback
    bgfxRelease bimgRelease bxRelease)   # resolved via the link dir below
target_link_directories(tetrisfront PUBLIC ${TP_BIN})
add_dependencies(tetrisfront tp_bx tp_bimg tp_bgfx)

# ----------------------------------------------------------------------------
#  shaders — regenerate the committed C header arrays from the .sc sources.
#  Explicit target (like the old phony `make shaders`); the app only needs the
#  headers to exist, which they do (they are committed).
# ----------------------------------------------------------------------------
add_custom_target(shaders
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/build_shaders.py
    ENVIRONMENT "BIN=${TP_BIN}" "BGFX=${BGFX_ROOT}"
    COMMENT "[shaders] building vs_quad/fs_quad"
    VERBATIM)
add_dependencies(shaders tp_shaderc)

# ----------------------------------------------------------------------------
#  tetris — macOS executable (Cocoa UI module)
# ----------------------------------------------------------------------------
add_executable(tetris
    macos/main.cpp
    macos/window.mm)
target_link_libraries(tetris PRIVATE
    tetrisfront
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework Metal"
    "-framework IOKit"
    "-framework CoreVideo"
    "-framework CoreMedia"
    "-framework VideoToolbox")
