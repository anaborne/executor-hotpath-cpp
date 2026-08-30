// The four microbenchmarks BENCHMARK.md section 3 names, on the executor side of the wire:
// decode, ack encode, sign, and a cpp-to-cpp round trip. Each is timed the way
// `latency_bench.py` times its own spans, a monotonic stamp on either side of the call, and
// reported through the same type-7 percentile estimator.
//
// Warm-up iterations are discarded and the count is printed on every line and written into every
// CSV row. BENCHMARK.md section 4 left that open and section 9 settles it; the same count is
// discarded in `latency_bench.py`, because a C++ run with warm-up against a Python run without it
// is rigged in exactly the direction this repository wants.
//
// Nothing here asserts a threshold. The numbers go to RESULTS.md at step 7 and the CSV is the
// record.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "hotpath/clock.hpp"
#include "hotpath/executor.hpp"
#include "hotpath/protocol.hpp"
#include "hotpath/signer.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "percentile.hpp"
#include "poller_client.hpp"

namespace {

using hotpath::WakeAck;
using hotpath::WakeMessage;
using hotpath::bench::Stats;

constexpr std::size_t kDefaultIterations = 2000;

// Ten percent of the sample, comfortably past the transients that motivate discarding anything
// (the signer's first blinding setup, a cold socket buffer, a cold page cache) and small enough
// that it cannot absorb a regression that persists.
constexpr std::size_t kDefaultWarmup = 200;

struct Options {
    std::size_t iterations = kDefaultIterations;
    std::size_t warmup = kDefaultWarmup;
    std::filesystem::path csv;
    std::string only;
};

struct Result {
    std::string name;
    Stats stats;
};

double elapsed_ms(std::int64_t started_ns) {
    return static_cast<double>(hotpath::monotonic_ns() - started_ns) / 1'000'000.0;
}

// `latency_bench.py::_platform_tag`, which is deliberately coarse: enough to tell an Apple-silicon
// laptop from an x86 runner, not enough to identify a machine. The third field names the compiler
// where the Python names its interpreter, since that is the analogous thing.
std::string platform_tag() {
    utsname system{};
    if (::uname(&system) != 0) {
        return "unknown";
    }
#ifdef __clang__
    const std::string toolchain = std::string("clang") + __clang_version__;
#elif defined(__GNUC__)
    const std::string toolchain = std::string("gcc") + __VERSION__;
#else
    const std::string toolchain = "unknown";
#endif
    // __clang_version__ carries the configuration and the vendor's build number after the number
    // itself, which would put a different tag on two runs of the same compiler.
    const std::string trimmed = toolchain.substr(0, toolchain.find(' '));
    return std::string(static_cast<const char*>(system.sysname)) + "-" +
           static_cast<const char*>(system.machine) + "-" + trimmed;
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    ::gmtime_r(&now, &parts);
    std::array<char, 32> buffer{};
    const std::size_t written =
        std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return {buffer.data(), written};
}

// The wake `latency_bench.py` sends, field for field, so the decode number is the cost of the
// bytes the py-to-cpp configuration actually carries and not of a message invented here. The
// correlation id is fixed at the first one rather than counted up, because re-encoding a frame
// inside the loop would put orjson's job inside the decoder's span.
WakeMessage bench_wake() {
    WakeMessage message;
    message.correlation_id = "bench-0";
    message.market_ticker = "KXBENCH-T100";
    message.asset = "BENCH";
    message.direction = hotpath::Direction::Yes;
    message.kalshi_price = 0.5;
    message.model_probability = 0.55;
    message.fee = 0.01;
    message.edge = 0.04;
    message.decision_ts_ms = 0;
    message.sent_at_ms = hotpath::wall_clock_ms();
    message.sent_at_ns = hotpath::monotonic_ns();
    message.wire_price_yes_dollars = 0.5;
    return message;
}

Stats bench_decode(const Options& options) {
    const std::vector<std::byte> frame = hotpath::encode_frame(bench_wake());
    const std::span<const std::byte> body(frame.data() + hotpath::kFrameLengthBytes,
                                          frame.size() - hotpath::kFrameLengthBytes);

    std::vector<double> samples;
    samples.reserve(options.warmup + options.iterations);
    for (std::size_t i = 0; i < options.warmup + options.iterations; ++i) {
        const std::int64_t started = hotpath::monotonic_ns();
        const hotpath::Decoded<WakeMessage> decoded = hotpath::decode_wake_message(body);
        samples.push_back(elapsed_ms(started));
        if (!decoded) {
            throw std::runtime_error("decode failed: " + decoded.error().message);
        }
    }
    return Stats::from_samples(samples, options.warmup);
}

Stats bench_encode_ack(const Options& options) {
    const WakeAck ack{
        .schema_version = hotpath::kSchemaVersion,
        .correlation_id = "bench-0",
        .received_at_ms = hotpath::wall_clock_ms(),
        .status = hotpath::WakeAckStatus::Accepted,
        .reason = std::nullopt,
    };

    // Into a buffer held across iterations, which is what the read loop does. Measuring a fresh
    // allocation each time would report the allocator rather than the encoder.
    std::vector<std::byte> out;
    out.reserve(256);

    std::vector<double> samples;
    samples.reserve(options.warmup + options.iterations);
    for (std::size_t i = 0; i < options.warmup + options.iterations; ++i) {
        const std::int64_t started = hotpath::monotonic_ns();
        hotpath::encode_frame_into(ack, out);
        samples.push_back(elapsed_ms(started));
    }
    return Stats::from_samples(samples, options.warmup);
}

struct PkeyDelete {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct BioDelete {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

// Removes the key however the benchmark leaves, including through the throw from a failed load.
class TempDirectory {
public:
    explicit TempDirectory(std::filesystem::path path) : path_(std::move(path)) {
        std::filesystem::create_directories(path_);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// An ephemeral key, never registered, generated for this run. `latency_bench.py::bench_sign` does
// the same, for the same reason: the number is a property of RSA-2048 and the message, and no
// credential has to exist for it.
void write_throwaway_key(const std::filesystem::path& path) {
    const std::unique_ptr<EVP_PKEY, PkeyDelete> key(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048U));
    if (key == nullptr) {
        throw std::runtime_error("RSA-2048 key generation failed");
    }
    const std::unique_ptr<BIO, BioDelete> bio(BIO_new_file(path.c_str(), "w"));
    if (bio == nullptr) {
        throw std::runtime_error("cannot write " + path.string());
    }
    if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
        1) {
        throw std::runtime_error("PEM_write_bio_PrivateKey failed");
    }
}

Stats bench_sign(const Options& options) {
    const TempDirectory directory(std::filesystem::temp_directory_path() /
                                  ("hotpath-bench-" + std::to_string(::getpid()) + "-key"));
    const std::filesystem::path key_path = directory.path() / "bench_key.pem";
    write_throwaway_key(key_path);

    hotpath::RequestSigner signer(key_path);
    if (const std::optional<std::string> failure = signer.load(); failure.has_value()) {
        throw std::runtime_error(*failure);
    }

    std::vector<double> samples;
    samples.reserve(options.warmup + options.iterations);
    for (std::size_t i = 0; i < options.warmup + options.iterations; ++i) {
        // Built outside the span, as `latency_bench.py::bench_sign` builds it.
        const std::string timestamp = std::to_string(hotpath::wall_clock_ms());
        const std::int64_t started = hotpath::monotonic_ns();
        const std::string signature =
            signer.sign(timestamp, "GET", "/trade-api/v2/portfolio/orders");
        samples.push_back(elapsed_ms(started));
        if (signature.empty()) {
            throw std::runtime_error("signer returned an empty signature");
        }
    }
    return Stats::from_samples(samples, options.warmup);
}

// cpp-to-cpp: this process's own executor on a thread, driven over a real Unix domain socket by
// the client in `poller_client.cpp`. Two spans come back from one run. `roundtrip` is write to ack
// as the client sees it, and `wake_recv` is decode plus ack as the server records it, which is the
// span `executor_server.py` records under that name and the only one of the two with a Python
// counterpart.
std::vector<Result> bench_roundtrip(const Options& options) {
    const std::filesystem::path socket_path =
        std::filesystem::temp_directory_path() /
        ("hotpath-bench-" + std::to_string(::getpid()) + ".sock");

    const std::size_t total = options.warmup + options.iterations;
    std::vector<double> wake_recv;
    wake_recv.reserve(total);
    std::uint64_t fired = 0;

    hotpath::ExecutorServer server(hotpath::ExecutorConfig{
        .socket_path = socket_path,
        .backlog = 4,
        .kill_switch = nullptr,
        // A counter rather than nothing, so the template fill and the dispatch hop stay in the
        // path being measured. The Python's fake REST client is the same idea.
        .dispatch = [&fired](const hotpath::FireRequest&) { ++fired; },
        .record_wake_recv =
            [&wake_recv](const hotpath::WakeRecvEvent& event) {
                wake_recv.push_back(event.duration_ms);
            },
    });

    if (const std::optional<std::string> failure = server.listen(); failure.has_value()) {
        throw std::runtime_error("listen: " + *failure);
    }

    // `wake_recv` and `fired` are touched only by the server thread, and read only past the join.
    std::thread worker([&server] { static_cast<void>(server.serve_one_connection()); });

    std::vector<double> roundtrip;
    try {
        hotpath::bench::PollerClient client(socket_path);
        if (const std::optional<std::string> failure = client.connect(std::chrono::seconds(5));
            failure.has_value()) {
            throw std::runtime_error("connect: " + *failure);
        }

        WakeMessage message = bench_wake();
        WakeAck ack;
        roundtrip.reserve(total);
        for (std::size_t i = 0; i < total; ++i) {
            message.correlation_id = "bench-" + std::to_string(i);
            message.sent_at_ms = hotpath::wall_clock_ms();
            message.sent_at_ns = hotpath::monotonic_ns();
            const std::int64_t started = hotpath::monotonic_ns();
            const std::optional<std::string> failure = client.send_wake(message, ack);
            roundtrip.push_back(elapsed_ms(started));
            if (failure.has_value()) {
                throw std::runtime_error("send_wake: " + *failure);
            }
            if (ack.status != hotpath::WakeAckStatus::Accepted) {
                throw std::runtime_error("executor rejected " + ack.correlation_id + ": " +
                                         ack.reason.value_or(""));
            }
        }
    } catch (...) {
        server.stop();
        worker.join();
        throw;
    }

    server.stop();
    worker.join();

    if (fired != total || wake_recv.size() != total) {
        throw std::runtime_error("executor handled " + std::to_string(fired) + " fires and " +
                                 std::to_string(wake_recv.size()) + " wakes, expected " +
                                 std::to_string(total));
    }

    return {
        Result{.name = "roundtrip", .stats = Stats::from_samples(roundtrip, options.warmup)},
        Result{.name = "wake_recv", .stats = Stats::from_samples(wake_recv, options.warmup)},
    };
}

void report(const Result& result) {
    static_cast<void>(
        std::printf("%-12s p50=%8.4fms  p90=%8.4fms  p99=%8.4fms  n=%-6zu warmup=%zu\n",
                    result.name.c_str(), result.stats.p50_ms, result.stats.p90_ms,
                    result.stats.p99_ms, result.stats.n, result.stats.warmup));
}

void write_csv(const std::filesystem::path& path, const std::vector<Result>& results) {
    const bool is_new = !std::filesystem::exists(path);
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("cannot append to " + path.string());
    }
    if (is_new) {
        out << "timestamp_utc,platform,benchmark,n,warmup,p50_ms,p90_ms,p99_ms\n";
    }

