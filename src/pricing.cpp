#include "hotpath/pricing.hpp"

#include <cmath>
#include <span>

#include "hotpath/protocol.hpp"

namespace hotpath {
namespace {

constexpr double kCentGrid = 0.01;

}  // namespace

double round_half_even(double value) noexcept {
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) == 0.5) {
        return 2.0 * std::round(value / 2.0);
    }
    return rounded;
}

double snap_to_grid(double price, std::span<const PriceRange> ranges) noexcept {
    for (const PriceRange& range : ranges) {
        if (range.step > 0.0 && range.start <= price && price <= range.end) {
            return round_half_even(price / range.step) * range.step;
        }
    }
    return round_half_even(price / kCentGrid) * kCentGrid;
}

}  // namespace hotpath
