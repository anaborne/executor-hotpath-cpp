#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "hotpath/executor.hpp"
#include "hotpath/killswitch.hpp"
#include "hotpath/version.hpp"

namespace {

hotpath::ExecutorServer* g_server = nullptr;

extern "C" void request_stop(int /*signal*/) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

void usage() {
    static_cast<void>(
        std::fputs("usage: executor_hotpath --socket PATH [--kill-switch PATH]\n", stderr));
}

int run(std::span<char*> args) {
    std::string socket_path;
    std::string kill_switch_path;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view flag(args[i]);
        const bool has_value = i + 1 < args.size();
        if (flag == "--socket" && has_value) {
            socket_path = args[++i];
        } else if (flag == "--kill-switch" && has_value) {
            kill_switch_path = args[++i];
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

    hotpath::ExecutorServer server(hotpath::ExecutorConfig{
        .socket_path = socket_path,
        .kill_switch = kill_switch.get(),
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
