# rexcore - Core utilities library
# Foundation types, data structures, and threading primitives

add_library(rexcore STATIC
    arena.cpp
    bit_map.cpp
    bit_stream.cpp
    byte_stream.cpp
    clock.cpp
    cvar.cpp
    exception_handler.cpp
    filesystem.cpp
    filesystem_wildcard.cpp
    logging.cpp
    memory.cpp
    mutex.cpp
    ring_buffer.cpp
    sha256.cpp
    string.cpp
    string_buffer.cpp
    threading.cpp
    timer_queue.cpp
    utf8.cpp
    vec128.cpp
)
add_library(rex::core ALIAS rexcore)

# Platform-specific sources
if(WIN32)
    target_sources(rexcore PRIVATE
        atomic_win.cpp
        clock_win.cpp
        dbg_win.cpp
        dynlib_win.cpp
        exception_handler_win.cpp
        filesystem_win.cpp
        fiber_win32.cpp
        mapped_memory_win.cpp
        math_msvc.cpp
        memory_win.cpp
        seh_win.cpp
        socket_win.cpp
        string_win.cpp
        threading_win.cpp
    )
elseif(EMSCRIPTEN)
    # Web (wasm64). Start from the POSIX set; any <name>_web.cpp that exists
    # replaces <name>_posix.cpp (WS-C adds memory/threading/clock/filesystem
    # web backends incrementally). fiber has no POSIX fallback on web (D6).
    set(_rex_core_posix_units
        atomic clock dbg dynlib exception_handler filesystem mapped_memory
        memory seh socket string threading)
    foreach(_unit IN LISTS _rex_core_posix_units)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_unit}_web.cpp")
            target_sources(rexcore PRIVATE ${_unit}_web.cpp)
        else()
            target_sources(rexcore PRIVATE ${_unit}_posix.cpp)
        endif()
    endforeach()
    target_sources(rexcore PRIVATE fiber_web.cpp math_gcc.cpp)
    # Pick up web-only helpers that have no POSIX counterpart.
    file(GLOB _rex_core_web_extra CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*_web.cpp")
    foreach(_f IN LISTS _rex_core_web_extra)
        get_filename_component(_n "${_f}" NAME_WE)
        string(REGEX REPLACE "_web$" "" _n "${_n}")
        if(NOT _n IN_LIST _rex_core_posix_units AND NOT _n STREQUAL "fiber")
            target_sources(rexcore PRIVATE "${_f}")
        endif()
    endforeach()
elseif(UNIX)
    target_sources(rexcore PRIVATE
        atomic_posix.cpp
        clock_posix.cpp
        dbg_posix.cpp
        dynlib_posix.cpp
        exception_handler_posix.cpp
        filesystem_posix.cpp
        fiber_posix.cpp
        mapped_memory_posix.cpp
        math_gcc.cpp
        memory_posix.cpp
        seh_posix.cpp
        socket_posix.cpp
        string_posix.cpp
        threading_posix.cpp
    )
endif()

target_include_directories(rexcore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(rexcore PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/thirdparty/cli11/include
)

if(UNIX AND NOT EMSCRIPTEN)
    target_link_libraries(rexcore PRIVATE pthread rt dl)
endif()

target_link_libraries(rexcore
    PUBLIC
        fmt::fmt
        simde
        tomlplusplus
        spdlog::spdlog
    PRIVATE
        disruptorplus
        utf8cpp
)
