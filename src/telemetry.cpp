#include "hotpath/telemetry.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "hotpath/clock.hpp"

#include <sqlite3.h>

namespace hotpath {

namespace {

// `telemetry/schema.sql`, restricted to the two tables this process writes. That file is
// CREATE TABLE IF NOT EXISTS throughout, so a Python `TelemetryDB.initialize()` against the same
// file fills in the other seven tables afterwards and neither side has to know what the other
// created.
constexpr const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS correlations (
    correlation_id  TEXT    PRIMARY KEY,
    created_at_ms   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS latency_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    correlation_id  TEXT    NOT NULL REFERENCES correlations (correlation_id),
    stage           TEXT    NOT NULL,
    started_at_ms   INTEGER NOT NULL,
    ended_at_ms     INTEGER NOT NULL,
    duration_ms     REAL    NOT NULL,
    metadata_json   TEXT,
    created_at_ms   INTEGER NOT NULL DEFAULT (CAST(strftime('%s', 'now') AS INTEGER) * 1000)
);

CREATE INDEX IF NOT EXISTS idx_latency_events_stage ON latency_events (stage);
CREATE INDEX IF NOT EXISTS idx_latency_events_correlation_id ON latency_events (correlation_id);
CREATE INDEX IF NOT EXISTS idx_latency_events_started_at ON latency_events (started_at_ms);
)sql";

// `telemetry/migrations.py::SCHEMA_VERSION`, the length of that module's migration list at
// prediction-market-infra e3fd937. Stamped only on a database this process created, which is the
// branch `initialize()` takes on a database `schema.sql` has just built. Every migration past the
// first alters a table the executor never writes, and `schema.sql` declares all of them in their
// post-migration shape, so a Python open that finds this file and skips the migrations lands where
// running them would have. Stamping zero instead would send it into `_add_order_fill_columns`
// against an `orders_fired` that does not exist yet, and the open would raise.
constexpr int kTelemetrySchemaVersion = 6;

// `db.py::_BUSY_TIMEOUT_MS`. Both processes hold write connections to this file and SQLite's
// default is to fail instantly on contention.
constexpr int kBusyTimeoutMs = 5000;

// `executor_server.py::_DRY_RUN_METADATA`, pre-serialized there for the same reason: it is written
// on every latency row and there are exactly two of them.
constexpr std::array<std::string_view, 2> kDryRunMetadata = {R"({"dry_run": false})",
                                                             R"({"dry_run": true})"};

// The producer never signals the consumer, so the read loop pays nothing for a wake and the cost
// of an idle writer is one timed wakeup per interval on its own thread. A condition variable would
// move that cost onto the path being measured.
constexpr auto kIdlePollInterval = std::chrono::microseconds(500);

std::string sqlite_failure(sqlite3* handle, std::string_view what) {
    return std::string(what) + ": " + sqlite3_errmsg(handle);
}

int exec(sqlite3* handle, const char* sql) {
    return sqlite3_exec(handle, sql, nullptr, nullptr, nullptr);
}

bool has_latency_events(sqlite3* handle) {
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'latency_events'";
    if (sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return found;
}

// SQLITE_TRANSIENT, so SQLite copies. The alternative is reasoning about how long a binding into
// the drained batch has to outlive the step that read it, on the one thread where a copy of at
// most 128 bytes costs nothing.
int bind_view(sqlite3_stmt* statement, int index, std::string_view text) {
    return sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()),
                             SQLITE_TRANSIENT);
}

}  // namespace

struct TelemetrySink::Db {
    sqlite3* handle = nullptr;
    sqlite3_stmt* correlation_insert = nullptr;
    sqlite3_stmt* event_insert = nullptr;

    Db() = default;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&&) = delete;
    Db& operator=(Db&&) = delete;

    ~Db() {
        sqlite3_finalize(correlation_insert);
        sqlite3_finalize(event_insert);
        sqlite3_close(handle);
    }
};

