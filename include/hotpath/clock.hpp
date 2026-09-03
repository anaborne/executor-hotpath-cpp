#pragma once

// Ported from `runtime/clock.py`. The poller stamps `recv_ns` and this process measures against
// it, so the two must agree on which monotonic clock they mean. `clock_domain()` is the token that
// says so, and it has to be built the same way on both sides or every cross-process span is
// silently dropped. Nothing in this process consumes the token yet.

#include <cstdint>
#include <string>

namespace hotpath {

[[nodiscard]] std::int64_t monotonic_ns() noexcept;

[[nodiscard]] std::int64_t wall_clock_ms() noexcept;

// Hostname and boot epoch floored to a 60-second bucket, as `gethostname():1787289960`. Computed
// once on first call.
[[nodiscard]] const std::string& clock_domain();

}  // namespace hotpath
