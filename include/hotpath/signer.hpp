#pragma once

// Ported from `auth/signer.py`. RSA-PSS over `timestamp + method + path` concatenated with no
// separator, MGF1 and the digest both SHA-256, salt length equal to the digest length, base64 out.
// That module's docstring is the specification for the message and this reproduces it; the path is
// the request path alone, no scheme, no host, no query string.
//
// PSS salts randomly, so one message signed twice with one key gives two different valid
// signatures and there is no golden signature to compare bytes against. `tests/golden/signing`
// holds signatures the Python produced and the public key that verifies them, which pins the
// message, the hash, the MGF and the salt length as tightly as a fixed vector would.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hotpath {

// One signer per thread. The message and the raw signature are assembled into member buffers, so
// every call after the first allocates only the base64 it returns.
class RequestSigner {
public:
    // The WebSocket handshake signs this fixed pair rather than anything a caller passes in, and
    // it keeps its own entry point for the reason `signer.py` gives: nobody can hand `sign` a REST
    // path by accident and authenticate a socket with it.
    static constexpr std::string_view kWebSocketAuthMethod = "GET";
    static constexpr std::string_view kWebSocketAuthPath = "/trade-api/ws/v2";

    explicit RequestSigner(std::filesystem::path private_key_path);
    RequestSigner(const RequestSigner&) = delete;
    RequestSigner& operator=(const RequestSigner&) = delete;
    RequestSigner(RequestSigner&&) = delete;
    RequestSigner& operator=(RequestSigner&&) = delete;
    ~RequestSigner();

    // Reads the PEM and refuses anything that is not an RSA private key, where
    // `KalshiRequestSigner.__init__` raises TypeError. Names the failure, or nothing.
    [[nodiscard]] std::optional<std::string> load();

    // Base64 of the RSA-PSS signature over `timestamp + method + path`.
    //
    // A key that will not load is an operator error and comes back from `load` as a message, the
    // same shape `ExecutorServer::listen` uses. A signature that will not compute from a key that
    // already loaded is a different animal: past a successful init the call fails on allocation or
    // on the RNG, there is no caller-side recovery to write, and threading an optional through the
    // benchmark for it would buy a branch nobody takes. It throws, where `_sign_message` raises.
    [[nodiscard]] std::string sign(std::string_view timestamp, std::string_view method,
                                   std::string_view path);

    [[nodiscard]] std::string sign_websocket_auth(std::string_view timestamp);

    [[nodiscard]] const std::filesystem::path& private_key_path() const noexcept {
        return private_key_path_;
    }

private:
    struct Key;

    std::string sign_message(std::string_view message);

    std::filesystem::path private_key_path_;
    std::unique_ptr<Key> key_;
    std::string message_;
    std::vector<unsigned char> signature_;
};

}  // namespace hotpath
