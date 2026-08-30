// What `latency_bench.py` reads back is the contract, so these assertions go through SQLite rather
// than through the sink's own accessors: the columns, their types, the metadata string, and the
// `user_version` stamp that decides whether a later Python open migrates or raises.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "hotpath/telemetry.hpp"

#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>

namespace {

using hotpath::LatencyEvent;
using hotpath::LatencyStage;
using hotpath::TelemetrySink;

std::filesystem::path unique_db() {
    static std::atomic<int> counter{0};
    return std::filesystem::temp_directory_path() /
           ("hotpath-telemetry-" + std::to_string(counter.fetch_add(1)) + ".db");
}

class Reader {
public:
    explicit Reader(const std::filesystem::path& path) {
        REQUIRE(sqlite3_open(path.c_str(), &handle_) == SQLITE_OK);
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;

    ~Reader() { sqlite3_close(handle_); }

    [[nodiscard]] std::int64_t integer(const std::string& sql) {
        sqlite3_stmt* statement = prepare(sql);
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    [[nodiscard]] double real(const std::string& sql) {
        sqlite3_stmt* statement = prepare(sql);
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        const double value = sqlite3_column_double(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    [[nodiscard]] std::string text(const std::string& sql) {
        sqlite3_stmt* statement = prepare(sql);
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        const auto* bytes = sqlite3_column_text(statement, 0);
        std::string value = bytes == nullptr ? std::string() : reinterpret_cast<const char*>(bytes);
        sqlite3_finalize(statement);
        return value;
    }

    void exec(const std::string& sql) {
        REQUIRE(sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }

private:
    sqlite3_stmt* prepare(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        REQUIRE(sqlite3_prepare_v2(handle_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
        return statement;
    }

    sqlite3* handle_ = nullptr;
};

LatencyEvent wake_recv(std::string_view correlation_id) {
    return LatencyEvent{
        .correlation_id = correlation_id,
        .stage = LatencyStage::WakeRecv,
        .started_at_ms = 1'787'290'000'000,
        .ended_at_ms = 1'787'290'000'001,
        .duration_ms = 0.004251,
        .dry_run = false,
    };
}

}  // namespace

TEST_CASE("every stage carries the name db.py validates against", "[telemetry]") {
    CHECK(hotpath::stage_name(LatencyStage::DetectFire) == "detect_fire");
    CHECK(hotpath::stage_name(LatencyStage::IngestFetch) == "ingest_fetch");
    CHECK(hotpath::stage_name(LatencyStage::Decision) == "decision");
    CHECK(hotpath::stage_name(LatencyStage::WakeSend) == "wake_send");
    CHECK(hotpath::stage_name(LatencyStage::WakeRecv) == "wake_recv");
    CHECK(hotpath::stage_name(LatencyStage::OrderBuild) == "order_build");
    CHECK(hotpath::stage_name(LatencyStage::Sign) == "sign");
    CHECK(hotpath::stage_name(LatencyStage::DispatchSend) == "dispatch_send");
    CHECK(hotpath::stage_name(LatencyStage::DispatchAck) == "dispatch_ack");
    CHECK(hotpath::stage_name(LatencyStage::TelemetryWrite) == "telemetry_write");
}

TEST_CASE("opening a fresh database builds the two tables and stamps the schema version",
          "[telemetry]") {
    const std::filesystem::path path = unique_db();
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        sink.close();
    }

    Reader reader(path);
    CHECK(reader.integer("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN "
                         "('correlations', 'latency_events')") == 2);
    CHECK(reader.integer("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name LIKE "
                         "'idx_latency_events_%'") == 3);
    // `migrations.py::SCHEMA_VERSION`. A zero here sends the next Python open into the migration
    // that alters `orders_fired`, a table this file does not have yet, and that open raises.
    CHECK(reader.integer("PRAGMA user_version") == 6);
    CHECK(reader.text("PRAGMA journal_mode") == "wal");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a recorded wake_recv reads back in the columns latency_bench queries", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        REQUIRE(sink.record(wake_recv("bench-0")));
        sink.close();
        CHECK(sink.stats().rows_written == 1);
        CHECK(sink.stats().queue_depth == 0);
    }

    Reader reader(path);
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events") == 1);
    CHECK(reader.text("SELECT correlation_id FROM latency_events") == "bench-0");
    CHECK(reader.text("SELECT stage FROM latency_events") == "wake_recv");
    CHECK(reader.integer("SELECT started_at_ms FROM latency_events") == 1'787'290'000'000);
    CHECK(reader.integer("SELECT ended_at_ms FROM latency_events") == 1'787'290'000'001);
    CHECK(reader.real("SELECT duration_ms FROM latency_events") == 0.004251);
    // Byte for byte what `executor_server.py::_DRY_RUN_METADATA` puts on the same column.
    CHECK(reader.text("SELECT metadata_json FROM latency_events") == R"({"dry_run": false})");
    CHECK(reader.text("SELECT typeof(duration_ms) FROM latency_events") == "real");
    CHECK(reader.text("SELECT typeof(started_at_ms) FROM latency_events") == "integer");
    CHECK(reader.integer("SELECT created_at_ms > 0 FROM latency_events") == 1);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a dry run writes the other metadata string", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        LatencyEvent event = wake_recv("bench-dry");
        event.dry_run = true;
        REQUIRE(sink.record(event));
        sink.close();
    }

    Reader reader(path);
    CHECK(reader.text("SELECT metadata_json FROM latency_events") == R"({"dry_run": true})");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("the correlations parent is written once per id", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        for (int i = 0; i < 3; ++i) {
            REQUIRE(sink.record(wake_recv("bench-repeat")));
        }
        REQUIRE(sink.record(wake_recv("bench-other")));
        sink.close();
        CHECK(sink.stats().rows_written == 4);
    }

    Reader reader(path);
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events") == 4);
    CHECK(reader.integer("SELECT COUNT(*) FROM correlations") == 2);
    // The join `latency_bench.py` and the dashboard both rely on. A latency row whose parent is
    // missing is a row the foreign key should never have let through.
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events e LEFT JOIN correlations c ON "
                         "c.correlation_id = e.correlation_id WHERE c.correlation_id IS NULL") ==
          0);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("more rows than one batch holds all land", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    constexpr int kRows = (static_cast<int>(TelemetrySink::kMaxBatchRows) * 3) + 7;
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        for (int i = 0; i < kRows; ++i) {
            REQUIRE(sink.record(wake_recv("bench-batch")));
        }
        sink.close();
        CHECK(sink.stats().rows_written == static_cast<std::uint64_t>(kRows));
        CHECK(sink.stats().rows_dropped_ring_full == 0);
    }

    Reader reader(path);
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events") == kRows);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("close flushes what was still queued", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    constexpr int kRows = 2000;
    {
        TelemetrySink sink(path, 4096);
        REQUIRE_FALSE(sink.open().has_value());
        for (int i = 0; i < kRows; ++i) {
            REQUIRE(sink.record(wake_recv("bench-flush")));
        }
        sink.close();
    }

    Reader reader(path);
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events") == kRows);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a full ring drops the row and counts it", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    // Never opened, so nothing drains and the ring fills on the fourth push.
    TelemetrySink sink(path, 4);

    for (int i = 0; i < 4; ++i) {
        CHECK(sink.record(wake_recv("bench-full")));
    }
    CHECK_FALSE(sink.record(wake_recv("bench-full")));
    CHECK_FALSE(sink.record(wake_recv("bench-full")));

    const hotpath::TelemetryStats stats = sink.stats();
    CHECK(stats.rows_dropped_ring_full == 2);
    CHECK(stats.queue_depth == 4);
    CHECK(stats.rows_written == 0);
}

TEST_CASE("a correlation_id too long for a slot is dropped rather than truncated", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    TelemetrySink sink(path, 64);

    const std::string oversized(TelemetrySink::kMaxCorrelationIdBytes + 1, 'x');
    const std::string longest(TelemetrySink::kMaxCorrelationIdBytes, 'x');
    CHECK_FALSE(sink.record(wake_recv(oversized)));
    CHECK(sink.record(wake_recv(longest)));

    const hotpath::TelemetryStats stats = sink.stats();
    CHECK(stats.rows_dropped_oversized_id == 1);
    CHECK(stats.rows_dropped_ring_full == 0);
    CHECK(stats.queue_depth == 1);
}

TEST_CASE("a database the Python already stamped keeps its own schema version", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    {
        Reader reader(path);
        reader.exec(
            "CREATE TABLE correlations (correlation_id TEXT PRIMARY KEY, created_at_ms INTEGER); "
            "CREATE TABLE latency_events (id INTEGER PRIMARY KEY AUTOINCREMENT, correlation_id "
            "TEXT NOT NULL REFERENCES correlations (correlation_id), stage TEXT NOT NULL, "
            "started_at_ms INTEGER NOT NULL, ended_at_ms INTEGER NOT NULL, duration_ms REAL NOT "
            "NULL, metadata_json TEXT, created_at_ms INTEGER NOT NULL DEFAULT 0); "
            "PRAGMA user_version = 3");
    }
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        REQUIRE(sink.record(wake_recv("bench-existing")));
        sink.close();
    }

