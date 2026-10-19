#==========================================================
# rexglue_configure_target() - Configure a consumer target
# with platform-specific settings and SDK source files.
#
# Usage:
#   rexglue_configure_target(<target>
#       [WEB_INITIAL_MEMORY <size>] [WEB_MAXIMUM_MEMORY <size>])
#
# Adds:
#   - Platform entry point source (windowed_app_main_*.cpp)
#   - ReXApp base class source (rex_app.cpp)
#   - Platform-specific link/compile settings
#
# Web (Emscripten) wasm memory, per title. Sizes use Emscripten
# units (e.g. 3GB, 3584MB). Defaults: REXGLUE_WEB_INITIAL_MEMORY /
# REXGLUE_WEB_MAXIMUM_MEMORY (cache variables: -D on the configure
# line, or set() in the title's CMakeLists.txt before
# rexglue_setup_target(), which calls this without arguments).
#   WEB_INITIAL_MEMORY  memory the tab commits at startup. The
#                       browser commits all of it up front
#                       (Windows commit charge), touched or not.
#   WEB_MAXIMUM_MEMORY  growth limit. Larger than the initial
#                       size: the memory grows on demand
#                       (-sALLOW_MEMORY_GROWTH, length-tracking JS
#                       views where the browser has them); only the
#                       grown part is committed. Equal: fixed size.
# Running out (limit reached, or the browser refuses to grow)
# aborts with an "out of memory" crash report (rex_web_pre.js).
# These link options come after CMAKE_EXE_LINKER_FLAGS, so they
# override -sINITIAL_MEMORY / -sALLOW_MEMORY_GROWTH set there.
#==========================================================
# Layout B needs a 2.75 GiB guest window plus ~0.4-1 GiB host heap
# (MW2 match / Halo 3 game: heap break 3.1-3.75 GiB, see
# docs/web_limitations.md), so 3 GiB initial grows a little during boot;
# 8 GiB keeps the previous fixed size as the ceiling.
set(REXGLUE_WEB_INITIAL_MEMORY "3GB" CACHE STRING
    "Web: initial wasm memory of rexglue_configure_target targets (Emscripten size, e.g. 3GB)")
set(REXGLUE_WEB_MAXIMUM_MEMORY "8GB" CACHE STRING
    "Web: maximum wasm memory (growth limit); equal to the initial size = fixed memory")
set(REXGLUE_WEB_PTHREAD_POOL_SIZE "32" CACHE STRING
    "Web: Workers spawned before main() (-sPTHREAD_POOL_SIZE); more threads spawn Workers lazily")

