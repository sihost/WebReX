# Unit Tests for ReXGlue core components

add_executable(unit_tests
    memory/heap_allocation_test.cpp
    memory/fold_web_test.cpp
    kernel/object_table_test.cpp
    kernel/xecrypt_rsa_test.cpp
    kernel/xnet_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/sha256_test.cpp
    core/stream_test.cpp
    core/byte_order_test.cpp
    core/chrono_test.cpp
    codegen/config_test.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src  # private headers under test (kernel/xboxkrnl/xecrypt_rsa.h)
)
if(WIN32)
    target_link_libraries(unit_tests PRIVATE bcrypt)
endif()

target_link_libraries(unit_tests PRIVATE
    rexcore
    rexcodegen
    rexkernel
    Catch2::Catch2WithMain
)

set_target_properties(unit_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/bin
)

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
