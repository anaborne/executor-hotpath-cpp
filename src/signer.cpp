#include "hotpath/signer.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace hotpath {
namespace {

std::string openssl_error() {
    std::string message;
    for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
        std::array<char, 256> text{};
        ERR_error_string_n(code, text.data(), text.size());
        if (!message.empty()) {
            message += "; ";
        }
        message += text.data();
    }
    return message.empty() ? std::string("no OpenSSL error recorded") : message;
}

// Refusing rather than prompting. The default callback reads a passphrase from the terminal, and
// an executor started by launchd with an encrypted key would hang there instead of failing.
extern "C" int refuse_passphrase(char* /*buffer*/, int /*size*/, int /*rwflag*/, void* /*user*/) {
    return -1;
}

// EVP_EncodeBlock rather than the base64 BIO, which wraps at 64 characters where
// `base64.b64encode` does not. It writes a terminator past the length it returns, so the buffer is
// one byte longer than the encoding needs.
std::string base64(const std::vector<unsigned char>& bytes) {
    std::string encoded((4 * ((bytes.size() + 2) / 3)) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                        bytes.data(), static_cast<int>(bytes.size()));
    if (written < 0) {
        throw std::runtime_error("EVP_EncodeBlock: " + openssl_error());
    }
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

}  // namespace

struct RequestSigner::Key {
    EVP_PKEY* pkey = nullptr;
    EVP_MD_CTX* ctx = nullptr;

    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    Key(Key&&) = delete;
    Key& operator=(Key&&) = delete;

    ~Key() {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
    }
};

RequestSigner::RequestSigner(std::filesystem::path private_key_path)
    : private_key_path_(std::move(private_key_path)) {}

RequestSigner::~RequestSigner() = default;

std::optional<std::string> RequestSigner::load() {
    ERR_clear_error();

    BIO* bio = BIO_new_file(private_key_path_.c_str(), "r");
    if (bio == nullptr) {
        return "cannot read " + private_key_path_.string() + ": " + openssl_error();
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, refuse_passphrase, nullptr);
    BIO_free(bio);
    if (pkey == nullptr) {
        return private_key_path_.string() +
               " is not an unencrypted PEM private key: " + openssl_error();
    }

    if (EVP_PKEY_is_a(pkey, "RSA") != 1) {
        EVP_PKEY_free(pkey);
        return private_key_path_.string() + " does not contain an RSA private key";
    }

    auto key = std::make_unique<Key>();
    key->pkey = pkey;
    key->ctx = EVP_MD_CTX_new();
    if (key->ctx == nullptr) {
        return "EVP_MD_CTX_new: " + openssl_error();
    }
    key_ = std::move(key);
    return std::nullopt;
}

std::string RequestSigner::sign(std::string_view timestamp, std::string_view method,
                                std::string_view path) {
    message_.assign(timestamp);
    message_.append(method);
    message_.append(path);
    return sign_message(message_);
}

std::string RequestSigner::sign_websocket_auth(std::string_view timestamp) {
    return sign(timestamp, kWebSocketAuthMethod, kWebSocketAuthPath);
}

std::string RequestSigner::sign_message(std::string_view message) {
    if (!key_) {
        throw std::logic_error("RequestSigner::sign before a successful load()");
    }
    ERR_clear_error();

    EVP_PKEY_CTX* pctx = nullptr;
    if (EVP_DigestSignInit(key_->ctx, &pctx, EVP_sha256(), nullptr, key_->pkey) != 1) {
        throw std::runtime_error("EVP_DigestSignInit: " + openssl_error());
    }

    // OpenSSL's salt default under PSS is the widest the modulus allows, 222 bytes for RSA-2048
    // with SHA-256, and a signature carrying one is rejected by any verifier pinned to the digest
    // width. `padding.PSS(salt_length=PSS.DIGEST_LENGTH)` is 32.
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) != 1) {
        throw std::runtime_error("PSS parameters: " + openssl_error());
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(message.data());
    std::size_t length = 0;
    if (EVP_DigestSign(key_->ctx, nullptr, &length, bytes, message.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign sizing: " + openssl_error());
    }
    signature_.resize(length);
    if (EVP_DigestSign(key_->ctx, signature_.data(), &length, bytes, message.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign: " + openssl_error());
    }
    signature_.resize(length);
    return base64(signature_);
}

}  // namespace hotpath
