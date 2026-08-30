// The contract test for the percentile estimator. `tests/golden/percentiles.tsv` holds vectors and
// the values `latency_bench.py::_percentile` returns for them, read out of that file's own AST by
// the generator, so a comparison here is a comparison against the Python.
//
// Bit patterns, not decimals. The whole failure BENCHMARK.md section 3 exists to prevent is an
// estimator that agrees to the printed precision and disagrees underneath, and a fixture written
// as decimal text could not tell the two apart.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "percentile.hpp"

namespace {

double from_bits(const std::string& hex) {
    REQUIRE(hex.size() == 16);
    const auto bits = static_cast<std::uint64_t>(std::stoull(hex, nullptr, 16));
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string to_bits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::string hex(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        hex[15 - i] = "0123456789abcdef"[(bits >> (i * 4)) & 0xFU];
    }
    return hex;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

struct Case {
    std::string name;
    std::vector<double> values;
    std::vector<std::pair<double, std::string>> expected;
};

// `remaining` counts the value lines still owed by the open `vector` header, so a truncated block
// fails here rather than silently shortening a sample.
void apply_line(const std::vector<std::string>& fields, std::vector<Case>& cases,
                std::size_t& remaining) {
    if (fields[0] == "vector") {
        REQUIRE(fields.size() == 3);
        REQUIRE(remaining == 0);
        cases.push_back(Case{.name = fields[1], .values = {}, .expected = {}});
        remaining = static_cast<std::size_t>(std::stoull(fields[2]));
        cases.back().values.reserve(remaining);
        return;
    }

    REQUIRE(!cases.empty());
    if (fields[0] == "p") {
        REQUIRE(fields.size() == 3);
        REQUIRE(remaining == 0);
        cases.back().expected.emplace_back(from_bits(fields[1]), fields[2]);
    } else {
        REQUIRE(remaining > 0);
        cases.back().values.push_back(from_bits(fields[0]));
        --remaining;
    }
}

std::vector<Case> read_cases() {
    std::ifstream file(std::filesystem::path(HOTPATH_GOLDEN_DIR) / "percentiles.tsv");
    REQUIRE(file.is_open());

    std::vector<Case> cases;
    std::string line;
    std::size_t remaining = 0;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            apply_line(split_tabs(line), cases, remaining);
        }
    }
    REQUIRE(remaining == 0);
    return cases;
}

}  // namespace

TEST_CASE("percentile reproduces latency_bench.py::_percentile", "[percentile][golden]") {
    const std::vector<Case> cases = read_cases();
    REQUIRE(cases.size() == 7);

    for (const Case& sample : cases) {
        INFO("vector " << sample.name);
        REQUIRE(!sample.expected.empty());

        // The Python sorts inside `_percentile`; this one takes a sorted span, so the ordering is
        // the caller's job and the fixture's `unsorted` vector is what says so.
        std::vector<double> ordered = sample.values;
        std::ranges::sort(ordered);

        for (const auto& [probability, expected_bits] : sample.expected) {
            INFO("p = " << probability);
            CHECK(to_bits(hotpath::bench::percentile(ordered, probability)) == expected_bits);
        }
    }
}

TEST_CASE("Stats discards the warm-up before it sorts", "[percentile]") {
    // The warm-up samples are the largest values in the vector, so a Stats that sorted first and
    // dropped afterwards would throw away the fast tail and report the slow one.
    std::vector<double> samples{9.0, 8.0, 7.0, 1.0, 2.0, 3.0, 4.0};
    const hotpath::bench::Stats stats = hotpath::bench::Stats::from_samples(samples, 3);

    CHECK(stats.n == 4);
    CHECK(stats.warmup == 3);
    CHECK(stats.p50_ms == 2.5);
    CHECK(stats.p99_ms < 4.0);
}

TEST_CASE("Stats refuses a run the warm-up consumed entirely", "[percentile]") {
    // Reporting the one surviving sample, or zeros, would put a number in the CSV that no
    // measurement stands behind.
    std::vector<double> samples{5.0, 6.0};
    CHECK_THROWS(hotpath::bench::Stats::from_samples(samples, 2));
}

TEST_CASE("percentile refuses an empty sample", "[percentile]") {
    const std::vector<double> empty;
    CHECK_THROWS(hotpath::bench::percentile(empty, 0.5));
}
