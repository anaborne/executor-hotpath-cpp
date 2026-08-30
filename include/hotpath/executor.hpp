#pragma once

// The executor process from socket accept to the point where `executor_server.py` calls
// `dispatch()`, ported from `ipc/executor_server.py`. The REST client is a fake, as it is in
// `benchmarks/latency_bench.py`, so `ExecutorConfig::dispatch` is where a real order would go.
//
// One thread, one listener, one connection at a time. The Python serves connections concurrently
// on an event loop and spawns each fire as its own task; the poller opens exactly one connection
// and the benchmark drives one wake at a time, so the concurrency is not part of what is being
// measured. PORT-FIDELITY.md records the divergence.
//
// The read loop's order is part of the protocol: read the body, stamp, decode, ack, then fire. The
// ack precedes the fire so a dispatch's network round trip never lands inside the `wake_recv` span
// the poller reads back.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hotpath/killswitch.hpp"
#include "hotpath/orders.hpp"
#include "hotpath/protocol.hpp"

namespace hotpath {

struct FireRequest {
    const OrderTemplate* order;
    double price_dollars;
    std::int64_t exchange_index;
    std::string_view correlation_id;
    std::string_view correlation_group;
    bool dry_run;
};

// Decode plus ack, the span the Python executor records as `wake_recv`.
struct WakeRecvEvent {
    std::string_view correlation_id;
    std::int64_t started_at_ms;
    std::int64_t ended_at_ms;
    double duration_ms;
    bool dry_run;
};

enum class FireOutcome : std::uint8_t {
    Dispatched,
    RefusedUntradeablePrice,
    RefusedKillSwitch,
};

struct ExecutorStats {
    std::uint64_t frames_read = 0;
    std::uint64_t acks_accepted = 0;
    std::uint64_t acks_rejected = 0;
    std::uint64_t fires_dispatched = 0;
    std::uint64_t fires_refused_price = 0;
    std::uint64_t fires_refused_kill_switch = 0;
};

struct ExecutorConfig {
    std::filesystem::path socket_path;
    int backlog = 16;
    KillSwitch* kill_switch = nullptr;
    std::function<void(const FireRequest&)> dispatch = nullptr;
    std::function<void(const WakeRecvEvent&)> record_wake_recv = nullptr;
};

class ExecutorServer {
public:
    explicit ExecutorServer(ExecutorConfig config);
    ExecutorServer(const ExecutorServer&) = delete;
    ExecutorServer& operator=(const ExecutorServer&) = delete;
    ExecutorServer(ExecutorServer&&) = delete;
    ExecutorServer& operator=(ExecutorServer&&) = delete;
    ~ExecutorServer();

    // Names the syscall that failed and its errno, or nothing. Unlinks a stale socket file first:
    // a crashed executor leaves one behind and bind then fails with EADDRINUSE.
    [[nodiscard]] std::optional<std::string> listen();

    // Accepts one peer and serves it until it disconnects. False once stopped.
    bool serve_one_connection();

    void serve_forever();

    // Wakes a blocked accept, and shuts down the connection being served so a read blocked
    // between wakes returns too. Callable from a signal handler or another thread: everything it
    // does is an atomic store, a `write` and a `shutdown`.
    void stop() noexcept;

    [[nodiscard]] const ExecutorStats& stats() const noexcept { return stats_; }

    // Public because the fire path is worth testing without a socket in front of it.
    FireOutcome handle_fire(const WakeMessage& message);

    [[nodiscard]] OrderTemplates& templates() noexcept { return templates_; }

private:
    void serve_connection(int fd);
    bool send_frame(int fd, const std::vector<std::byte>& frame);

    ExecutorConfig config_;
    OrderTemplates templates_;
    ExecutorStats stats_;
    std::vector<std::byte> body_;
    std::vector<std::byte> ack_;
    int listen_fd_ = -1;
    int wake_read_fd_ = -1;
    int wake_write_fd_ = -1;
    std::atomic<int> connection_fd_ = -1;
    std::atomic<bool> stopping_ = false;
};

}  // namespace hotpath
