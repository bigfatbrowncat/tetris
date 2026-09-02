# ============================================================================
#  Third-party (bgfx + bx + bimg) — build targets for the gmake project
#  GENie generates.
#  ---------------------------------------------------------------------------
#  bgfx's build system is a GENie-generated gmake project at:
#    macOS:  third-party/bgfx/.build/projects/gmake-osx-arm64
#    Linux:  third-party/bgfx/.build/projects/gmake-linux-gcc
#  `.build` is git-ignored in bgfx, so the project does not exist on a fresh
#  checkout and is removed entirely by a clean (bgfx's canonical clean is
#  `rm -rf .build`). Whenever it is missing, tp_projgen regenerates it with
#  the prebuilt GENie tool shipped in bx.
#
#  Artifacts land in the platform bin dir, e.g. .build/linux64_gcc/bin/:
#    libbxRelease.a  libbimgRelease.a  libbgfxRelease.a  shadercRelease
#
#  Targets:
#    tp_projgen         (re)generate the gmake project if missing
#    tp_bx/tp_bimg/tp_bgfx/tp_shaderc   component artifacts (single make run)
#    third-party        everything the app needs
#    third-party-clean  remove all third-party artifacts (bgfx/.build)
# ============================================================================

set(BGFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bgfx" CACHE INTERNAL "bgfx checkout")
set(BX_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/bx"   CACHE INTERNAL "bx checkout")
set(BIMG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bimg" CACHE INTERNAL "bimg checkout")

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(TP_ARCH "osx-arm64")
    else()
        set(TP_ARCH "osx-x64")
    endif()
    set(TP_GENIE "${BX_ROOT}/tools/bin/darwin/genie")
    set(TP_BIN   "${BGFX_ROOT}/.build/${TP_ARCH}/bin")
    set(TP_CFG   "release")   # osx config lands in .build/osx-<arch>/bin
else()
    set(TP_ARCH  "linux-gcc")
    set(TP_GENIE "${BX_ROOT}/tools/bin/linux/genie")
    set(TP_BIN   "${BGFX_ROOT}/.build/linux64_gcc/bin")
    set(TP_CFG   "release64") # plain `release` is a legacy fallback (-> scripts/)
endif()

set(TP_SUFFIX "Release")      # library filename suffix (lib<name>Release.a)
set(TP_PROJ   "${BGFX_ROOT}/.build/projects/gmake-${TP_ARCH}")

# (Re)generate the gmake project whenever it is missing (fresh checkout or
# after a clean). GENie runs from the bgfx root and reads scripts/genie.lua.
add_custom_command(
    OUTPUT "${TP_PROJ}/Makefile"
    COMMAND "${TP_GENIE}" --with-tools --with-combined-examples --gcc=${TP_ARCH} gmake
    WORKING_DIRECTORY "${BGFX_ROOT}"
    COMMENT "[tp] generating gmake-${TP_ARCH} project (genie)"
    VERBATIM)
add_custom_target(tp_projgen DEPENDS "${TP_PROJ}/Makefile")

# Build all components in ONE make invocation, serialized by a lock
# (tp_build.sh): the gmake project cannot be built by concurrent make
# processes — every component's archive recipe does `rm -f` + `ar` on shared
# output files — and CMake's Makefile generator duplicates this rule into
# each dependent target's sub-make, so parallel builds would race.
add_custom_command(
    OUTPUT "${TP_BIN}/libbx${TP_SUFFIX}.a"
           "${TP_BIN}/libbimg${TP_SUFFIX}.a"
           "${TP_BIN}/libbgfx${TP_SUFFIX}.a"
           "${TP_BIN}/shaderc${TP_SUFFIX}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tp_build.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/tp-make.lock"
            "${TP_PROJ}" "${TP_CFG}" bx bimg bgfx shaderc
    DEPENDS "${TP_PROJ}/Makefile"
    COMMENT "[tp] building bx bimg bgfx shaderc"
    VERBATIM)

# Per-component targets (no-op if their artifact is current).
add_custom_target(tp_bx      DEPENDS "${TP_BIN}/libbx${TP_SUFFIX}.a")
add_custom_target(tp_bimg    DEPENDS "${TP_BIN}/libbimg${TP_SUFFIX}.a")
add_custom_target(tp_bgfx    DEPENDS "${TP_BIN}/libbgfx${TP_SUFFIX}.a")
add_custom_target(tp_shaderc DEPENDS "${TP_BIN}/shaderc${TP_SUFFIX}")

# Everything the app needs from third-party.
add_custom_target(third-party DEPENDS tp_bx tp_bimg tp_bgfx tp_shaderc)

# Remove all third-party artifacts — same as bgfx's own `make clean`:
#   cmake --build build --target third-party-clean
add_custom_target(third-party-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${BGFX_ROOT}/.build"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BGFX_ROOT}/.build"
    COMMENT "[tp] cleaning third-party (removes bgfx/.build)"
    VERBATIM)

# Export to the parent scope.
set(TP_BIN    "${TP_BIN}"    PARENT_SCOPE)
set(BGFX_ROOT "${BGFX_ROOT}" PARENT_SCOPE)
set(BX_ROOT   "${BX_ROOT}"   PARENT_SCOPE)
