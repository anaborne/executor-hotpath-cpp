#include "hotpath/executor.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "hotpath/clock.hpp"
#include "hotpath/orders.hpp"
#include "hotpath/pricing.hpp"
#include "hotpath/protocol.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hotpath {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

enum class ReadResult : std::uint8_t { Ok, PeerClosed, Failed };

std::string syscall_error(const char* name) {
    return std::string(name) + ": " + std::generic_category().message(errno);
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

ReadResult read_exactly(int fd, std::byte* out, std::size_t count, const std::atomic<bool>& stop) {
    std::size_t filled = 0;
    while (filled < count) {
        const ssize_t got = ::read(fd, out + filled, count - filled);
        if (got == 0) {
            return filled == 0 ? ReadResult::PeerClosed : ReadResult::Failed;
        }
        if (got < 0) {
            if (errno == EINTR && !stop.load(std::memory_order_relaxed)) {
                continue;
            }
            return ReadResult::Failed;
        }
        filled += static_cast<std::size_t>(got);
    }
    return ReadResult::Ok;
}

}  // namespace

ExecutorServer::ExecutorServer(ExecutorConfig config) : config_(std::move(config)) {
    ack_.reserve(256);
}

ExecutorServer::~ExecutorServer() {
    close_fd(listen_fd_);
    close_fd(wake_read_fd_);
    close_fd(wake_write_fd_);
    std::error_code ec;
    std::filesystem::remove(config_.socket_path, ec);
}

std::optional<std::string> ExecutorServer::listen() {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = config_.socket_path.string();
    if (path.size() >= sizeof(address.sun_path)) {
        return "socket path exceeds " + std::to_string(sizeof(address.sun_path) - 1) + " bytes";
    }
    std::memcpy(&address.sun_path[0], path.data(), path.size());

    std::error_code ec;
    std::filesystem::create_directories(config_.socket_path.parent_path(), ec);
    std::filesystem::remove(config_.socket_path, ec);

    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) {
        return syscall_error("pipe");
    }
    wake_read_fd_ = pipe_fds[0];
    wake_write_fd_ = pipe_fds[1];

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return syscall_error("socket");
    }
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return syscall_error("bind");
    }
    // Anything that can connect to this socket can make the executor place an order. asyncio binds
    // it with the process umask, typically world-connectable, and the Python narrows it here too.
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return syscall_error("chmod");
    }
    if (::listen(listen_fd_, config_.backlog) != 0) {
        return syscall_error("listen");
    }
    return std::nullopt;
}

void ExecutorServer::stop() noexcept {
    stopping_.store(true, std::memory_order_relaxed);
    if (wake_write_fd_ >= 0) {
        const char byte = 'x';
        const ssize_t written = ::write(wake_write_fd_, &byte, 1);
        static_cast<void>(written);
    }
    // The pipe wakes `poll`, not a `read` sitting on the connection. A signal interrupts that
    // read only if it lands while the read is in progress; one that lands between the flag check
    // at the top of the loop and the read entering would leave the loop blocked until the peer
    // spoke again. Shutting the socket down makes the read return whenever the signal landed.
    const int fd = connection_fd_.load(std::memory_order_relaxed);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

bool ExecutorServer::serve_one_connection() {
    if (stopping_.load(std::memory_order_relaxed) || listen_fd_ < 0) {
        return false;
    }

    std::array<pollfd, 2> waiting{pollfd{.fd = listen_fd_, .events = POLLIN, .revents = 0},
                                  pollfd{.fd = wake_read_fd_, .events = POLLIN, .revents = 0}};
    while (true) {
        if (::poll(waiting.data(), waiting.size(), -1) < 0) {
            if (errno == EINTR && !stopping_.load(std::memory_order_relaxed)) {
                continue;
            }
            return false;
        }
        if ((waiting[1].revents & POLLIN) != 0 || stopping_.load(std::memory_order_relaxed)) {
            return false;
        }
        if ((waiting[0].revents & POLLIN) != 0) {
            break;
        }
    }

    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        return !stopping_.load(std::memory_order_relaxed) && errno == EINTR;
    }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    connection_fd_.store(fd, std::memory_order_relaxed);
    serve_connection(fd);
    connection_fd_.store(-1, std::memory_order_relaxed);
    ::close(fd);
    return !stopping_.load(std::memory_order_relaxed);
}

void ExecutorServer::serve_forever() {
    while (serve_one_connection()) {
    }
}

