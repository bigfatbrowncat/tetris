# ============================================================================
#  Tetris — cross-platform (bgfx; macOS Metal+Cocoa / Linux Vulkan+GTK4)
#  ---------------------------------------------------------------------------
#  Modules:
#    backend/   game logic, no UI / no OS          -> libtetrisback.a
#    frontend/  bgfx renderer + game loop          -> libtetrisfront.a
#    ui/        platform-neutral window/input C API (window.h)
#    macos/     Cocoa window + input (OS-specific) -> macOS UI module
#    gtk/       GTK4 window + input (OS-specific)  -> Linux UI module
#    third-party/  bgfx / bx / bimg build targets  (see its CMakeLists.txt)
#
#  Build:
#    cmake -S . -B build
#    cmake --build build            # ./build/tetris
#    cmake --build build --target shaders   # regenerate embedded shader headers
#
#  All generated files (the executable, the embedded shader headers) are
#  written only inside the build directory; the build never touches the
#  source tree.
#
#  Clean:
#    cmake --build build --target clean        # app artifacts only
#    cmake --build build --target third-party-clean  # third-party artifacts
#    cmake --build build --target distclean    # everything (app + third-party)
#
#  Threading contract (bgfx): the game thread owns bgfx (init/frame/shutdown);
#  the UI thread (Cocoa or GTK) only pumps events and must never call
#  bgfx::renderFrame().
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# The UI language is platform-specific: Objective-C++ on macOS, plain C++ on Linux.
if(APPLE)
    project(tetris C CXX OBJCXX)
else()
    project(tetris C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)  # bx headers require C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The executable lands in the build dir (CMake default).

# Third-party bgfx build (targets: tp_bx, tp_bimg, tp_bgfx, tp_shaderc, third-party)
add_subdirectory(third-party)

# ----------------------------------------------------------------------------
#  UI module selection (platform detected at configure time)
# ----------------------------------------------------------------------------
# The embedded shader headers are generated into the build dir
# (${GEN_SHADERS}/shaders/); that dir is first on the include path so the
# shared `#include "shaders/vs_quad.h"` in the frontend always resolves to
# the generated files.
set(GEN_SHADERS "${CMAKE_BINARY_DIR}/gen-shaders")

if(APPLE)
    set(UI_SOURCES macos/window.mm)
    set(UI_LINK
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework CoreMedia"
        "-framework VideoToolbox")
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    set(UI_SOURCES gtk/window.cpp)
    set(UI_LINK PkgConfig::GTK4 PkgConfig::WAYLAND_CLIENT)
else()
    message(FATAL_ERROR "Unsupported platform (need macOS or Linux)")
endif()
set(PLATFORM_SHADERS_INC ${GEN_SHADERS})

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
    ${PLATFORM_SHADERS_INC}      # generated shader headers (build dir)
    ${CMAKE_SOURCE_DIR}          # ui/window.h etc.
    ${CMAKE_SOURCE_DIR}/frontend # font.h
    ${BGFX_ROOT}/include
    ${BX_ROOT}/include)
if(APPLE)
    target_link_libraries(tetrisfront PUBLIC
        tetrisback
        bgfxRelease bimgRelease bxRelease)   # resolved via the link dir below
else()
    target_link_libraries(tetrisfront PUBLIC
        tetrisback
        bgfxRelease bimgRelease bxRelease    # resolved via the link dir below
        vulkan)                              # VK_KHR_wayland_surface mailbox detection
endif()
target_link_directories(tetrisfront PUBLIC ${TP_BIN})
add_dependencies(tetrisfront tp_bx tp_bimg tp_bgfx)

# ----------------------------------------------------------------------------
#  shaders — generate the C header arrays from the .sc sources, into the
#  build dir only (${GEN_SHADERS}/shaders/). tetrisfront depends on
#  gen_shaders, so a fresh build generates the headers before compiling.
# ----------------------------------------------------------------------------
set(GEN_SHADER_HDRS
    ${GEN_SHADERS}/shaders/vs_quad.h
    ${GEN_SHADERS}/shaders/fs_quad.h)

add_custom_command(
    OUTPUT ${GEN_SHADER_HDRS}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/build_shaders.py
            ${GEN_SHADERS}/shaders ${TP_BIN}/shadercRelease ${BGFX_ROOT}
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/vs_quad.sc
            ${CMAKE_SOURCE_DIR}/shaders/fs_quad.sc
            tp_shaderc
    COMMENT "[shaders] building vs_quad/fs_quad"
    VERBATIM)

add_custom_target(gen_shaders DEPENDS ${GEN_SHADER_HDRS})
add_dependencies(tetrisfront gen_shaders)

# Explicit target (like the old phony `make shaders`); no-op if current.
add_custom_target(shaders DEPENDS gen_shaders)

# ----------------------------------------------------------------------------
#  tetris — executable (main.cpp + the platform UI module)
# ----------------------------------------------------------------------------
add_executable(tetris
    main.cpp
    ${UI_SOURCES})
target_link_libraries(tetris PRIVATE
    tetrisfront
    ${UI_LINK})

# ----------------------------------------------------------------------------
#  vulkan_resize_test — standalone bgfx + GTK4 + Vulkan + Wayland resize demo.
#  Exercises the frame-clock-driven resize approach from
#  docs/tips/gtk4-bgfx-vulkan-resize.md (BGFX_RESET_VSYNC == VK_PRESENT_MODE_FIFO)
#  against the real compositor, to validate the FIFO path on this driver.
# ----------------------------------------------------------------------------
#if(NOT APPLE)
#    add_executable(vulkan_resize_test vulkan_resize_test.cpp)
#    target_include_directories(vulkan_resize_test PRIVATE
#        ${BGFX_ROOT}/include
#        ${BX_ROOT}/include)
#    target_link_libraries(vulkan_resize_test PRIVATE
#        bgfxRelease bimgRelease bxRelease
#        PkgConfig::GTK4
#        PkgConfig::WAYLAND_CLIENT
#        vulkan)
#    target_link_directories(vulkan_resize_test PRIVATE ${TP_BIN})
#    add_dependencies(vulkan_resize_test tp_bx tp_bimg tp_bgfx)
#endif()

# ----------------------------------------------------------------------------
#  distclean — remove ALL build artifacts: the app (equivalent to the
#  built-in `clean`, which cannot be customized) plus third-party
#  (bx, bimg, bgfx, shaderc + deps). The build/ dir and its cache are kept.
# ----------------------------------------------------------------------------
add_custom_target(distclean
    COMMAND ${CMAKE_MAKE_PROGRAM} -C "${CMAKE_BINARY_DIR}" clean
    COMMAND ${CMAKE_MAKE_PROGRAM} -C "${CMAKE_BINARY_DIR}" third-party-clean
    COMMENT "[clean] removing all build artifacts (app + third-party)"
    VERBATIM)
