# rex_tint: Tint (SPIR-V reader + WGSL writer) from thirdparty/dawn, plus
# rex::graphics::webgpu::SpirvToWgsl (src/graphics/webgpu/webgpu_shader_tint.cpp).
#
# Standalone project, built as an ExternalProject by the SDK's wasm64 build
# (cmake/rexglue_tint.cmake) so Dawn's compile settings stay separate from the
# SDK's, and directly on the host for rex_spirv_to_wgsl:
#
#   host:  cmake -S thirdparty/tint -B C:/rex/b/tint-host -G Ninja -DCMAKE_BUILD_TYPE=Release
#          -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl   (MSVC environment)
#          cmake --build C:/rex/b/tint-host --target rex_spirv_to_wgsl
#   web:   emcmake cmake -S thirdparty/tint -B <dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
#          -DCMAKE_C_FLAGS=... (same -m64 -pthread -fwasm-exceptions ... as the SDK)
#          cmake --build <dir> --target tint_web      -> <dir>/libtint_web.a
#
# thirdparty/dawn is a sparse checkout of Dawn 31e25af254ab (the emdawnwebgpu
# v20260423.175430 revision): src/cmake, src/tint, src/utils, include,
# third_party + the abseil-cpp, spirv-headers/src, spirv-tools/src submodules.
# The project's tools/tint/fetch_dawn.bat recreates it.
cmake_minimum_required(VERSION 3.25)
project(rex_tint LANGUAGES C CXX)

set(REX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "ReXGlue SDK root")
set(REX_DAWN_DIR "${REX_SDK_DIR}/thirdparty/dawn" CACHE PATH "Dawn checkout")
set(REX_DAWN_REVISION "31e25af254ab572c77054edec4946d2244e184dd")

if(NOT EXISTS "${REX_DAWN_DIR}/src/tint/CMakeLists.txt")
  message(FATAL_ERROR "rex_tint: no Dawn checkout at ${REX_DAWN_DIR} (run tools/tint/fetch_dawn.bat)")
endif()

# Only Tint: no Dawn backends, samples, tests or tools.
foreach(opt DAWN_ENABLE_D3D11 DAWN_ENABLE_D3D12 DAWN_ENABLE_METAL DAWN_ENABLE_NULL
            DAWN_ENABLE_DESKTOP_GL DAWN_ENABLE_OPENGLES DAWN_ENABLE_VULKAN
            DAWN_ENABLE_WEBGPU_ON_WEBGPU DAWN_ENABLE_SPIRV_VALIDATION DAWN_BUILD_SAMPLES
            DAWN_BUILD_TESTS DAWN_BUILD_PROTOBUF DAWN_USE_GLFW DAWN_USE_WINDOWS_UI DAWN_USE_X11
            DAWN_USE_WAYLAND DAWN_ENABLE_INSTALL TINT_BUILD_CMD_TOOLS TINT_BUILD_TESTS
            TINT_BUILD_BENCHMARKS TINT_BUILD_FUZZERS TINT_BUILD_SPV_WRITER TINT_BUILD_GLSL_WRITER
            TINT_BUILD_GLSL_VALIDATOR TINT_BUILD_HLSL_WRITER TINT_BUILD_MSL_WRITER
            TINT_BUILD_NULL_WRITER TINT_BUILD_IR_BINARY TINT_BUILD_TINTD
            TINT_ENABLE_IR_DUMPING TINT_ENABLE_INSTALL)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(DAWN_BUILD_MONOLITHIC_LIBRARY OFF CACHE STRING "" FORCE)
set(TINT_BUILD_SPV_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_WRITER ON CACHE BOOL "" FORCE)
# The WGSL reader is only needed by the host CLI (re-validating the output);
# the browser validates the WGSL itself.
if(EMSCRIPTEN)
  set(TINT_BUILD_WGSL_READER OFF CACHE BOOL "" FORCE)
else()
  set(TINT_BUILD_WGSL_READER ON CACHE BOOL "" FORCE)
endif()

# Dawn's CMake treats Emscripten as "emdawnwebgpu only": it skips SPIRV-Tools
# and adds src/dawn (not in the sparse checkout). Tint itself is portable C++,
# so configure Dawn as a generic (UNIX) target and restore the flag after.
set(_rex_tint_emscripten "${EMSCRIPTEN}")
if(EMSCRIPTEN)
  # abseil's config.h reads the deprecated __EMSCRIPTEN_major__ macros, and
  # some Tint targets compile with -Werror.
  add_compile_options(-Wno-deprecated-pragma)