void ExecutorServer::serve_connection(int fd) {
    std::array<std::byte, kFrameLengthBytes> prefix{};

    while (!stopping_.load(std::memory_order_relaxed)) {
        if (read_exactly(fd, prefix.data(), prefix.size(), stopping_) != ReadResult::Ok) {
            return;
        }

        const Decoded<std::uint32_t> length = decode_frame_length(prefix);
        if (!length) {
            // A length this process will not read leaves the stream at an unknown offset, so the
            // connection cannot be resynchronized and is dropped instead.
            return;
        }

        body_.resize(length.value());
        if (read_exactly(fd, body_.data(), body_.size(), stopping_) != ReadResult::Ok) {
            return;
        }
        ++stats_.frames_read;

        // Stamped after the read returns, not before it. The read blocks until a frame arrives, so
        // a stamp taken ahead of it measures the gap between wakes rather than the cost of one. In
        // the Python that mistake put `wake_recv` at 1181 ms against a sub-millisecond budget,
        // tracking wake inter-arrival time, and the burst-shaped benchmark never showed it.
        const std::int64_t started_at_ms = wall_clock_ms();
        const std::int64_t started_ns = monotonic_ns();

        const Decoded<WakeMessage> decoded = decode_wake_message(body_);
        if (!decoded) {
            encode_frame_into(WakeAck{.schema_version = kSchemaVersion,
                                      .correlation_id = "",
                                      .received_at_ms = started_at_ms,
                                      .status = WakeAckStatus::Rejected,
                                      .reason = decoded.error().message},
                              ack_);
            ++stats_.acks_rejected;
            if (!send_frame(fd, ack_)) {
                return;
            }
            continue;
        }

        const WakeMessage& message = decoded.value();
        encode_frame_into(WakeAck{.schema_version = kSchemaVersion,
                                  .correlation_id = message.correlation_id,
                                  .received_at_ms = started_at_ms,
                                  .status = WakeAckStatus::Accepted,
                                  .reason = std::nullopt},
                          ack_);
        ++stats_.acks_accepted;
        if (!send_frame(fd, ack_)) {
            return;
        }

        const std::int64_t ended_ns = monotonic_ns();
        if (config_.record_wake_recv) {
            config_.record_wake_recv(WakeRecvEvent{
                .correlation_id = message.correlation_id,
                .started_at_ms = started_at_ms,
                .ended_at_ms = wall_clock_ms(),
                .duration_ms = static_cast<double>(ended_ns - started_ns) / 1'000'000.0,
                .dry_run = message.dry_run,
            });
        }

        static_cast<void>(handle_fire(message));
    }
}

bool ExecutorServer::send_frame(int fd, const std::vector<std::byte>& frame) {
    std::size_t sent = 0;
    while (sent < frame.size()) {
        const ssize_t wrote = ::send(fd, frame.data() + sent, frame.size() - sent, kSendFlags);
        if (wrote < 0) {
            if (errno == EINTR && !stopping_.load(std::memory_order_relaxed)) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

FireOutcome ExecutorServer::handle_fire(const WakeMessage& message) {
    // The order goes out at the YES-side wire price and never at `kalshi_price`, which is the
    // poller's edge-math cost and is carried for logging. A real fire without a tradeable one is
    // refused: 0.0 is both the pre-v3 default and what an empty book quotes, so there is nothing
    // correct to send. Shadow fires pass, since refusing them would thin the detect_fire sample to
    // well-quoted books only.
    if (!message.dry_run &&
        (message.wire_price_yes_dollars <= 0.0 || message.wire_price_yes_dollars >= 1.0)) {
        ++stats_.fires_refused_price;
        return FireOutcome::RefusedUntradeablePrice;
    }

    const double wire_price = snap_to_grid(message.wire_price_yes_dollars, message.price_ranges);

    // After the price guard and before anything is spent on the fire. Real fires only: a shadow
    // fire places no order, and a halted bot keeps measuring.
    if (!message.dry_run && config_.kill_switch != nullptr && config_.kill_switch->is_engaged()) {
        ++stats_.fires_refused_kill_switch;
        return FireOutcome::RefusedKillSwitch;
    }

    const OrderTemplate& order = templates_.acquire(message.market_ticker, message.direction);
    ++stats_.fires_dispatched;
    if (config_.dispatch) {
        config_.dispatch(FireRequest{
            .order = &order,
            .price_dollars = wire_price,
            .exchange_index = message.exchange_index,
            .correlation_id = message.correlation_id,
            .correlation_group = message.correlation_group,
            .dry_run = message.dry_run,
        });
    }
    return FireOutcome::Dispatched;
}

}  // namespace hotpath