    const std::string timestamp = utc_now();
    const std::string platform = platform_tag();
    for (const Result& result : results) {
        out << timestamp << ',' << platform << ',' << result.name << ',' << result.stats.n << ','
            << result.stats.warmup << ',';
        std::array<char, 64> formatted{};
        static_cast<void>(std::snprintf(formatted.data(), formatted.size(), "%.4f,%.4f,%.4f",
                                        result.stats.p50_ms, result.stats.p90_ms,
                                        result.stats.p99_ms));
        out << formatted.data() << '\n';
    }
    if (!out.flush()) {
        throw std::runtime_error("write to " + path.string() + " failed");
    }
}

void usage() {
    static_cast<void>(
        std::fputs("usage: executor_hotpath_bench [--iterations N] [--warmup N] [--csv PATH]\n"
                   "                              [--only decode|encode_ack|sign|roundtrip]\n",
                   stderr));
}

std::optional<std::size_t> parse_count(const char* text) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

int run(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view flag(args[i]);
        const bool has_value = i + 1 < args.size();
        if (flag == "--iterations" && has_value) {
            const std::optional<std::size_t> value = parse_count(args[++i]);
            if (!value.has_value() || *value == 0) {
                usage();
                return 2;
            }
            options.iterations = *value;
        } else if (flag == "--warmup" && has_value) {
            const std::optional<std::size_t> value = parse_count(args[++i]);
            if (!value.has_value()) {
                usage();
                return 2;
            }
            options.warmup = *value;
        } else if (flag == "--csv" && has_value) {
            options.csv = args[++i];
        } else if (flag == "--only" && has_value) {
            options.only = args[++i];
        } else {
            usage();
            return 2;
        }
    }

