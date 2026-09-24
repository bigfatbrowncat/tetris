# ============================================================================
#  Third-party (bgfx + bx + bimg) — build targets for the GENie projects.
#  ---------------------------------------------------------------------------
#  bgfx's build system is a GENie-generated project:
#    macOS:  third-party/bgfx/.build/projects/gmake-osx-arm64 (gmake)
#    Linux:  third-party/bgfx/.build/projects/gmake-linux-gcc (gmake)
#    Windows: third-party/bgfx/.build/projects/vs2022 (Visual Studio, v143)
#  `.build` is git-ignored in bgfx, so the project does not exist on a fresh
#  checkout and is removed entirely by a clean (bgfx's canonical clean is
#  `rm -rf .build`). Whenever it is missing, tp_projgen regenerates it with
#  the prebuilt GENie tool shipped in bx.
#
#  Artifacts land in the platform bin dir, e.g.:
#    .build/osx-arm64/bin/:        libbxRelease.a  ...  shadercRelease
#    .build/linux64_gcc/bin/:      libbxRelease.a  ...  shadercRelease
#    .build/win64_vs2022/bin/:     bxRelease.lib   ...  shadercRelease.exe
#
#  Windows toolchain: the pinned bx requires MSVC >= 19.35 (VS 2022 17.5+),
#  so the Visual Studio action is chosen from the compiler version
#  (19.3x/19.4x -> vs2022, 19.2x -> vs2019 — the latter fails bx's
#  static_assert; install VS 2022 Build Tools).
#
#  Targets:
#    tp_projgen         (re)generate the project if missing
#    tp_bx/tp_bimg/tp_bgfx/tp_shaderc   component artifacts (one build run)
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
    set(TP_PROJ  "${BGFX_ROOT}/.build/projects/gmake-${TP_ARCH}")
    set(TP_LIB_PREFIX "lib")
    set(TP_LIB_SUFFIX ".a")
