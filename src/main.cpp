#include <csignal>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "hotpath/executor.hpp"
#include "hotpath/killswitch.hpp"
#include "hotpath/telemetry.hpp"
#include "hotpath/version.hpp"

namespace {

hotpath::ExecutorServer* g_server = nullptr;

extern "C" void request_stop(int /*signal*/) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

void usage() {
    static_cast<void>(std::fputs(
        "usage: executor_hotpath --socket PATH [--kill-switch PATH] [--telemetry-db PATH]\n",
        stderr));
}

int run(std::span<char*> args) {
    std::string socket_path;
    std::string kill_switch_path;
    std::string telemetry_db_path;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view flag(args[i]);
        const bool has_value = i + 1 < args.size();
        if (flag == "--socket" && has_value) {
            socket_path = args[++i];
        } else if (flag == "--kill-switch" && has_value) {
            kill_switch_path = args[++i];
        } else if (flag == "--telemetry-db" && has_value) {
            telemetry_db_path = args[++i];
        } else if (flag == "--version") {
            const std::string_view name = hotpath::version();
            static_cast<void>(
                std::printf("executor-hotpath %.*s\n", static_cast<int>(name.size()), name.data()));
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    if (socket_path.empty()) {
        usage();
        return 2;
    }

    std::unique_ptr<hotpath::KillSwitch> kill_switch;
    if (!kill_switch_path.empty()) {
        kill_switch = std::make_unique<hotpath::KillSwitch>(kill_switch_path);
    }

    std::unique_ptr<hotpath::TelemetrySink> telemetry;
    if (!telemetry_db_path.empty()) {
        telemetry = std::make_unique<hotpath::TelemetrySink>(telemetry_db_path);
        if (const std::optional<std::string> failure = telemetry->open(); failure.has_value()) {
            static_cast<void>(
                std::fprintf(stderr, "telemetry open failed: %s\n", failure->c_str()));
            return 1;
        }
    }

    std::function<void(const hotpath::WakeRecvEvent&)> record_wake_recv;
    if (telemetry) {
        record_wake_recv = [sink = telemetry.get()](const hotpath::WakeRecvEvent& event) {
            static_cast<void>(sink->record(hotpath::LatencyEvent{
                .correlation_id = event.correlation_id,
                .stage = hotpath::LatencyStage::WakeRecv,
                .started_at_ms = event.started_at_ms,
                .ended_at_ms = event.ended_at_ms,
                .duration_ms = event.duration_ms,
                .dry_run = event.dry_run,
            }));
        };
    }

    hotpath::ExecutorServer server(hotpath::ExecutorConfig{
        .socket_path = socket_path,
        .kill_switch = kill_switch.get(),
        .record_wake_recv = std::move(record_wake_recv),
    });

    if (const std::optional<std::string> failure = server.listen(); failure.has_value()) {
        static_cast<void>(std::fprintf(stderr, "listen failed: %s\n", failure->c_str()));
        return 1;
    }

    g_server = &server;
    // sigaction without SA_RESTART, not signal(): both glibc and macOS install a signal() handler
    // with restart semantics, and a restarted read never reports the stop to a connection sitting
    // idle between wakes.
    struct sigaction stop_action{};
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = 0;
    static_cast<void>(sigaction(SIGINT, &stop_action, nullptr));
    static_cast<void>(sigaction(SIGTERM, &stop_action, nullptr));
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));

    server.serve_forever();
    g_server = nullptr;

    const hotpath::ExecutorStats& stats = server.stats();
    static_cast<void>(
        std::fprintf(stderr,
                     "frames=%llu accepted=%llu rejected=%llu fired=%llu refused_price=%llu "
                     "refused_kill_switch=%llu\n",
                     static_cast<unsigned long long>(stats.frames_read),
                     static_cast<unsigned long long>(stats.acks_accepted),
                     static_cast<unsigned long long>(stats.acks_rejected),
                     static_cast<unsigned long long>(stats.fires_dispatched),
                     static_cast<unsigned long long>(stats.fires_refused_price),
                     static_cast<unsigned long long>(stats.fires_refused_kill_switch)));

    if (telemetry) {
        telemetry->close();
        const hotpath::TelemetryStats counters = telemetry->stats();
        static_cast<void>(std::fprintf(
            stderr,
            "telemetry rows_written=%llu dropped_ring_full=%llu dropped_oversized_id=%llu "
            "dropped_write_failed=%llu\n",
            static_cast<unsigned long long>(counters.rows_written),
            static_cast<unsigned long long>(counters.rows_dropped_ring_full),
            static_cast<unsigned long long>(counters.rows_dropped_oversized_id),
            static_cast<unsigned long long>(counters.rows_dropped_write_failed)));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(std::span<char*>(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& failure) {
        static_cast<void>(std::fprintf(stderr, "executor-hotpath: %s\n", failure.what()));
        return 1;
    }
}
