list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(hotpath_tests
    test_build_gate.cpp
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