elseif(WIN32)
    # MSVC via GENie's Visual Studio action + MSBuild. The action's toolset
    # must be the v143 (19.3x+) that bx requires, so pick it from the
    # compiler version in use.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.30)
        set(TP_VS "vs2022")
    else()
        set(TP_VS "vs2019")
    endif()
    set(TP_GENIE "${BX_ROOT}/tools/bin/windows/genie.exe")
    set(TP_BIN   "${BGFX_ROOT}/.build/win64_${TP_VS}/bin")
    set(TP_PROJ  "${BGFX_ROOT}/.build/projects/${TP_VS}")
    set(TP_LIB_PREFIX "")      # Windows libraries: <name>Release.lib
    set(TP_LIB_SUFFIX ".lib")
    # MSBuild: derive it from the compiler's directory
    # (<vs-root>/VC/Tools/MSVC/<ver>/bin/Hostx64/x64 — the cl.exe dir is
    # seven levels below the VS root), falling back to PATH.
    get_filename_component(TP_CL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(TP_MSBUILD MSBuild.exe
        PATHS "${TP_CL_DIR}/../../../../../../../MSBuild/Current/Bin"
        NO_DEFAULT_PATH)
    if(NOT TP_MSBUILD)
        find_program(TP_MSBUILD MSBuild.exe)
    endif()
    if(NOT TP_MSBUILD)
        message(FATAL_ERROR "MSBuild.exe not found (install Visual Studio 2022 with the C++ workload, or the Build Tools 2022)")
    endif()
else()
    set(TP_ARCH  "linux-gcc")
    set(TP_GENIE "${BX_ROOT}/tools/bin/linux/genie")
    set(TP_BIN   "${BGFX_ROOT}/.build/linux64_gcc/bin")
    set(TP_CFG   "release64") # plain `release` is a legacy fallback (-> scripts/)
    set(TP_PROJ  "${BGFX_ROOT}/.build/projects/gmake-${TP_ARCH}")
    set(TP_LIB_PREFIX "lib")
    set(TP_LIB_SUFFIX ".a")
endif()

set(TP_SUFFIX "Release")  # library filename suffix (Release)

set(TP_LIB_BX      "${TP_BIN}/${TP_LIB_PREFIX}bx${TP_SUFFIX}${TP_LIB_SUFFIX}")
set(TP_LIB_BIMG    "${TP_BIN}/${TP_LIB_PREFIX}bimg${TP_SUFFIX}${TP_LIB_SUFFIX}")
set(TP_LIB_BGFX    "${TP_BIN}/${TP_LIB_PREFIX}bgfx${TP_SUFFIX}${TP_LIB_SUFFIX}")
set(TP_EXE_SHADERC "${TP_BIN}/shaderc${TP_SUFFIX}${CMAKE_EXECUTABLE_SUFFIX}")
set(TP_ARTIFACTS "${TP_LIB_BX}" "${TP_LIB_BIMG}" "${TP_LIB_BGFX}" "${TP_EXE_SHADERC}")

if(WIN32)
    set(TP_PROJGEN_OUTPUT "${TP_PROJ}/bgfx.sln")
    set(TP_PROJGEN_CMD "${TP_GENIE}" --with-tools ${TP_VS})
    set(TP_PROJGEN_COMMENT "[tp] generating ${TP_VS} project (genie)")
else()
    set(TP_PROJGEN_OUTPUT "${TP_PROJ}/Makefile")
    set(TP_PROJGEN_CMD "${TP_GENIE}" --with-tools --with-combined-examples --gcc=${TP_ARCH} gmake)
    set(TP_PROJGEN_COMMENT "[tp] generating gmake-${TP_ARCH} project (genie)")
endif()

# (Re)generate the project whenever it is missing (fresh checkout or after a
# clean). GENie runs from the bgfx root and reads scripts/genie.lua.
add_custom_command(
    OUTPUT "${TP_PROJGEN_OUTPUT}"
    COMMAND ${TP_PROJGEN_CMD}
    WORKING_DIRECTORY "${BGFX_ROOT}"
    COMMENT ${TP_PROJGEN_COMMENT}
    VERBATIM)
add_custom_target(tp_projgen DEPENDS "${TP_PROJGEN_OUTPUT}")

if(WIN32)
    # Build all components via MSBuild (tp_build.ps1), serialized by a lock:
    # CMake's VS/NMake generators duplicate a file-output custom command into
    # every target that consumes one of its outputs, so a parallel build can
    # launch several copies of the script at once. The script itself is the
    # guard: the lock serializes the concurrent copies (the first does the
    # work, the rest are no-ops) and also makes its GENie regeneration atomic
    # (no two GENie runs may interleave their .build/projects writes).
    # (One project per MSBuild invocation — it accepts only one project file;
    # the rest come in through the generated project references.)
    add_custom_command(
        OUTPUT ${TP_ARTIFACTS}
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tp_build.ps1"
                "${CMAKE_CURRENT_BINARY_DIR}/tp-msbuild.lock"
                "${TP_MSBUILD}"
                "${TP_GENIE}"
                "${BGFX_ROOT}"
                "${TP_PROJ}/bgfx.sln"
                "${TP_VS}"
                "${TP_PROJ}/bx.vcxproj"
                "${TP_PROJ}/bimg.vcxproj"
                "${TP_PROJ}/bgfx.vcxproj"
                "${TP_PROJ}/shaderc.vcxproj"
        COMMENT "[tp] building bx bimg bgfx shaderc (msbuild)"
        VERBATIM)
else()
    # Build all components in ONE make invocation, serialized by a lock
    # (tp_build.sh): the gmake project cannot be built by concurrent make
    # processes — every component's archive recipe does `rm -f` + `ar` on
    # shared output files — and CMake's Makefile generator duplicates this
    # rule into each dependent target's sub-make, so parallel builds would
    # race.
    add_custom_command(
        OUTPUT ${TP_ARTIFACTS}
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tp_build.sh"
                "${CMAKE_CURRENT_BINARY_DIR}/tp-make.lock"
                "${TP_PROJ}" "${TP_CFG}" bx bimg bgfx shaderc
        DEPENDS "${TP_PROJGEN_OUTPUT}"
        COMMENT "[tp] building bx bimg bgfx shaderc"
        VERBATIM)
endif()

# Per-component targets (no-op if their artifact is current).
add_custom_target(tp_bx      DEPENDS ${TP_LIB_BX})
add_custom_target(tp_bimg    DEPENDS ${TP_LIB_BIMG})
add_custom_target(tp_bgfx    DEPENDS ${TP_LIB_BGFX})
add_custom_target(tp_shaderc DEPENDS ${TP_EXE_SHADERC})

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
