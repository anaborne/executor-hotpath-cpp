// The only test until the protocol lands. It exists to prove the toolchain end to end: compile
// under the project warning set, archive into hotpath_core, link against Catch2, register with
// ctest, and run clean under the sanitizers. Asserting `true` would prove none of that, so it
// calls a symbol that has to come out of the library.

#include "hotpath/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the library links and reports a version", "[build-gate]") {
    REQUIRE_FALSE(hotpath::version().empty());
}
