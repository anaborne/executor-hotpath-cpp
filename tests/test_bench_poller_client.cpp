// The benchmark's poller client against the real `ExecutorServer`. This is the cpp-to-cpp
// configuration in miniature, and it is here rather than only in `bench/` because the client is
// the instrument the numbers are read off: a client that dropped a frame, or read the ack of the
// wake before it, would report a latency and never a failure.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "hotpath/executor.hpp"
#include "hotpath/protocol.hpp"

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "poller_client.hpp"

namespace {

std::filesystem::path unique_socket_path(const std::string& tag) {
    static std::atomic<int> counter{0};
    return std::filesystem::temp_directory_path() /
           ("hotpath-bench-" + tag + "-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter.fetch_add(1)) + ".sock");
}

hotpath::WakeMessage wake(const std::string& correlation_id) {
    hotpath::WakeMessage message;
    message.correlation_id = correlation_id;
    message.market_ticker = "KXBENCH-T100";
    message.asset = "BENCH";
    message.kalshi_price = 0.5;
    message.model_probability = 0.55;
    message.fee = 0.01;
    message.edge = 0.04;
    message.wire_price_yes_dollars = 0.5;
    return message;
}

// Extracted so the round-trip case stays under the cognitive-complexity threshold, which every
// assertion inside a loop inside a TEST_CASE counts against.
void check_ack(const hotpath::WakeAck& ack, const std::string& correlation_id) {
    // Each ack has to be the ack for the wake just sent. A client that read them one behind would
    // still produce a plausible round-trip number.
    CHECK(ack.correlation_id == correlation_id);
    CHECK(ack.status == hotpath::WakeAckStatus::Accepted);
    CHECK(ack.schema_version == hotpath::kSchemaVersion);
    CHECK(!ack.reason.has_value());
}

}  // namespace

TEST_CASE("the bench poller client round trips against the executor", "[bench][poller]") {
    const std::filesystem::path socket_path = unique_socket_path("roundtrip");

    std::vector<std::string> fired;
    hotpath::ExecutorServer server(hotpath::ExecutorConfig{
        .socket_path = socket_path,
        .backlog = 4,
        .dispatch =
            [&fired](const hotpath::FireRequest& fire) { fired.emplace_back(fire.correlation_id); },
    });
    REQUIRE(!server.listen().has_value());
    std::jthread worker([&server] { static_cast<void>(server.serve_one_connection()); });

    {
        hotpath::bench::PollerClient client(socket_path);
        REQUIRE(!client.connect(std::chrono::seconds(5)).has_value());

        hotpath::WakeAck ack;
        for (int i = 0; i < 8; ++i) {
            const std::string id = "bench-" + std::to_string(i);
            const std::optional<std::string> failure = client.send_wake(wake(id), ack);
            INFO(failure.value_or(""));
            REQUIRE(!failure.has_value());
            check_ack(ack, id);
        }
    }

    server.stop();
    worker.join();

    CHECK(server.stats().frames_read == 8);
    CHECK(fired.size() == 8);
    CHECK(fired.front() == "bench-0");
    CHECK(fired.back() == "bench-7");
}

TEST_CASE("the bench poller client names a connect that never succeeds", "[bench][poller]") {
    hotpath::bench::PollerClient client(unique_socket_path("absent"));
    const std::optional<std::string> failure = client.connect(std::chrono::milliseconds(50));
    REQUIRE(failure.has_value());
    CHECK(failure.value_or("").starts_with("connect: "));
}

TEST_CASE("the bench poller client refuses to send before it connects", "[bench][poller]") {
    hotpath::bench::PollerClient client(unique_socket_path("unconnected"));
    hotpath::WakeAck ack;
    CHECK(client.send_wake(wake("bench-0"), ack).has_value());
}
