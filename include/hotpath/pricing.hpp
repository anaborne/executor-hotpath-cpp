#pragma once

// Ported from `executor_server._snap_to_grid`. Kalshi rejects an off-grid price outright, and the
// grid is read from the wake's own `price_ranges` rather than assumed to be cents, because the
// 15-minute crypto series already quotes three ranges with tighter steps in the tails.

#include <span>

#include "hotpath/protocol.hpp"

namespace hotpath {

// Python's `round()` is half-to-even; `std::round` is half-away-from-zero. This is CPython's
// `float.__round__` for `ndigits=None` transcribed, so a price on a half-step lands the same side
// of the grid in both processes.
[[nodiscard]] double round_half_even(double value) noexcept;

// Nearest multiple of the step of the first range containing `price`. No match, which includes an
// empty `ranges` and a price outside every range, falls back to one cent.
[[nodiscard]] double snap_to_grid(double price, std::span<const PriceRange> ranges) noexcept;

}  // namespace hotpath