std::string_view stage_name(LatencyStage stage) noexcept {
    switch (stage) {
        case LatencyStage::DetectFire:
            return "detect_fire";
        case LatencyStage::IngestFetch:
            return "ingest_fetch";
        case LatencyStage::Decision:
            return "decision";
        case LatencyStage::WakeSend:
            return "wake_send";
        case LatencyStage::WakeRecv:
            return "wake_recv";
        case LatencyStage::OrderBuild:
            return "order_build";
        case LatencyStage::Sign:
            return "sign";
        case LatencyStage::DispatchSend:
            return "dispatch_send";
        case LatencyStage::DispatchAck:
            return "dispatch_ack";
        case LatencyStage::TelemetryWrite:
            return "telemetry_write";
    }
    return "";
}

TelemetrySink::TelemetrySink(std::filesystem::path db_path, std::size_t capacity)
    : db_path_(std::move(db_path)), ring_(capacity) {}

TelemetrySink::~TelemetrySink() {
    close();
}

std::optional<std::string> TelemetrySink::open() {
    if (writer_.joinable()) {
        return std::nullopt;
    }

    if (db_path_.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(db_path_.parent_path(), ec);
    }

    auto db = std::make_unique<Db>();
    if (sqlite3_open(db_path_.c_str(), &db->handle) != SQLITE_OK) {
        return sqlite_failure(db->handle, "sqlite3_open");
    }

    const bool preexisting = has_latency_events(db->handle);
    // WAL so the dashboard's read-only connection can query this file while the writer holds it.
    // Persisted in the file, so it only matters on the open that creates it.
    if (exec(db->handle, "PRAGMA journal_mode = WAL") != SQLITE_OK) {
        return sqlite_failure(db->handle, "PRAGMA journal_mode");
    }
    if (exec(db->handle, "PRAGMA foreign_keys = ON") != SQLITE_OK) {
        return sqlite_failure(db->handle, "PRAGMA foreign_keys");
    }
    if (exec(db->handle, "PRAGMA synchronous = NORMAL") != SQLITE_OK) {
        return sqlite_failure(db->handle, "PRAGMA synchronous");
    }
    if (sqlite3_busy_timeout(db->handle, kBusyTimeoutMs) != SQLITE_OK) {
        return sqlite_failure(db->handle, "sqlite3_busy_timeout");
    }
    if (exec(db->handle, kSchemaSql) != SQLITE_OK) {
        return sqlite_failure(db->handle, "schema");
    }
    if (!preexisting) {
        // PRAGMA accepts no bound parameter, and the value is a constant in this file.
        const std::string stamp =
            "PRAGMA user_version = " + std::to_string(kTelemetrySchemaVersion);
        if (exec(db->handle, stamp.c_str()) != SQLITE_OK) {
            return sqlite_failure(db->handle, "PRAGMA user_version");
        }
    }

    if (sqlite3_prepare_v2(db->handle,
                           "INSERT OR IGNORE INTO correlations (correlation_id, created_at_ms) "
                           "VALUES (?, ?)",
                           -1, &db->correlation_insert, nullptr) != SQLITE_OK) {
        return sqlite_failure(db->handle, "prepare correlations insert");
    }
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO latency_events (correlation_id, stage, started_at_ms, "
                           "ended_at_ms, duration_ms, metadata_json) VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &db->event_insert, nullptr) != SQLITE_OK) {
        return sqlite_failure(db->handle, "prepare latency_events insert");
    }

    db_ = std::move(db);
    stopping_.store(false, std::memory_order_release);
    writer_ = std::thread(&TelemetrySink::run_writer, this);
    return std::nullopt;
}

