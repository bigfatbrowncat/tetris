# ============================================================================
#  Third-party (bgfx + bx + bimg) — wraps bgfx's GENie-generated gmake
#  project in CMake custom targets.
#  ---------------------------------------------------------------------------
#  The gmake project lives at:
#    third-party/bgfx/.build/projects/gmake-osx-arm64
#  and (config=release) produces into .build/osx-arm64/bin/:
#    libbxRelease.a  libbimgRelease.a  libbgfxRelease.a  shadercRelease
#
#  Each target tp_<name> invokes the gmake project for that component.
#  Running it when everything is up to date is a fast no-op, so depending
#  on these targets unconditionally is safe.
# ============================================================================

set(TP_CFG    "release")      # genie config
set(TP_SUFFIX "Release")      # library filename suffix (lib<name>Release.a)
set(BGFX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bgfx" CACHE INTERNAL "bgfx checkout")
set(BX_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/bx"   CACHE INTERNAL "bx checkout")
set(BIMG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bimg" CACHE INTERNAL "bimg checkout")
set(TP_BIN    "${BGFX_ROOT}/.build/osx-arm64/bin")
set(TP_PROJ   "${BGFX_ROOT}/.build/projects/gmake-osx-arm64")

# tp_lib(<name>) — build lib<name>Release.a via the gmake project.
function(tp_lib name)
    set(lib "${TP_BIN}/lib${name}${TP_SUFFIX}.a")
    add_custom_command(
        OUTPUT "${lib}"
        COMMAND make -C "${TP_PROJ}" "config=${TP_CFG}" "${name}"
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
    COMMENT "[tp] building shaderc"
    VERBATIM)
add_custom_target(tp_shaderc DEPENDS "${TP_BIN}/shaderc${TP_SUFFIX}")

# Everything the app needs from third-party.
add_custom_target(third-party DEPENDS tp_bx tp_bimg tp_bgfx tp_shaderc)

# Export to the parent scope.
set(TP_BIN    "${TP_BIN}"    PARENT_SCOPE)
set(BGFX_ROOT "${BGFX_ROOT}" PARENT_SCOPE)
set(BX_ROOT   "${BX_ROOT}"   PARENT_SCOPE)
