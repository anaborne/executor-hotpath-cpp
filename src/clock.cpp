#include "hotpath/clock.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

#include <unistd.h>

namespace hotpath {
namespace {

// Same bucket as `runtime/clock.py`: wide enough to absorb NTP correcting the wall clock under a
// running process, far narrower than any uptime two processes could disagree about.
constexpr double kBootQuantumSeconds = 60.0;

std::string compute_clock_domain() {
    std::array<char, 256> host{};
    if (::gethostname(host.data(), host.size() - 1) != 0) {
        host[0] = '\0';
    }

    // steady_clock is CLOCK_MONOTONIC on Linux and mach_absolute_time on macOS, which is what
    // `time.monotonic()` reads on each, so wall minus uptime lands in the same bucket there.
    const auto wall =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto up =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto boot = static_cast<std::int64_t>(std::floor((wall - up) / kBootQuantumSeconds) *
                                                kBootQuantumSeconds);

    return std::string(host.data()) + ":" + std::to_string(boot);
}

}  // namespace

std::int64_t monotonic_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t wall_clock_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const std::string& clock_domain() {
    static const std::string domain = compute_clock_domain();
    return domain;
}

}  // namespace hotpath
