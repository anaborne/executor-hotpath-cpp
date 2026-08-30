list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(hotpath_tests
    test_build_gate.cpp
    test_double_format.cpp
    test_executor.cpp
    test_killswitch.cpp
    test_pricing.cpp
    test_protocol.cpp
    test_protocol_golden.cpp
    test_spsc_ring.cpp
)

# src/ is on the path because the JSON layer is internal to the library and tested directly. Its
# contract is orjson's output, and asserting that through the protocol API alone would report a
# formatting difference as a frame mismatch three layers up.
target_include_directories(hotpath_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Read at run time rather than embedded, so regenerating a golden frame does not recompile the
# suite and a diff on a frame file is a diff on the thing being asserted.
target_compile_definitions(hotpath_tests
    PRIVATE HOTPATH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
)

target_link_libraries(hotpath_tests
    PRIVATE
        hotpath::core
        hotpath::warnings
        Catch2::Catch2WithMain
)

# PRE_TEST rather than the default POST_BUILD. Discovery runs the freshly built binary, so under
# ASan a fault during static initialization fails the build with an error naming a CMake post-build
# command. Deferred to ctest time it is reported as a test failure with the sanitizer output.
catch_discover_tests(hotpath_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60
)