bool TelemetrySink::record(const LatencyEvent& event) noexcept {
    if (event.correlation_id.size() > kMaxCorrelationIdBytes) {
        dropped_oversized_id_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Row row{};
    row.started_at_ms = event.started_at_ms;
    row.ended_at_ms = event.ended_at_ms;
    row.duration_ms = event.duration_ms;
    row.correlation_id_len = static_cast<std::uint8_t>(event.correlation_id.size());
    row.stage = event.stage;
    row.dry_run = event.dry_run;
    if (!event.correlation_id.empty()) {
        std::memcpy(row.correlation_id.data(), event.correlation_id.data(),
                    event.correlation_id.size());
    }

    if (!ring_.try_push(row)) {
        dropped_ring_full_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TelemetrySink::close() {
    if (writer_.joinable()) {
        stopping_.store(true, std::memory_order_release);
        writer_.join();
    }
    db_.reset();
}

TelemetryStats TelemetrySink::stats() const noexcept {
    return TelemetryStats{
        .rows_written = rows_written_.load(std::memory_order_relaxed),
        .rows_dropped_ring_full = dropped_ring_full_.load(std::memory_order_relaxed),
        .rows_dropped_oversized_id = dropped_oversized_id_.load(std::memory_order_relaxed),
        .rows_dropped_write_failed = dropped_write_failed_.load(std::memory_order_relaxed),
        .queue_depth = ring_.size(),
    };
}

void TelemetrySink::run_writer() {
    std::vector<Row> batch;
    batch.reserve(kMaxBatchRows);

    for (;;) {
        // Read before the drain, not after. The producer stops pushing before it sets this, so an
        // empty drain that follows a true read is the end of the stream and nothing is left behind.
        const bool draining = stopping_.load(std::memory_order_acquire);

        batch.clear();
        Row row{};
        while (batch.size() < kMaxBatchRows && ring_.try_pop(row)) {
            batch.push_back(row);
        }

        if (batch.empty()) {
            if (draining) {
                return;
            }
            std::this_thread::sleep_for(kIdlePollInterval);
            continue;
        }
        write_batch(batch);
    }
}

void TelemetrySink::write_batch(std::span<const Row> batch) {
    bool committed = false;
    if (exec(db_->handle, "BEGIN") == SQLITE_OK) {
        bool all_inserted = true;
        for (const Row& row : batch) {
            if (!insert(row)) {
                all_inserted = false;
                break;
            }
        }
        committed = all_inserted && exec(db_->handle, "COMMIT") == SQLITE_OK;
        if (!committed) {
            exec(db_->handle, "ROLLBACK");
        }
    }

    if (committed) {
        rows_written_.fetch_add(batch.size(), std::memory_order_relaxed);
        return;
    }

    // One row SQLite refuses would otherwise take every good row batched with it down on the
    // rollback, so the batch is retried a row at a time and exactly the bad one is lost.
    // `db.py::_write_batch` falls back the same way.
    for (const Row& row : batch) {
        if (exec(db_->handle, "BEGIN") == SQLITE_OK && insert(row) &&
            exec(db_->handle, "COMMIT") == SQLITE_OK) {
            rows_written_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        exec(db_->handle, "ROLLBACK");
        dropped_write_failed_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool TelemetrySink::insert(const Row& row) {
    const std::string_view correlation_id(row.correlation_id.data(), row.correlation_id_len);

    // The parent row first, or the foreign key on `latency_events.correlation_id` rejects the
    // child. `db.py::_insert` does the same upsert ahead of every table that carries the column.
    sqlite3_stmt* parent = db_->correlation_insert;
    sqlite3_reset(parent);
    if (bind_view(parent, 1, correlation_id) != SQLITE_OK ||
        sqlite3_bind_int64(parent, 2, wall_clock_ms()) != SQLITE_OK ||
        sqlite3_step(parent) != SQLITE_DONE) {
        return false;
    }

    sqlite3_stmt* event = db_->event_insert;
    sqlite3_reset(event);
    return bind_view(event, 1, correlation_id) == SQLITE_OK &&
           bind_view(event, 2, stage_name(row.stage)) == SQLITE_OK &&
           sqlite3_bind_int64(event, 3, row.started_at_ms) == SQLITE_OK &&
           sqlite3_bind_int64(event, 4, row.ended_at_ms) == SQLITE_OK &&
           sqlite3_bind_double(event, 5, row.duration_ms) == SQLITE_OK &&
           bind_view(event, 6, kDryRunMetadata[static_cast<std::size_t>(row.dry_run)]) ==
               SQLITE_OK &&
           sqlite3_step(event) == SQLITE_DONE;
}

}  // namespace hotpath
