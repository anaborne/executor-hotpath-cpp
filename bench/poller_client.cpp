#include "poller_client.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "hotpath/protocol.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace hotpath::bench {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A benchmark that hangs reports nothing. An executor that has not answered in this long is a
// failure to name, not a slow sample to wait for.
constexpr timeval kReadTimeout{.tv_sec = 10, .tv_usec = 0};

std::string syscall_error(const char* name) {
    return std::string(name) + ": " + std::generic_category().message(errno);
}

}  // namespace

PollerClient::PollerClient(std::filesystem::path socket_path)
    : socket_path_(std::move(socket_path)) {
    frame_.reserve(1024);
    body_.reserve(1024);
}

PollerClient::~PollerClient() {
    close();
}

std::optional<std::string> PollerClient::connect(std::chrono::milliseconds timeout) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path_.string();
    if (path.size() >= sizeof(address.sun_path)) {
        return "socket path exceeds " + std::to_string(sizeof(address.sun_path) - 1) + " bytes";
    }
    std::memcpy(&address.sun_path[0], path.data(), path.size());

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return syscall_error("socket");
        }
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            break;
        }
        std::string failure = syscall_error("connect");
        close();
        if (std::chrono::steady_clock::now() >= deadline) {
            return failure;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &kReadTimeout, sizeof(kReadTimeout)) != 0) {
        return syscall_error("setsockopt SO_RCVTIMEO");
    }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return std::nullopt;
}

std::optional<std::string> PollerClient::send_wake(const WakeMessage& message, WakeAck& ack) {
    if (fd_ < 0) {
        return "not connected";
    }

    encode_frame_into(message, frame_);
    std::size_t sent = 0;
    while (sent < frame_.size()) {
        const ssize_t wrote = ::send(fd_, frame_.data() + sent, frame_.size() - sent, kSendFlags);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return syscall_error("send");
        }
        sent += static_cast<std::size_t>(wrote);
    }

    std::array<std::byte, kFrameLengthBytes> prefix{};
    std::size_t filled = 0;
    while (filled < prefix.size()) {
        const ssize_t got = ::read(fd_, prefix.data() + filled, prefix.size() - filled);
        if (got <= 0) {
            return got == 0 ? "executor closed the connection" : syscall_error("read");
        }
        filled += static_cast<std::size_t>(got);
    }

    const Decoded<std::uint32_t> length = decode_frame_length(prefix);
    if (!length) {
        return "ack frame length: " + length.error().message;
    }

    body_.resize(length.value());
    filled = 0;
    while (filled < body_.size()) {
        const ssize_t got = ::read(fd_, body_.data() + filled, body_.size() - filled);
        if (got <= 0) {
            return got == 0 ? "executor closed the connection" : syscall_error("read");
        }
        filled += static_cast<std::size_t>(got);
    }

    const Decoded<WakeAck> decoded = decode_wake_ack(body_);
    if (!decoded) {
        return "ack body: " + decoded.error().message;
    }
    ack = decoded.value();
    return std::nullopt;
}

void PollerClient::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace hotpath::bench
