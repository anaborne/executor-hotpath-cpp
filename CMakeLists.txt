cmake_minimum_required(VERSION 3.25)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(executor_hotpath
        VERSION 0.1.0
        DESCRIPTION "C++ reimplementation of the prediction-market-infra executor hot path"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(HOTPATH_WERROR "Treat compiler warnings as errors" ON)
option(HOTPATH_BUILD_TESTS "Build the test suite" ON)
option(HOTPATH_BUILD_BENCH "Build the microbenchmark executable" ON)
set(HOTPATH_SANITIZE "" CACHE STRING
    "Comma-separated -fsanitize list, for example address,undefined. Empty disables.")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(SQLite3 REQUIRED)

# Apple stopped shipping OpenSSL headers and CMake does not look inside a keg-only Homebrew prefix
# on its own, so on macOS a plain find_package reports nothing found with 3.6 sitting installed.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    execute_process(COMMAND brew --prefix openssl@3
                    OUTPUT_VARIABLE HOMEBREW_OPENSSL_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(HOMEBREW_OPENSSL_PREFIX)
        set(OPENSSL_ROOT_DIR "${HOMEBREW_OPENSSL_PREFIX}")
    endif()
endif()

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# Sanitizers are directory-wide and must be included before anything is added below, so that
# Catch2 is instrumented too. A binary where only some translation units carry ASan's container
# annotations reports container-overflow every time a std::vector crosses the boundary.
include(Sanitizers)
include(CompilerWarnings)

if(HOTPATH_BUILD_TESTS)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)

    # SYSTEM marks Catch2's include directories -isystem. Without it, -Wold-style-cast and
    # -Wconversion fire inside catch_test_macros.hpp and are charged to our test files.
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.15.3
        GIT_SHALLOW TRUE
        SYSTEM
    )

    set(CATCH_INSTALL_DOCS OFF)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
endif()

# After FetchContent, deliberately. This initializes COMPILE_WARNING_AS_ERROR when a target is
# created, so setting it here leaves every Catch2 target already built without it.
set(CMAKE_COMPILE_WARNING_AS_ERROR ${HOTPATH_WERROR})

add_subdirectory(src)

# Before tests, which link the benchmark support library.
add_subdirectory(bench)

if(HOTPATH_BUILD_TESTS)
    add_subdirectory(tests)
endif()
