// The read loop and the fire path. The socket cases drive a real Unix domain socket, because the
// ordering under test is decode, ack, fire, and an in-process call cannot show that the ack left
// before the fire ran.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "hotpath/clock.hpp"
#include "hotpath/executor.hpp"
#include "hotpath/killswitch.hpp"
#include "hotpath/protocol.hpp"
#include "hotpath/telemetry.hpp"

#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using hotpath::Direction;
using hotpath::ExecutorConfig;
using hotpath::ExecutorServer;
using hotpath::FireOutcome;
using hotpath::KillSwitch;
using hotpath::WakeMessage;

struct RecordedFire {
    std::string ticker;
    Direction side = Direction::Yes;
    double price_dollars = 0.0;
    std::string correlation_id;
    bool dry_run = false;
    const void* order_address = nullptr;
};

WakeMessage wake(const std::string& correlation_id = "c-1") {
    WakeMessage message;
    message.correlation_id = correlation_id;
    message.market_ticker = "KXBTCD-26AUG2917-T112750";
    message.asset = "BTC";
    message.direction = Direction::Yes;
    message.kalshi_price = 0.56;
    message.model_probability = 0.62;
    message.fee = 0.02;
    message.edge = 0.04;
    message.decision_ts_ms = 1787000000000;
    message.sent_at_ms = 1787000000001;
    message.sent_at_ns = 1234567890;
    message.recv_ns = 1234567000;
    message.clock_domain = "studio.local:1787290000";
    message.wire_price_yes_dollars = 0.5234;
    message.exchange_index = 7;
    message.available_size_contracts = 250.0;
    message.correlation_group = "KXBTCD-26AUG2917";
    return message;
}

std::filesystem::path unique_socket_path(const std::string& tag) {
    static std::atomic<int> counter{0};
    const std::string name = "hotpath-" + tag + "-" + std::to_string(::getpid()) + "-" +
                             std::to_string(counter.fetch_add(1)) + ".sock";
    return std::filesystem::temp_directory_path() / name;
}

int connect_to(const std::filesystem::path& socket_path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path.string();
    REQUIRE(path.size() < sizeof(address.sun_path));
    std::memcpy(&address.sun_path[0], path.data(), path.size());

    // A misbehaving server must fail the test rather than hang it until ctest's timeout.
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

void send_all(int fd, std::span<const std::byte> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t wrote = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        REQUIRE(wrote > 0);
        sent += static_cast<std::size_t>(wrote);
    }
}

// The frame body, or empty when the server closed the connection. No frame this protocol sends
// has an empty body, so the two cannot be confused.
std::vector<std::byte> read_frame(int fd) {
    std::array<std::byte, hotpath::kFrameLengthBytes> prefix{};
    std::size_t filled = 0;
    while (filled < prefix.size()) {
        const ssize_t got = ::read(fd, prefix.data() + filled, prefix.size() - filled);
        if (got <= 0) {
            return {};
        }
        filled += static_cast<std::size_t>(got);
    }

    const hotpath::Decoded<std::uint32_t> length = hotpath::decode_frame_length(prefix);
    REQUIRE(length.has_value());

    std::vector<std::byte> body(length.value());
    filled = 0;
    while (filled < body.size()) {
        const ssize_t got = ::read(fd, body.data() + filled, body.size() - filled);
        if (got <= 0) {
            return {};
        }
        filled += static_cast<std::size_t>(got);
    }
    return body;
}

