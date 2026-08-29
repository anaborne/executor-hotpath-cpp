# An interface library rather than add_compile_options, so the flags reach our targets and never
# the FetchContent dependencies linked alongside them.
add_library(hotpath_warnings INTERFACE)
add_library(hotpath::warnings ALIAS hotpath_warnings)

target_compile_options(hotpath_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-align
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wundef
    # Clang rejects an unknown -W option outright once warnings are errors, so the GCC-only ones
    # are gated on the compiler rather than listed unconditionally.
    $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond>
    $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-branches>
    $<$<CXX_COMPILER_ID:GNU>:-Wlogical-op>
)
