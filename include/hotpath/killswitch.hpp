#pragma once

// Ported from `runtime/killswitch.py`. A file exists at a known path and new real dispatch stops.
// The mechanism has to stay a bare `stat`: the operator engages it from a shell against a process
// that may be too wedged to run a signal handler.

#include <cstdint>
#include <filesystem>
#include <string>

namespace hotpath {

class KillSwitch {
public:
    static constexpr std::int64_t kDefaultTtlNs = 1'000'000'000;

    explicit KillSwitch(std::filesystem::path path, std::int64_t ttl_ns = kDefaultTtlNs);

    // Cached for `ttl_ns`, which bounds the syscall rate and bounds the delay between an operator
    // engaging the switch and this process honouring it by the same number.
    [[nodiscard]] bool is_engaged() noexcept;

    // Empty when the switch is released or the file cannot be read.
    [[nodiscard]] std::string reason() const;

    void engage(const std::string& reason);

    void release();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void invalidate() noexcept;

    std::filesystem::path path_;
    std::int64_t ttl_ns_;
    std::int64_t checked_at_ns_ = 0;
    bool cached_ = false;
    bool has_cached_ = false;
};

}  // namespace hotpath
