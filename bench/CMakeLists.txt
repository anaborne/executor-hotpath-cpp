# No benchmark targets yet. The microbenchmarks land with the code they measure; this file exists
# so HOTPATH_BUILD_BENCH resolves and the release preset configures.
add_library(hotpath_bench_support INTERFACE)
target_link_libraries(hotpath_bench_support INTERFACE hotpath::core hotpath::warnings)