    const bool selected_decode = options.only.empty() || options.only == "decode";
    const bool selected_encode = options.only.empty() || options.only == "encode_ack";
    const bool selected_sign = options.only.empty() || options.only == "sign";
    const bool selected_roundtrip = options.only.empty() || options.only == "roundtrip";
    if (!selected_decode && !selected_encode && !selected_sign && !selected_roundtrip) {
        usage();
        return 2;
    }

    static_cast<void>(std::printf("platform %s, %zu iterations, %zu warm-up discarded\n\n",
                                  platform_tag().c_str(), options.iterations, options.warmup));

    std::vector<Result> results;
    if (selected_decode) {
        results.push_back(Result{.name = "decode", .stats = bench_decode(options)});
    }
    if (selected_encode) {
        results.push_back(Result{.name = "encode_ack", .stats = bench_encode_ack(options)});
    }
    if (selected_sign) {
        results.push_back(Result{.name = "sign", .stats = bench_sign(options)});
    }
    if (selected_roundtrip) {
        for (Result& result : bench_roundtrip(options)) {
            results.push_back(std::move(result));
        }
    }

    for (const Result& result : results) {
        report(result);
    }

    if (!options.csv.empty()) {
        write_csv(options.csv, results);
        static_cast<void>(
            std::printf("\nappended %zu rows to %s\n", results.size(), options.csv.c_str()));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(std::span<char*>(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& failure) {
        static_cast<void>(std::fprintf(stderr, "executor-hotpath-bench: %s\n", failure.what()));
        return 1;
    }
}
