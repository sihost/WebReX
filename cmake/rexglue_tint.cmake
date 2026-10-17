# libtint_web.a (Tint SPIR-V reader + WGSL writer + rex::graphics::webgpu::SpirvToWgsl) for the
# WebGPU backend. Included by src/graphics/CMakeLists.txt when REXGLUE_USE_WEBGPU is on.
#
# Default: thirdparty/tint is built as an ExternalProject of the SDK build, with the same
# toolchain file and object flags (-m64 -pthread -msimd128 -fwasm-exceptions
# -sSUPPORT_LONGJMP=wasm), into <build>/tint-web/libtint_web.a (first build ~10 min; later builds
# are a no-op sub-ninja). It needs the sparse Dawn checkout in thirdparty/dawn (Dawn 31e25af2,
# the emdawnwebgpu v20260423.175430 revision; scripts/fetch_dawn.bat / fetch_dawn.sh).
#
# Alternatives:
#   -DREXGLUE_TINT_WEB_LIBRARY=<path>/libtint_web.a  link a prebuilt archive (for example the
#                                                    archive from an earlier SDK build)
#   -DREXGLUE_BUILD_TINT_WEB=OFF                     no Tint: shaders are not converted and the
#                                                    WebGPU backend drops every draw
#
# Result: REXGLUE_TINT_WEB_LIB (internal cache; empty without Tint) and, when built here, the
# target rex_tint_web_build. The archive is installed as lib/librex_tint_web.a.
include_guard(GLOBAL)

set(REXGLUE_TINT_WEB_LIBRARY "" CACHE FILEPATH
    "Prebuilt libtint_web.a (SpirvToWgsl) for the WebGPU backend; empty = build thirdparty/tint")
option(REXGLUE_BUILD_TINT_WEB
       "Build thirdparty/tint (libtint_web.a) when REXGLUE_TINT_WEB_LIBRARY is not set" ON)
set(REXGLUE_TINT_WEB_BUILD_TYPE "Release" CACHE STRING
    "CMAKE_BUILD_TYPE of the thirdparty/tint ExternalProject (Release also for Debug SDK builds)")

set(_rex_tint_lib "")
if(REXGLUE_TINT_WEB_LIBRARY AND EXISTS "${REXGLUE_TINT_WEB_LIBRARY}")
    set(_rex_tint_lib "${REXGLUE_TINT_WEB_LIBRARY}")
    message(STATUS "WebGPU: SpirvToWgsl from the prebuilt ${REXGLUE_TINT_WEB_LIBRARY}")
elseif(REXGLUE_BUILD_TINT_WEB AND EXISTS "${REXGLUE_ROOT}/thirdparty/dawn/src/tint/CMakeLists.txt")
    if(REXGLUE_TINT_WEB_LIBRARY)
        message(STATUS "WebGPU: REXGLUE_TINT_WEB_LIBRARY=${REXGLUE_TINT_WEB_LIBRARY} does not exist, "
            "building thirdparty/tint instead")
    endif()
    include(ExternalProject)
    set(_rex_tint_dir "${CMAKE_BINARY_DIR}/tint-web")
    set(_rex_tint_lib "${_rex_tint_dir}/libtint_web.a")
    ExternalProject_Add(rex_tint_web_build
        SOURCE_DIR "${REXGLUE_ROOT}/thirdparty/tint"
        BINARY_DIR "${_rex_tint_dir}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_CACHE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
            "-DCMAKE_BUILD_TYPE:STRING=${REXGLUE_TINT_WEB_BUILD_TYPE}"
            "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}"
            "-DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}"
            "-DREX_SDK_DIR:PATH=${REXGLUE_ROOT}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build "${_rex_tint_dir}" --target tint_web
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${_rex_tint_lib}"
        # The sub-build tracks its own sources (Tint, webgpu_shader_tint.cpp); a no-op when clean.
        BUILD_ALWAYS ON
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD ON)
    message(STATUS "WebGPU: SpirvToWgsl built from thirdparty/tint into ${_rex_tint_lib}")
endif()
set(REXGLUE_TINT_WEB_LIB "${_rex_tint_lib}" CACHE INTERNAL "libtint_web.a linked by rexgraphics")
