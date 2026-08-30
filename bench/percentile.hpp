#pragma once

// `latency_bench.py::_percentile`, the type-7 estimator: linear interpolation on
// `rank = (n - 1) * p`. BENCHMARK.md section 3 pins it because a nearest-rank implementation
// would print into the same columns, agree with the Python to two decimal places, and be wrong,
// with nothing in either output to say so.

#include <cstddef>
#include <span>
#include <vector>

namespace hotpath::bench {

// `ordered` sorted ascending and non-empty, `p` in [0, 1]. Throws on an empty sample, which is a
// harness that measured nothing rather than a value the caller could do anything with.
[[nodiscard]] double percentile(std::span<const double> ordered, double p);

// No mean. BENCHMARK.md section 3 reports p50/p90/p99 and nothing else, and `statistics.mean` is
// exact-then-rounded where a running sum is not, so a mean column would differ in its last digit
// for a reason that has nothing to do with either implementation's speed.
struct Stats {
    double p50_ms = 0.0;
    double p90_ms = 0.0;
    double p99_ms = 0.0;
    std::size_t n = 0;
    std::size_t warmup = 0;

    // Drops the leading `warmup` samples, then sorts the rest of `samples_ms` in place.
    [[nodiscard]] static Stats from_samples(std::vector<double>& samples_ms, std::size_t warmup);
};

}  // namespace hotpath::bench
