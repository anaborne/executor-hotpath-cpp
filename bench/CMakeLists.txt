# The support library is built unconditionally, and the executable is what HOTPATH_BUILD_BENCH
# gates. The percentile estimator and the poller client are covered by the test suite, which the
# dev and sanitizer presets build and the release preset does not run for numbers; leaving them
# behind the option would put the one piece of arithmetic BENCHMARK.md calls invisibly wrong
# outside every gate that could catch it.
add_library(hotpath_bench_support STATIC
    percentile.cpp
    poller_client.cpp
)
add_library(hotpath::bench_support ALIAS hotpath_bench_support)

target_include_directories(hotpath_bench_support PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(hotpath_bench_support PUBLIC hotpath::core PRIVATE hotpath::warnings)

if(HOTPATH_BUILD_BENCH)
    add_executable(executor_hotpath_bench bench_main.cpp)
    target_link_libraries(executor_hotpath_bench
        PRIVATE hotpath::bench_support hotpath::warnings OpenSSL::Crypto)
endif()
