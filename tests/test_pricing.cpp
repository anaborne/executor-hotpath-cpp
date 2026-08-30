// snap_to_grid against a corpus the Python produced. `tests/golden/snap_to_grid.tsv` holds a
// price, the wake's price_ranges, and the value `_snap_to_grid` returned, every number as its
// IEEE-754 bits. The corpus leads with the quotients that land exactly on a half, where Python's
// round-half-to-even and std::round's round-half-away-from-zero disagree.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hotpath/pricing.hpp"
#include "hotpath/protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

double from_bits(const std::string& hex) {
    return std::bit_cast<double>(std::stoull(hex, nullptr, 16));
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t hit = text.find(separator, start);
        if (hit == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, hit - start));
        start = hit + 1;
    }
    return parts;
}

std::vector<hotpath::PriceRange> parse_ranges(const std::string& field) {
    std::vector<hotpath::PriceRange> ranges;
    if (field.empty()) {
        return ranges;
    }
    for (const std::string& triple : split(field, ';')) {
        const std::vector<std::string> parts = split(triple, ':');
        REQUIRE(parts.size() == 3);
        ranges.push_back(hotpath::PriceRange{
            .start = from_bits(parts[0]), .end = from_bits(parts[1]), .step = from_bits(parts[2])});
    }
    return ranges;
}

}  // namespace

TEST_CASE("every snap in the corpus lands where Python put it", "[pricing]") {
    const std::filesystem::path path =
        std::filesystem::path(HOTPATH_GOLDEN_DIR) / "snap_to_grid.tsv";
    std::ifstream corpus(path);
    REQUIRE(corpus.is_open());

    std::string line;
    std::size_t checked = 0;
    while (std::getline(corpus, line)) {
        const std::vector<std::string> columns = split(line, '\t');
        REQUIRE(columns.size() == 3);

        const double price = from_bits(columns[0]);
        const std::vector<hotpath::PriceRange> ranges = parse_ranges(columns[1]);
        const double expected = from_bits(columns[2]);

        INFO("price bits " << columns[0] << " ranges " << columns[1]);
        CHECK(std::bit_cast<std::uint64_t>(hotpath::snap_to_grid(price, ranges)) ==
              std::bit_cast<std::uint64_t>(expected));
        ++checked;
    }

    CHECK(checked == 424);
}

TEST_CASE("a tie rounds to even, as Python's round does", "[pricing]") {
    CHECK(hotpath::round_half_even(0.5) == 0.0);
    CHECK(hotpath::round_half_even(1.5) == 2.0);
    CHECK(hotpath::round_half_even(2.5) == 2.0);
    CHECK(hotpath::round_half_even(3.5) == 4.0);
    CHECK(hotpath::round_half_even(-0.5) == 0.0);
    CHECK(hotpath::round_half_even(-2.5) == -2.0);
    CHECK(hotpath::round_half_even(0.4) == 0.0);
    CHECK(hotpath::round_half_even(0.6) == 1.0);
}

TEST_CASE("a price outside every range falls back to one cent", "[pricing]") {
    const std::vector<hotpath::PriceRange> tail{{.start = 0.9, .end = 1.0, .step = 0.001}};
    CHECK(hotpath::snap_to_grid(0.5039, tail) == Catch::Approx(0.5));
    CHECK(hotpath::snap_to_grid(0.5039, {}) == Catch::Approx(0.5));
}

TEST_CASE("a zero step is not a grid", "[pricing]") {
    const std::vector<hotpath::PriceRange> broken{{.start = 0.0, .end = 1.0, .step = 0.0}};
    CHECK(hotpath::snap_to_grid(0.5039, broken) == Catch::Approx(0.5));
}

TEST_CASE("the first range containing the price wins", "[pricing]") {
    const std::vector<hotpath::PriceRange> ladder{{.start = 0.0, .end = 0.05, .step = 0.001},
                                                  {.start = 0.05, .end = 0.95, .step = 0.01},
                                                  {.start = 0.95, .end = 1.0, .step = 0.001}};
    CHECK(hotpath::snap_to_grid(0.0234, ladder) == Catch::Approx(0.023));
    CHECK(hotpath::snap_to_grid(0.5234, ladder) == Catch::Approx(0.52));
    CHECK(hotpath::snap_to_grid(0.9734, ladder) == Catch::Approx(0.973));
}
