#pragma once

// Ported from `runtime/clock.py`. The poller stamps `recv_ns` and this process measures against
// it, so the two must agree on which monotonic clock they mean. `clock_domain()` is the token that
// says so, and it has to be built the same way on both sides or every cross-process span is
// silently dropped.

#include <cstdint>
#include <string>

namespace hotpath {

[[nodiscard]] std::int64_t monotonic_ns() noexcept;

[[nodiscard]] std::int64_t wall_clock_ms() noexcept;

// Hostname and quantized boot epoch, as `gethostname():1787290000`. Computed once on first call.
[[nodiscard]] const std::string& clock_domain();

}  // namespace hotpath