// One server on one thread, serving a single connection, torn down with the fixture.
class Harness {
public:
    explicit Harness(const std::string& tag, KillSwitch* kill_switch = nullptr,
                     hotpath::TelemetrySink* sink = nullptr)
        : socket_path_(unique_socket_path(tag)),
          server_(ExecutorConfig{
              .socket_path = socket_path_,
              .backlog = 4,
              .kill_switch = kill_switch,
              .dispatch =
                  [this](const hotpath::FireRequest& fire) {
                      fires_.push_back(
                          RecordedFire{.ticker = fire.order->ticker,
                                       .side = fire.order->outcome_side,
                                       .price_dollars = fire.price_dollars,
                                       .correlation_id = std::string(fire.correlation_id),
                                       .dry_run = fire.dry_run,
                                       .order_address = fire.order});
                  },
              .record_wake_recv =
                  [this, sink](const hotpath::WakeRecvEvent& event) {
                      wake_recv_.push_back(event);
                      if (sink != nullptr) {
                          static_cast<void>(sink->record(
                              hotpath::LatencyEvent{.correlation_id = event.correlation_id,
                                                    .stage = hotpath::LatencyStage::WakeRecv,
                                                    .started_at_ms = event.started_at_ms,
                                                    .ended_at_ms = event.ended_at_ms,
                                                    .duration_ms = event.duration_ms,
                                                    .dry_run = event.dry_run}));
                      }
                  },
          }) {
        const std::optional<std::string> failure = server_.listen();
        INFO(failure.value_or(""));
        REQUIRE(!failure.has_value());
        worker_ = std::jthread([this] { static_cast<void>(server_.serve_one_connection()); });
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    ~Harness() {
        server_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void join_after_disconnect() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] const std::filesystem::path& socket_path() const { return socket_path_; }

    [[nodiscard]] const std::vector<RecordedFire>& fires() const { return fires_; }

    [[nodiscard]] const std::vector<hotpath::WakeRecvEvent>& wake_recv() const {
        return wake_recv_;
    }

    [[nodiscard]] const hotpath::ExecutorStats& stats() const { return server_.stats(); }

private:
    std::filesystem::path socket_path_;
    std::vector<RecordedFire> fires_;
    std::vector<hotpath::WakeRecvEvent> wake_recv_;
    ExecutorServer server_;
    std::jthread worker_;
};

std::vector<std::byte> frame_of(std::string_view body) {
    std::vector<std::byte> frame(hotpath::kFrameLengthBytes + body.size());
    for (std::size_t i = 0; i < hotpath::kFrameLengthBytes; ++i) {
        frame[i] = static_cast<std::byte>((body.size() >> (8U * (3U - i))) & 0xFFU);
    }
    for (std::size_t i = 0; i < body.size(); ++i) {
        frame[hotpath::kFrameLengthBytes + i] = static_cast<std::byte>(body[i]);
    }
    return frame;
}

hotpath::WakeAck ack_from(const std::vector<std::byte>& body) {
    const hotpath::Decoded<hotpath::WakeAck> decoded = hotpath::decode_wake_ack(body);
    REQUIRE(decoded.has_value());
    return decoded.value();
}

// The poller calls `orjson.loads` on the whole ack frame before it reaches the `reason` it logs,
// so a rejected ack has to be valid UTF-8 even when the frame that caused it was not. Strict, the
// way CPython's decoder is: an overlong form is refused, and so is any code point in the surrogate
// range or past U+10FFFF.
bool is_valid_utf8(std::span<const std::byte> body) {
    std::size_t i = 0;
    while (i < body.size()) {
        const auto lead = static_cast<unsigned char>(body[i]);
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        if ((lead & 0xE0U) == 0xC0U) {
            extra = 1;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            extra = 2;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            extra = 3;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= body.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto continuation = static_cast<unsigned char>(body[i + k]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        const bool overlong = (extra == 1 && code_point < 0x80U) ||
                              (extra == 2 && code_point < 0x800U) ||
                              (extra == 3 && code_point < 0x10000U);
        if (overlong || (code_point >= 0xD800U && code_point <= 0xDFFFU) ||
            code_point > 0x10FFFFU) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

}  // namespace

TEST_CASE("a wake is acked with its own correlation id and then fired", "[executor]") {
    Harness harness("accept");
    const int client = connect_to(harness.socket_path());

    const WakeMessage message = wake("c-accepted");
    send_all(client, hotpath::encode_frame(message));

    const std::vector<std::byte> reply = read_frame(client);
    REQUIRE_FALSE(reply.empty());
    const hotpath::WakeAck ack = ack_from(reply);
    CHECK(ack.status == hotpath::WakeAckStatus::Accepted);
    CHECK(ack.correlation_id == "c-accepted");
    CHECK_FALSE(ack.reason.has_value());
    CHECK(ack.received_at_ms > 0);
    CHECK(ack.schema_version == hotpath::kSchemaVersion);

    ::close(client);
    harness.join_after_disconnect();

    REQUIRE(harness.fires().size() == 1);
    CHECK(harness.fires()[0].ticker == message.market_ticker);
    CHECK(harness.fires()[0].side == Direction::Yes);
    CHECK(harness.fires()[0].correlation_id == "c-accepted");
    // 0.5234 snapped to the default one-cent grid.
    CHECK(harness.fires()[0].price_dollars == Catch::Approx(0.52));

    REQUIRE(harness.wake_recv().size() == 1);
    CHECK(harness.wake_recv()[0].duration_ms >= 0.0);
    CHECK(harness.wake_recv()[0].started_at_ms > 0);
    CHECK(harness.stats().frames_read == 1);
    CHECK(harness.stats().acks_accepted == 1);
}

TEST_CASE("the ack leaves before the fire runs", "[executor]") {
    const std::filesystem::path socket_path = unique_socket_path("order");
    std::mutex mutex;
    std::condition_variable ack_seen;
    bool acked = false;

    ExecutorServer server(ExecutorConfig{
        .socket_path = socket_path,
        // Blocks until the client has the ack in hand. Were the fire to run first, this would hold
        // the read loop and the ack would never arrive, failing on the client's receive timeout
        // rather than deadlocking the suite.
        .dispatch =
            [&](const hotpath::FireRequest&) {
                std::unique_lock<std::mutex> lock(mutex);
                ack_seen.wait_for(lock, std::chrono::seconds(5), [&] { return acked; });
            },
    });
    REQUIRE(!server.listen().has_value());
    const std::jthread worker([&] { static_cast<void>(server.serve_one_connection()); });

    const int client = connect_to(socket_path);
    send_all(client, hotpath::encode_frame(wake("c-order")));

    const std::vector<std::byte> reply = read_frame(client);
    REQUIRE_FALSE(reply.empty());
    CHECK(ack_from(reply).correlation_id == "c-order");

    {
        const std::scoped_lock lock(mutex);
        acked = true;
    }
    ack_seen.notify_all();
    ::close(client);
}

TEST_CASE("a frame that does not decode is rejected and the connection survives", "[executor]") {
    Harness harness("reject");
    const int client = connect_to(harness.socket_path());

    send_all(client, frame_of(R"({"schema_version":4,)"));

    const std::vector<std::byte> rejection = read_frame(client);
    REQUIRE_FALSE(rejection.empty());
    const hotpath::WakeAck ack = ack_from(rejection);
    CHECK(ack.status == hotpath::WakeAckStatus::Rejected);
    // Python sends the empty string here too: `cls(**data)` raised before there was a message to
    // read a correlation_id off.
    CHECK(ack.correlation_id.empty());
    CHECK(ack.reason.has_value());
    CHECK_FALSE(ack.reason.value_or("").empty());

    send_all(client, hotpath::encode_frame(wake("c-after")));
    const std::vector<std::byte> reply = read_frame(client);
    REQUIRE_FALSE(reply.empty());
    CHECK(ack_from(reply).correlation_id == "c-after");

    ::close(client);
    harness.join_after_disconnect();

    CHECK(harness.stats().acks_rejected == 1);
    CHECK(harness.stats().acks_accepted == 1);
    // A rejected frame is not a wake_recv row in the Python either.
    CHECK(harness.wake_recv().size() == 1);
    CHECK(harness.fires().size() == 1);
}

// Three frames that make the decoder build a reason string out of a byte from the frame itself.
// Each byte goes out as hex, so the ack stays decodable to a poller that only wanted to log the
// reason.
TEST_CASE("a rejected ack is valid UTF-8 when the frame that caused it is not", "[executor]") {
    Harness harness("reject-utf8");
    const int client = connect_to(harness.socket_path());

    const std::array<std::string, 3> bodies{
        std::string("{\"schema_version\":4\xff}"),
        std::string("{\"asset\"\xc3:1}"),
        std::string("{\"asset\":\"\\\xff\"}"),
    };

    for (const std::string& body : bodies) {
        INFO(body);
        send_all(client, frame_of(body));
        const std::vector<std::byte> rejection = read_frame(client);
        REQUIRE_FALSE(rejection.empty());
        CHECK(is_valid_utf8(rejection));
        const hotpath::WakeAck ack = ack_from(rejection);
        CHECK(ack.status == hotpath::WakeAckStatus::Rejected);
        CHECK_FALSE(ack.reason.value_or("").empty());
    }

    ::close(client);
    harness.join_after_disconnect();

    CHECK(harness.stats().acks_rejected == 3);
    CHECK(harness.fires().empty());
}

TEST_CASE("an oversize length prefix closes the connection", "[executor]") {
    Harness harness("oversize");
    const int client = connect_to(harness.socket_path());

    const std::array<std::byte, 4> prefix{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                          std::byte{0xFF}};
    send_all(client, prefix);

    CHECK(read_frame(client).empty());
    ::close(client);
    harness.join_after_disconnect();
    CHECK(harness.stats().frames_read == 0);
}

TEST_CASE("stop wakes a read blocked between wakes", "[executor]") {
    // The SIGTERM path is `stop()` from a signal handler. A signal that lands while the read is
    // in progress interrupts it; one that lands just before the read enters does not, and the
    // pipe only wakes `poll`. The shutdown inside `stop()` covers both, so a connection that has
    // gone quiet is not what keeps the process alive.
    const std::filesystem::path socket_path = unique_socket_path("stop");
    ExecutorServer server(ExecutorConfig{.socket_path = socket_path, .backlog = 4});
    REQUIRE_FALSE(server.listen().has_value());

    std::atomic<bool> returned{false};
    const std::jthread worker([&server, &returned] {
        static_cast<void>(server.serve_one_connection());
        returned.store(true);
    });

    const int client = connect_to(socket_path);
    // Long enough that the server is inside `read` on this connection when stop() is called.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(returned.load());

    server.stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(returned.load());
    CHECK(read_frame(client).empty());
    ::close(client);
}

TEST_CASE("the socket is owner-only", "[executor]") {
    const Harness harness("perms");
    struct stat info{};
    REQUIRE(::stat(harness.socket_path().c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
}

TEST_CASE("a real fire needs a tradeable wire price", "[executor]") {
    ExecutorServer server(ExecutorConfig{.socket_path = unique_socket_path("price")});

    WakeMessage message = wake();
    message.dry_run = false;

    message.wire_price_yes_dollars = 0.0;
    CHECK(server.handle_fire(message) == FireOutcome::RefusedUntradeablePrice);
    message.wire_price_yes_dollars = 1.0;
    CHECK(server.handle_fire(message) == FireOutcome::RefusedUntradeablePrice);
    message.wire_price_yes_dollars = 1.5;
    CHECK(server.handle_fire(message) == FireOutcome::RefusedUntradeablePrice);
    message.wire_price_yes_dollars = 0.01;
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);

    // A shadow fire places no order, so refusing it would thin the detect_fire sample to
    // well-quoted books.
    message.dry_run = true;
    message.wire_price_yes_dollars = 0.0;
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);

    CHECK(server.stats().fires_refused_price == 3);
    CHECK(server.stats().fires_dispatched == 2);
}

TEST_CASE("the kill switch stops a real fire and not a shadow one", "[executor]") {
    const std::filesystem::path path = unique_socket_path("kill");
    KillSwitch kill_switch(path, 0);
    ExecutorServer server(ExecutorConfig{.socket_path = unique_socket_path("kill-socket"),
                                         .kill_switch = &kill_switch});

    WakeMessage message = wake();
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);

    kill_switch.engage("test");
    CHECK(server.handle_fire(message) == FireOutcome::RefusedKillSwitch);

    message.dry_run = true;
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);

    kill_switch.release();
    message.dry_run = false;
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);
}

TEST_CASE("a second wake on one ticker reuses its template", "[executor]") {
    const void* first = nullptr;
    const void* second = nullptr;
    int calls = 0;
    ExecutorServer server(ExecutorConfig{
        .socket_path = unique_socket_path("templates"),
        .dispatch =
            [&](const hotpath::FireRequest& fire) {
                if (calls++ == 0) {
                    first = fire.order;
                } else {
                    second = fire.order;
                }
            },
    });

    const WakeMessage message = wake();
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);
    CHECK(server.handle_fire(message) == FireOutcome::Dispatched);