    Reader reader(path);
    // Stamping over a Python database would claim migrations 4 to 6 had run against it. The
    // version belongs to whichever process created the file.
    CHECK(reader.integer("PRAGMA user_version") == 3);
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events") == 1);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("opening twice is idempotent and closing twice is safe", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    TelemetrySink sink(path);

    REQUIRE_FALSE(sink.open().has_value());
    REQUIRE_FALSE(sink.open().has_value());
    REQUIRE(sink.record(wake_recv("bench-idempotent")));
    sink.close();
    sink.close();
    CHECK(sink.stats().rows_written == 1);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("a directory that does not exist yet is created for the database", "[telemetry]") {
    const std::filesystem::path directory = unique_db();
    const std::filesystem::path path = directory / "nested" / "telemetry.db";
    {
        TelemetrySink sink(path);
        REQUIRE_FALSE(sink.open().has_value());
        sink.close();
    }

    CHECK(std::filesystem::exists(path));

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST_CASE("the recording thread and the writer thread agree on the row count", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    constexpr int kRows = 20'000;
    std::uint64_t refused = 0;
    {
        TelemetrySink sink(path, 256);
        REQUIRE_FALSE(sink.open().has_value());
        for (int i = 0; i < kRows; ++i) {
            const std::string id = "bench-" + std::to_string(i);
            if (!sink.record(wake_recv(id))) {
                ++refused;
            }
        }
        sink.close();
        // Every row either reached SQLite or is on the drop counter. A row that is on neither is
        // the failure this counter exists to make impossible to miss.
        const hotpath::TelemetryStats stats = sink.stats();
        CHECK(stats.rows_written + stats.rows_dropped_ring_full == kRows);
        CHECK(stats.rows_dropped_ring_full == refused);
        CHECK(stats.rows_dropped_write_failed == 0);
        CHECK(stats.queue_depth == 0);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// The counter no ordinary run can reach, because `LatencyStage` is an enum and every other column
// is bound from a fixed-width field. The only way to make SQLite refuse a row is to hand this a
// table that refuses it, which is what a database left behind by a schema the executor does not
// know about would look like.
TEST_CASE("a row SQLite refuses is dropped and counted, not retried forever", "[telemetry]") {
    const std::filesystem::path path = unique_db();
    {
        Reader reader(path);
        reader.exec(
            "CREATE TABLE correlations (correlation_id TEXT PRIMARY KEY, created_at_ms INTEGER); "
            "CREATE TABLE latency_events (id INTEGER PRIMARY KEY AUTOINCREMENT, correlation_id "
            "TEXT NOT NULL REFERENCES correlations (correlation_id), stage TEXT NOT NULL CHECK "
            "(stage = 'sign'), started_at_ms INTEGER NOT NULL, ended_at_ms INTEGER NOT NULL, "
            "duration_ms REAL NOT NULL, metadata_json TEXT, created_at_ms INTEGER NOT NULL "
            "DEFAULT 0)");
    }

    TelemetrySink sink(path);
    REQUIRE_FALSE(sink.open().has_value());
    REQUIRE(sink.record(wake_recv("bench-refused")));
    LatencyEvent accepted = wake_recv("bench-accepted");
    accepted.stage = LatencyStage::Sign;
    REQUIRE(sink.record(accepted));
    sink.close();

    const hotpath::TelemetryStats stats = sink.stats();
    CHECK(stats.rows_dropped_write_failed == 1);
    // The per-row fallback is what saves this one. Both rows shared a batch, and the rollback the
    // refused row triggered took the good row down with it before the retry put it back.
    CHECK(stats.rows_written == 1);

    Reader reader(path);
    CHECK(reader.integer("SELECT COUNT(*) FROM latency_events") == 1);
    CHECK(reader.text("SELECT stage FROM latency_events") == "sign");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
