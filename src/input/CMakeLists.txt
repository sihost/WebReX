# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rexinput STATIC
    input_system.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp xinput/xinput_input_driver.cpp)
elseif(EMSCRIPTEN)
    # No SDL on the web (browser input is WS-F); MnK + NOP drivers only.
else()
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)
endif()

target_link_libraries(rexinput PUBLIC rexcore)
if(NOT EMSCRIPTEN)
    target_link_libraries(rexinput PUBLIC SDL3::SDL3)
endif()
