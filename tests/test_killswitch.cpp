// The file-presence kill switch, including the cache. The TTL is the delay an operator accepts
// between engaging the switch and the process honouring it, so a test that could not observe a
// stale read would not be testing the cache at all.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "hotpath/killswitch.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

std::filesystem::path unique_path(const std::string& tag) {
    static std::atomic<int> counter{0};
    return std::filesystem::temp_directory_path() /
           ("hotpath-" + tag + "-" + std::to_string(counter.fetch_add(1)));
}

}  // namespace

TEST_CASE("the kill switch reads the file the operator wrote", "[killswitch]") {
    const std::filesystem::path path = unique_path("switch-file");
    hotpath::KillSwitch kill_switch(path, 0);

    CHECK_FALSE(kill_switch.is_engaged());
    CHECK(kill_switch.reason().empty());

    kill_switch.engage("disk pressure, see incident 4");
    CHECK(kill_switch.is_engaged());
    CHECK(kill_switch.reason() == "disk pressure, see incident 4");

    kill_switch.release();
    CHECK_FALSE(kill_switch.is_engaged());
    kill_switch.release();
    CHECK_FALSE(kill_switch.is_engaged());
}

TEST_CASE("a cached kill switch check does not see the file appear", "[killswitch]") {
    const std::filesystem::path path = unique_path("switch-ttl");
    hotpath::KillSwitch kill_switch(path, 60'000'000'000);

    CHECK_FALSE(kill_switch.is_engaged());
    std::ofstream(path) << "engaged out from under the cache\n";
    CHECK_FALSE(kill_switch.is_engaged());

    hotpath::KillSwitch fresh(path, 60'000'000'000);
    CHECK(fresh.is_engaged());
    std::filesystem::remove(path);
}
