# ============================================================================
#  Third-party (bgfx + bx + bimg) — build targets for the gmake project
#  GENie generates.
#  ---------------------------------------------------------------------------
#  bgfx's build system is a GENie-generated gmake project at:
#    third-party/bgfx/.build/projects/gmake-osx-arm64
#  `.build` is git-ignored in bgfx, so the project does not exist on a fresh
#  checkout and is removed entirely by a clean (bgfx's canonical clean is
#  `rm -rf .build`). Whenever it is missing, tp_projgen regenerates it with
#  the prebuilt GENie tool shipped in bx.
#
#  Artifacts land in .build/osx-arm64/bin/:
#    libbxRelease.a  libbimgRelease.a  libbgfxRelease.a  shadercRelease
#
#  Targets:
#    tp_projgen         (re)generate the gmake project if missing
#    tp_bx/tp_bimg/tp_bgfx/tp_shaderc   build one component (no-op if current)
#    third-party        everything the app needs
#    third-party-clean  remove all third-party artifacts (bgfx/.build)
# ============================================================================

set(TP_CFG    "release")      # genie config
set(TP_SUFFIX "Release")      # library filename suffix (lib<name>Release.a)
set(BGFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bgfx" CACHE INTERNAL "bgfx checkout")
set(BX_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/bx"   CACHE INTERNAL "bx checkout")
set(BIMG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bimg" CACHE INTERNAL "bimg checkout")
set(TP_BIN    "${BGFX_ROOT}/.build/osx-arm64/bin")
set(TP_PROJ   "${BGFX_ROOT}/.build/projects/gmake-osx-arm64")
set(TP_GENIE  "${BX_ROOT}/tools/bin/darwin/genie")

# (Re)generate the gmake project whenever it is missing (fresh checkout or
# after a clean). GENie runs from the bgfx root and reads scripts/genie.lua.
add_custom_command(
    OUTPUT "${TP_PROJ}/Makefile"
    COMMAND "${TP_GENIE}" --with-tools --with-combined-examples --gcc=osx-arm64 gmake
    WORKING_DIRECTORY "${BGFX_ROOT}"
    COMMENT "[tp] generating gmake-osx-arm64 project (genie)"
    VERBATIM)
add_custom_target(tp_projgen DEPENDS "${TP_PROJ}/Makefile")

# tp_lib(<name>) — build lib<name>Release.a via the gmake project.
function(tp_lib name)
    set(lib "${TP_BIN}/lib${name}${TP_SUFFIX}.a")
    add_custom_command(
        OUTPUT "${lib}"
        COMMAND make -C "${TP_PROJ}" "config=${TP_CFG}" "${name}"
        DEPENDS "${TP_PROJ}/Makefile"
        COMMENT "[tp] building ${name}"
        VERBATIM)
    add_custom_target(tp_${name} DEPENDS "${lib}")
endfunction()

tp_lib(bx)
tp_lib(bimg)
tp_lib(bgfx)

# shaderc — heavy (glslang / SPIRV-Tools / tint); only needed by `shaders`.
add_custom_command(
    OUTPUT "${TP_BIN}/shaderc${TP_SUFFIX}"
    COMMAND make -C "${TP_PROJ}" "config=${TP_CFG}" shaderc
    DEPENDS "${TP_PROJ}/Makefile"
    COMMENT "[tp] building shaderc"
    VERBATIM)
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