endif()
set(EMSCRIPTEN 0)
add_subdirectory("${REX_DAWN_DIR}" dawn EXCLUDE_FROM_ALL)
set(EMSCRIPTEN "${_rex_tint_emscripten}")

add_library(rex_tint STATIC "${REX_SDK_DIR}/src/graphics/webgpu/webgpu_shader_tint.cpp")
target_include_directories(rex_tint PUBLIC "${REX_SDK_DIR}/include")
target_link_libraries(rex_tint PUBLIC tint_lang_spirv_reader tint_lang_wgsl_writer)
string(SUBSTRING "${REX_DAWN_REVISION}" 0 12 _rev)
target_compile_definitions(rex_tint PRIVATE "REX_TINT_REVISION=\"dawn-${_rev}\"")
set_target_properties(rex_tint PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

if(NOT EMSCRIPTEN)
  add_executable(rex_spirv_to_wgsl rex_spirv_to_wgsl.cpp)
  target_link_libraries(rex_spirv_to_wgsl PRIVATE rex_tint tint_lang_wgsl_reader)
  set_target_properties(rex_spirv_to_wgsl PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

# One archive with rex_tint and every static library it needs, for linking
# into the SDK / game without exporting ~60 Tint targets.
function(_rex_tint_collect target out_var)
  set(seen ${${out_var}})
  if(NOT TARGET ${target} OR ${target} IN_LIST seen)
    return()
  endif()
  get_target_property(aliased ${target} ALIASED_TARGET)
  if(aliased)
    _rex_tint_collect(${aliased} seen)
    set(${out_var} ${seen} PARENT_SCOPE)
    return()
  endif()
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "STATIC_LIBRARY")
    list(APPEND seen ${target})
  endif()
  set(deps)
  get_target_property(l1 ${target} LINK_LIBRARIES)
  get_target_property(l2 ${target} INTERFACE_LINK_LIBRARIES)
  foreach(d IN LISTS l1 l2)
    if(d MATCHES "^\\$<LINK_ONLY:(.+)>$")
      set(d "${CMAKE_MATCH_1}")
    endif()
    if(d AND NOT d MATCHES "^\\$<" AND TARGET ${d})
      list(APPEND deps ${d})
    endif()
  endforeach()
  foreach(d IN LISTS deps)
    _rex_tint_collect(${d} seen)
  endforeach()
  set(${out_var} ${seen} PARENT_SCOPE)
endfunction()
set(_rex_tint_libs)
_rex_tint_collect(rex_tint _rex_tint_libs)
list(LENGTH _rex_tint_libs _n)
message(STATUS "rex_tint: ${_n} static libraries go into the combined archive")

if(EMSCRIPTEN)
  set(_combined "${CMAKE_BINARY_DIR}/libtint_web.a")
else()
  set(_combined "${CMAKE_BINARY_DIR}/rex_tint_all${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()
set(_mri "create ${_combined}\n")
set(_deps)
foreach(l IN LISTS _rex_tint_libs)
  string(APPEND _mri "addlib $<TARGET_FILE:${l}>\n")
  list(APPEND _deps ${l})
endforeach()
string(APPEND _mri "save\nend\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tint_web.mri" CONTENT "${_mri}")
# llvm-ar MRI script (emar forwards to llvm-ar; the host uses the LLVM one).
if(EMSCRIPTEN)
  set(_ar "${CMAKE_AR}")
else()
  find_program(_ar NAMES llvm-ar HINTS "C:/Program Files/LLVM/bin" REQUIRED)
endif()
add_custom_command(OUTPUT "${_combined}"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${_combined}"
  COMMAND "${CMAKE_COMMAND}" "-DAR=${_ar}" "-DMRI=${CMAKE_BINARY_DIR}/tint_web.mri"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/run_mri.cmake"
  DEPENDS ${_deps} "${CMAKE_BINARY_DIR}/tint_web.mri"
  COMMENT "Combining ${_n} Tint libraries into ${_combined}"
  VERBATIM)
add_custom_target(tint_web ALL DEPENDS "${_combined}")