# Emscripten size string (123, 64KB, 3584MB, 3GB) -> bytes.
function(_rexglue_web_size_bytes out value)
    string(TOUPPER "${value}" v)
    if(v MATCHES "^([0-9]+)(B|KB|MB|GB|TB)?$")
        set(n "${CMAKE_MATCH_1}")
        set(unit "${CMAKE_MATCH_2}")
        if(unit STREQUAL "KB")
            math(EXPR n "${n} * 1024")
        elseif(unit STREQUAL "MB")
            math(EXPR n "${n} * 1048576")
        elseif(unit STREQUAL "GB")
            math(EXPR n "${n} * 1073741824")
        elseif(unit STREQUAL "TB")
            math(EXPR n "${n} * 1099511627776")
        endif()
        set(${out} "${n}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "rexglue: bad wasm memory size '${value}' (want e.g. 3GB, 3584MB)")
    endif()
endfunction()

function(rexglue_configure_target target_name)
    cmake_parse_arguments(PARSE_ARGV 1 RCT "" "WEB_INITIAL_MEMORY;WEB_MAXIMUM_MEMORY" "")
    # Platform entry point
    if(WIN32)
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_win.cpp)
    elseif(EMSCRIPTEN)
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_web.cpp)
    else()
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_posix.cpp)
    endif()

    # ReXApp base class
    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)

    # Web (Emscripten, wasm64) platform settings. No GTK, no code model.
    if(EMSCRIPTEN)
        target_compile_options(${target_name} PRIVATE
            -pthread -msimd128 -fwasm-exceptions "SHELL:-s SUPPORT_LONGJMP=wasm")
        # Kernel exports self-register through static PPCFuncRegistrar objects
        # (XBOXKRNL_EXPORT/XAM_EXPORT); wasm-ld would drop the unreferenced
        # objects of librexkernel.a exactly like ld does, so force them in.
        target_link_options(${target_name} PRIVATE
            -Wl,--whole-archive
            $<TARGET_FILE:rex::kernel>
            -Wl,--no-whole-archive
        )
        target_link_options(${target_name} PRIVATE
            -pthread
            -fwasm-exceptions
            "SHELL:-s SUPPORT_LONGJMP=wasm"
            # main() runs on a pthread so it may block (Atomics.wait).
            "SHELL:-s PROXY_TO_PTHREAD=1"
            "SHELL:-s OFFSCREENCANVAS_SUPPORT=1"
            # libpthread's canvas transfer tracks OffscreenCanvases in the GL
            # library object (GL.offscreenCanvases); without it pthread_create
            # fails with "GL is not defined" even though we render via WebGPU.
            # -lGL alone is dead-stripped (nothing references $GL), so force it.
            -lGL
            "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$GL"
            "SHELL:-s ENVIRONMENT=web,worker"
            # main()/runtime setup runs on this stack (default is only 64 KiB).
            "SHELL:-s STACK_SIZE=8MB"
            "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=2MB"
            # ~25 threads per session (15 guest XThreads, 8 SDK host threads,
            # the PROXY_TO_PTHREAD main thread, the WasmFS OPFS worker).
            # Pre-spawned Workers avoid instantiating the module lazily mid-game.
            "SHELL:-s PTHREAD_POOL_SIZE=${REXGLUE_WEB_PTHREAD_POOL_SIZE}"
            # WasmFS: OPFS backend for persistent xstorage: and the
            # HttpRangeDevice block cache (rex::filesystem::web). Replaces the
            # JS FS API, so loaders must not use FS.mount(IDBFS).
            -sWASMFS
            # web_storage.cpp references the OPFS backend weakly (links without
            # WASMFS); weak refs never pull archive members, so force them in.
            -Wl,--undefined=wasmfs_create_opfs_backend
            -Wl,--undefined=wasmfs_create_directory
            # Stack overflows otherwise corrupt the heap silently.
            $<$<CONFIG:Debug>:-sSTACK_OVERFLOW_CHECK=2>
            # Keep function names (name section only, no DWARF) in optimized
            # builds so browser stack traces of traps are symbolized.
            $<$<NOT:$<CONFIG:Debug>>:--profiling-funcs>
            --use-port=emdawnwebgpu
            # Crash reports to the page (Module.rexOnFatal) and the page lifecycle
            # (rex/crash_report.h, rex/web_page.h); runs in the page and every Worker.
            "SHELL:--pre-js ${REXGLUE_SHARE_DIR}/rex_web_pre.js"
        )
        set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS
            ${REXGLUE_SHARE_DIR}/rex_web_pre.js)

        # Wasm memory (see the header comment).
        set(web_initial "${REXGLUE_WEB_INITIAL_MEMORY}")
        set(web_maximum "${REXGLUE_WEB_MAXIMUM_MEMORY}")
        if(RCT_WEB_INITIAL_MEMORY)
            set(web_initial "${RCT_WEB_INITIAL_MEMORY}")
        endif()
        if(RCT_WEB_MAXIMUM_MEMORY)
            set(web_maximum "${RCT_WEB_MAXIMUM_MEMORY}")
        endif()
        _rexglue_web_size_bytes(web_initial_bytes "${web_initial}")
        _rexglue_web_size_bytes(web_maximum_bytes "${web_maximum}")
        if(web_maximum_bytes LESS web_initial_bytes)
            message(FATAL_ERROR "rexglue: ${target_name}: WEB_MAXIMUM_MEMORY ${web_maximum} is below "
                "WEB_INITIAL_MEMORY ${web_initial}")
        endif()
        if(web_maximum_bytes GREATER web_initial_bytes)
            target_link_options(${target_name} PRIVATE
                "SHELL:-s INITIAL_MEMORY=${web_initial_bytes}"
                "SHELL:-s ALLOW_MEMORY_GROWTH=1"
                "SHELL:-s MAXIMUM_MEMORY=${web_maximum_bytes}"
                # Abort with a report (not a NULL malloc) when the memory can not grow.
                "SHELL:-s ABORTING_MALLOC=1"
                # Plain SharedArrayBuffer views, refreshed in JS after a growth (one compare per
                # heap access). NOT -sGROWABLE_ARRAYBUFFERS: views of a growable SharedArrayBuffer
                # are rejected by FileSystemSyncAccessHandle.read/write (WasmFS OPFS: every write
                # failed with EINVAL/ENOSPC, "disk full", in Chrome 155), while WebGPU accepts them.
                "SHELL:-s GROWABLE_ARRAYBUFFERS=0"
                -Wno-pthreads-mem-growth)
            message(STATUS "rexglue: ${target_name}: web memory ${web_initial} initial, grows to ${web_maximum}")
        else()
            target_link_options(${target_name} PRIVATE
                "SHELL:-s INITIAL_MEMORY=${web_initial_bytes}"
                "SHELL:-s ALLOW_MEMORY_GROWTH=0"
                "SHELL:-s ABORTING_MALLOC=1")
            message(STATUS "rexglue: ${target_name}: web memory fixed at ${web_initial}")
        endif()
    endif()

    # Linux platform settings
    if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        target_include_directories(${target_name} PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(${target_name} PRIVATE ${GTK3_LIBRARIES})

        # Whole-archive linking for kernel hooks
        target_link_options(${target_name} PRIVATE
            -Wl,--whole-archive
            $<TARGET_FILE:rex::kernel>
            -Wl,--no-whole-archive
        )
        # Large executable support
        target_link_options(${target_name} PRIVATE -Wl,--no-relax)
        target_compile_options(${target_name} PRIVATE -mcmodel=large)
    endif()

    if(NOT MSVC AND NOT EMSCRIPTEN)
        target_compile_options(${target_name} PRIVATE -msse4.1)
    endif()

    # Copy runtime DLLs next to the executable
    if(WIN32)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
        )
        # FidelityFX is linked PRIVATE by rexui (to avoid propagating DLL
        # requirements to tool-mode targets), so copy its DLLs explicitly.
        if(TARGET amd_fidelityfx_vk)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:amd_fidelityfx_vk>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
        if(TARGET amd_fidelityfx_dx12)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:amd_fidelityfx_dx12>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
    endif()
endfunction()
