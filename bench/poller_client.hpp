#pragma once

// The C++ half of the cpp-to-cpp configuration, and nothing beyond it. `poller_client.py` is
// async, queues, and reads acks on a task of its own; this writes one frame and blocks for its
// ack, so the span it measures is write-to-ack and not the Python's `wake_send`. PORT-FIDELITY.md
// records that. It exists so the Python poller's floor can be separated from the executor's cost,
// which is the only reason BENCHMARK.md section 2 keeps the configuration.
//
// A measurement instrument. No reconnect, no backoff, no queue, no telemetry.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "hotpath/protocol.hpp"

namespace hotpath::bench {

class PollerClient {
public:
    explicit PollerClient(std::filesystem::path socket_path);
    PollerClient(const PollerClient&) = delete;
    PollerClient& operator=(const PollerClient&) = delete;
    PollerClient(PollerClient&&) = delete;
    PollerClient& operator=(PollerClient&&) = delete;
    ~PollerClient();

    // Names the syscall that failed and its errno, or nothing. Retries a refused connect for
    // `timeout`, since the executor may still be between bind and listen, or in another process
    // that has not reached either.
    [[nodiscard]] std::optional<std::string> connect(std::chrono::milliseconds timeout);

    // Writes `message` and blocks until its ack arrives. Names the failure, or nothing.
    [[nodiscard]] std::optional<std::string> send_wake(const WakeMessage& message, WakeAck& ack);

    void close() noexcept;

private:
    std::filesystem::path socket_path_;
    int fd_ = -1;
    std::vector<std::byte> frame_;
    std::vector<std::byte> body_;
};

}  // namespace hotpath::bench
