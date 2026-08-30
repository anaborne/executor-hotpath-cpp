#pragma once

// Ported from `telemetry/db.py`. A `record` call copies the row into a bounded ring and returns;
// one writer thread owns the SQLite connection and appends batches to `latency_events`. The Python
// reaches the same shape with a `queue.Queue` and a thread, for the reason its docstring gives:
// telemetry must never be able to affect the path it is observing.
//
// The bound matters more here than it does there. A saturated Python queue grows memory until the
// process dies; a saturated ring here would stall the read loop mid-wake if pushing could block.
// It cannot. A full ring drops the row and counts it, and the count is printed at shutdown, so a
// writer that fell behind is a number in the run's output rather than a hole in the table.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "hotpath/spsc_ring.hpp"

namespace hotpath {

// `telemetry/db.py::LATENCY_STAGES`, which that module validates as a string against a frozenset
// because a mistyped stage there produced a silent hole in the data the stage was added to
// collect. An enum cannot hold a name that is not on the list, so the check has nowhere to live.
enum class LatencyStage : std::uint8_t {
    DetectFire,
    IngestFetch,
    Decision,
    WakeSend,
    WakeRecv,
    OrderBuild,
    Sign,
    DispatchSend,
    DispatchAck,
    TelemetryWrite,
};

[[nodiscard]] std::string_view stage_name(LatencyStage stage) noexcept;

struct LatencyEvent {
    std::string_view correlation_id;
    LatencyStage stage = LatencyStage::WakeRecv;
    std::int64_t started_at_ms = 0;
    std::int64_t ended_at_ms = 0;
    double duration_ms = 0.0;
    bool dry_run = false;
};

struct TelemetryStats {
    std::uint64_t rows_written = 0;
    std::uint64_t rows_dropped_ring_full = 0;
    std::uint64_t rows_dropped_oversized_id = 0;
    std::uint64_t rows_dropped_write_failed = 0;
    std::uint64_t queue_depth = 0;
};

class TelemetrySink {
public:
    // A `correlation_id` is `uuid4().hex` from `order_dispatcher.py` or a short benchmark label,
    // so 128 bytes is four times the longest one either process produces. A longer one is dropped
    // rather than truncated: a truncated id still satisfies the foreign key and still joins, to
    // the wrong parent, which is worse than a missing row.
    static constexpr std::size_t kMaxCorrelationIdBytes = 128;

    // 8192 rows against `db.py`'s 10,000. Both are sized to absorb a stalled writer rather than to
    // hold a backlog worth keeping, and this one is a power of two because the ring masks.
    static constexpr std::size_t kDefaultCapacity = 8192;

    // Rows per transaction, as `db.py::_MAX_BATCH_SIZE`. Bounds what one commit represents, so a
    // crash loses at most this many and shutdown never waits on an unbounded drain.
    static constexpr std::size_t kMaxBatchRows = 500;

    explicit TelemetrySink(std::filesystem::path db_path, std::size_t capacity = kDefaultCapacity);
    TelemetrySink(const TelemetrySink&) = delete;
    TelemetrySink& operator=(const TelemetrySink&) = delete;
    TelemetrySink(TelemetrySink&&) = delete;
    TelemetrySink& operator=(TelemetrySink&&) = delete;
    ~TelemetrySink();

    // Applies the schema and starts the writer thread. Names the SQLite failure, or nothing.
    [[nodiscard]] std::optional<std::string> open();

    // False when the row was dropped. Never blocks, never allocates, never touches SQLite. Rows
    // recorded before `open()` sit in the ring and are written once the writer starts, where the
    // Python raises; a sink that is never opened reports them as queue depth at shutdown.
    bool record(const LatencyEvent& event) noexcept;

    // Drains what is queued and joins the writer. Idempotent.
    void close();

    [[nodiscard]] TelemetryStats stats() const noexcept;

    [[nodiscard]] const std::filesystem::path& db_path() const noexcept { return db_path_; }

private:
    struct Row {
        std::int64_t started_at_ms;
        std::int64_t ended_at_ms;
        double duration_ms;
        std::uint8_t correlation_id_len;
        LatencyStage stage;
        bool dry_run;
        std::array<char, kMaxCorrelationIdBytes> correlation_id;
    };

    struct Db;

    void run_writer();
    void write_batch(std::span<const Row> batch);
    [[nodiscard]] bool insert(const Row& row);

    std::filesystem::path db_path_;
    std::unique_ptr<Db> db_;
    SpscRing<Row> ring_;
    std::thread writer_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> rows_written_{0};
    std::atomic<std::uint64_t> dropped_ring_full_{0};
    std::atomic<std::uint64_t> dropped_oversized_id_{0};
    std::atomic<std::uint64_t> dropped_write_failed_{0};
};

}  // namespace hotpath
