#include "hotpath/killswitch.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "hotpath/clock.hpp"

namespace hotpath {

KillSwitch::KillSwitch(std::filesystem::path path, std::int64_t ttl_ns)
    : path_(std::move(path)), ttl_ns_(ttl_ns) {}

bool KillSwitch::is_engaged() noexcept {
    const std::int64_t now = monotonic_ns();
    if (has_cached_ && now - checked_at_ns_ < ttl_ns_) {
        return cached_;
    }

    std::error_code ec;
    cached_ = std::filesystem::exists(path_, ec) && !ec;
    has_cached_ = true;
    checked_at_ns_ = now;
    return cached_;
}

std::string KillSwitch::reason() const {
    std::ifstream file(path_);
    if (!file) {
        return {};
    }

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return {};
    }
    text.resize(end + 1);
    return text;
}

void KillSwitch::engage(const std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    std::ofstream file(path_, std::ios::trunc);
    file << reason << '\n';
    invalidate();
}

void KillSwitch::release() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    invalidate();
}

void KillSwitch::invalidate() noexcept {
    has_cached_ = false;
    checked_at_ns_ = 0;
}

}  // namespace hotpath
