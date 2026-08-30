#include "percentile.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

namespace hotpath::bench {

double percentile(std::span<const double> ordered, double p) {
    if (ordered.empty()) {
        throw std::invalid_argument("percentile of an empty sample");
    }
    if (ordered.size() == 1) {
        return ordered[0];
    }

    // Written as the Python writes it rather than through std::lerp, because the two round
    // differently and the golden fixture compares bit patterns.
    const double rank = static_cast<double>(ordered.size() - 1) * p;
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    if (lo == hi) {
        return ordered[lo];
    }
    return ordered[lo] + ((ordered[hi] - ordered[lo]) * (rank - static_cast<double>(lo)));
}

Stats Stats::from_samples(std::vector<double>& samples_ms, std::size_t warmup) {
    const std::size_t dropped = std::min(warmup, samples_ms.size());
    const std::span<double> kept(samples_ms.begin() + static_cast<std::ptrdiff_t>(dropped),
                                 samples_ms.end());
    std::ranges::sort(kept);

    return Stats{
        .p50_ms = percentile(kept, 0.50),
        .p90_ms = percentile(kept, 0.90),
        .p99_ms = percentile(kept, 0.99),
        .n = kept.size(),
        .warmup = dropped,
    };
}

}  // namespace hotpath::bench