    CHECK(first != nullptr);
    CHECK(first == second);
}

// The whole path, from a frame on the socket to a row `latency_bench.py`'s query returns. Every
// other telemetry test starts at `record()`; this one starts where the poller does.
TEST_CASE("a wake reaches latency_events through the sink the executable wires", "[executor]") {
    const std::filesystem::path db_path =
        std::filesystem::temp_directory_path() /
        ("hotpath-executor-telemetry-" + std::to_string(::getpid()) + ".db");
    std::error_code ec;
    std::filesystem::remove(db_path, ec);

    hotpath::TelemetrySink sink(db_path);
    REQUIRE_FALSE(sink.open().has_value());

    {
        Harness harness("telemetry", nullptr, &sink);
        const int client = connect_to(harness.socket_path());
        send_all(client, hotpath::encode_frame(wake("c-telemetry")));
        REQUIRE_FALSE(read_frame(client).empty());
        ::close(client);
        harness.join_after_disconnect();
    }
    sink.close();

    sqlite3* handle = nullptr;
    REQUIRE(sqlite3_open(db_path.c_str(), &handle) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    REQUIRE(
        sqlite3_prepare_v2(handle,
                           "SELECT correlation_id, duration_ms, metadata_json FROM latency_events "
                           "WHERE stage = 'wake_recv'",
                           -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) ==
          "c-telemetry");
    CHECK(sqlite3_column_double(statement, 1) > 0.0);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))) ==
          R"({"dry_run": false})");
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(handle);

    CHECK(sink.stats().rows_written == 1);
    std::filesystem::remove(db_path, ec);
}
