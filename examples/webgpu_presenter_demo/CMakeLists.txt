# WebGPU presenter demo + self-test (browser, wasm64). Links the installed wasm64 SDK
# (cmake --install of the wasm64-release preset -> <repo>/out/install/wasm64).
# Build: build.bat / build.sh in this directory.
cmake_minimum_required(VERSION 3.25)
project(webgpu_presenter_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue REQUIRED CONFIG)

add_executable(presenter_demo presenter_demo.cpp)
target_link_libraries(presenter_demo PRIVATE rex::ui rex::graphics rex::core)
target_compile_options(presenter_demo PRIVATE --use-port=emdawnwebgpu)
target_link_options(presenter_demo PRIVATE
    "SHELL:-s PROXY_TO_PTHREAD=1"
    "SHELL:-s OFFSCREENCANVAS_SUPPORT=1"
    -lGL
    "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$GL"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s STACK_SIZE=1MB"
    "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=1MB"
    "SHELL:-s PTHREAD_POOL_SIZE=8"
    "SHELL:-s INITIAL_MEMORY=512MB"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    --use-port=emdawnwebgpu
)
set_target_properties(presenter_demo PROPERTIES SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/index.html ${CMAKE_BINARY_DIR}/web/index.html COPYONLY)
