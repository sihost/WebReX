# rexaudio - Audio subsystem library
# Audio processing and XMA decoding for Xbox 360 emulation

add_library(rexaudio STATIC
    
    audio_driver.cpp
    audio_system.cpp
    xma_context.cpp
    xma_decoder.cpp
    xma_register_file.cpp
    # NOP backend (always available)
    nop/nop_audio_system.cpp
)
if(NOT EMSCRIPTEN)
    # SDL backend (not built for the web)
    target_sources(rexaudio PRIVATE
        sdl/sdl_audio_system.cpp
        sdl/sdl_audio_driver.cpp
    )
else()
    # Browser backend: AudioWorklet fed from a SharedArrayBuffer ring. The worklet source
    # (web/rex_audio_worklet.js) is compiled in with #embed.
    target_sources(rexaudio PRIVATE
        web/web_audio_output.cpp
        web/web_audio_system.cpp
    )
    set_source_files_properties(web/web_audio_output.cpp PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/rex_audio_worklet.js")
endif()
add_library(rex::audio ALIAS rexaudio)

target_include_directories(rexaudio PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(rexaudio
    PUBLIC rexcore
    PRIVATE libavcodec libavutil
)
if(NOT EMSCRIPTEN)
    target_link_libraries(rexaudio PUBLIC SDL3::SDL3)
endif()
