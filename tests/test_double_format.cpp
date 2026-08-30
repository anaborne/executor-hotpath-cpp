// orjson's float output is the one place a general-purpose C++ serializer would silently differ,
// so the formatter is checked against a corpus orjson itself produced rather than against a
// hand-written expectation. tests/golden/doubles.tsv holds a value's IEEE-754 bits beside the
// string orjson.dumps returned for it, bits rather than decimal text so nothing lossy sits between
// the value under test and the expectation.
//
// The corpus is 1000 values: the boundaries where the fixed-versus-scientific rule can break, then
// a seeded half-and-half sample of the wire's own domain and of the whole double range by random
// bit pattern. PORT-FIDELITY.md states the layout rule and how wide it was checked before any of
// this was written.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "json.hpp"

namespace {

std::string format(double value) {
    std::array<char, hotpath::json::kDoubleBufferSize> buffer{};
    return {buffer.data(), hotpath::json::format_double(value, buffer)};
}

}  // namespace

TEST_CASE("every double in the corpus formats as orjson formats it", "[json]") {
    const std::filesystem::path path = std::filesystem::path(HOTPATH_GOLDEN_DIR) / "doubles.tsv";
    std::ifstream corpus(path);
    REQUIRE(corpus.is_open());

    std::string line;
    std::size_t checked = 0;
    while (std::getline(corpus, line)) {
        const std::size_t tab = line.find('\t');
        REQUIRE(tab != std::string::npos);
        const std::uint64_t bits = std::stoull(line.substr(0, tab), nullptr, 16);
        const std::string expected = line.substr(tab + 1);

        const auto value = std::bit_cast<double>(bits);
        // INFO rather than a bare CHECK: a failure has to name the value, and the value is only
        // legible as its bits.
        INFO("bits " << line.substr(0, tab) << " expected " << expected);
        CHECK(format(value) == expected);
        ++checked;
    }

    CHECK(checked == 1000);
}

TEST_CASE("the notation boundaries are where orjson puts them", "[json]") {
    CHECK(format(1e15) == "1000000000000000.0");
    CHECK(format(1e16) == "1e+16");
    CHECK(format(1e-5) == "0.00001");
    CHECK(format(1e-6) == "1e-6");
    CHECK(format(0.0) == "0.0");
    CHECK(format(-0.0) == "-0.0");
}

TEST_CASE("a value with no digits is null, as orjson writes it", "[json]") {
    CHECK(format(std::numeric_limits<double>::quiet_NaN()) == "null");
    CHECK(format(std::numeric_limits<double>::infinity()) == "null");
    CHECK(format(-std::numeric_limits<double>::infinity()) == "null");
}
