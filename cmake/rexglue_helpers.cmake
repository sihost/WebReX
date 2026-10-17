#==========================================================
# rexglue_configure_target() - Configure a consumer target
# with platform-specific settings and SDK source files.
#
# Usage:
#   rexglue_configure_target(<target>)
#
# Adds:
#   - Platform entry point source (windowed_app_main_*.cpp)
#   - ReXApp base class source (rex_app.cpp)
#   - Platform-specific link/compile settings
#==========================================================
function(rexglue_configure_target target_name)
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
            "SHELL:-s ENVIRONMENT=web,worker"
            # main()/runtime setup runs on this stack (default is only 64 KiB).
            "SHELL:-s STACK_SIZE=8MB"
            "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=2MB"
            # ~25 threads per session (15 guest XThreads, 8 SDK host threads,
            # the PROXY_TO_PTHREAD main thread, the WasmFS OPFS worker).
            # Pre-spawned Workers avoid instantiating the module lazily mid-game.
            -sPTHREAD_POOL_SIZE=32
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
            --use-port=emdawnwebgpu
        )
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
