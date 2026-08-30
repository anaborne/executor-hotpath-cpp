// The cross-language check for the signer. PSS salts randomly, so there is no signature to compare
// bytes against and the fixture cannot be an expected value; it is the public key and a set of
// signatures the real `auth/signer.py` produced, and what this asserts is that they verify here
// under the published parameters over the message this port builds.
//
// The verifier below is written straight against OpenSSL rather than calling anything in
// `src/signer.cpp`. A shared helper would let a wrong salt length or a wrong MGF agree with itself
// and pass.
//
// The other direction cannot run from here, so this writes what it signed to
// HOTPATH_SIGNER_CROSS_CHECK_DIR and `generate_signing_fixture.py --verify-cpp` reads it back
// through `cryptography`'s verifier.

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hotpath/signer.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <catch2/catch_test_macros.hpp>

namespace {

struct PkeyDelete {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct MdCtxDelete {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

struct BioDelete {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDelete>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxDelete>;
using Bio = std::unique_ptr<BIO, BioDelete>;

std::filesystem::path signing_dir() {
    return std::filesystem::path(HOTPATH_GOLDEN_DIR) / "signing";
}

Pkey generate_key(const char* type, unsigned int rsa_bits, const char* curve) {
    EVP_PKEY* raw = curve != nullptr ? EVP_PKEY_Q_keygen(nullptr, nullptr, type, curve)
                                     : EVP_PKEY_Q_keygen(nullptr, nullptr, type, rsa_bits);
    REQUIRE(raw != nullptr);
    return Pkey(raw);
}

void write_private_pem(EVP_PKEY* key, const std::filesystem::path& path) {
    Bio bio(BIO_new_file(path.c_str(), "w"));
    REQUIRE(bio != nullptr);
    REQUIRE(PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
}

void write_public_pem(EVP_PKEY* key, const std::filesystem::path& path) {
    Bio bio(BIO_new_file(path.c_str(), "w"));
    REQUIRE(bio != nullptr);
    REQUIRE(PEM_write_bio_PUBKEY(bio.get(), key) == 1);
}

Pkey read_public_pem(const std::filesystem::path& path) {
    Bio bio(BIO_new_file(path.c_str(), "r"));
    REQUIRE(bio != nullptr);
    Pkey key(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
    REQUIRE(key != nullptr);
    return key;
}

// EVP_DecodeBlock rounds up to the block and reports the padding bytes as data, so the '=' at the
// end have to come off by hand or every signature is three bytes too long and verifies against
// nothing.
std::vector<unsigned char> base64_decode(std::string_view encoded) {
    REQUIRE(encoded.size() % 4 == 0);
    std::vector<unsigned char> decoded((encoded.size() / 4) * 3);
    const int written =
        EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(encoded.data()),
                        static_cast<int>(encoded.size()));
    REQUIRE(written > 0);
    auto length = static_cast<std::size_t>(written);
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.remove_suffix(1);
        --length;
    }
    decoded.resize(length);
    return decoded;
}

// SHA-256, MGF1-SHA256, and a salt the caller names, so a test can ask whether a signature carrying
// the wrong salt length is caught rather than assuming it would be.
bool verifies(EVP_PKEY* key, std::string_view message, const std::vector<unsigned char>& signature,
              int salt_length) {
    MdCtx ctx(EVP_MD_CTX_new());
    REQUIRE(ctx != nullptr);
    EVP_PKEY_CTX* pctx = nullptr;
    REQUIRE(EVP_DigestVerifyInit(ctx.get(), &pctx, EVP_sha256(), nullptr, key) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt_length) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) == 1);
    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1;
}

bool verifies(EVP_PKEY* key, std::string_view message, const std::string& signature_b64) {
    return verifies(key, message, base64_decode(signature_b64), RSA_PSS_SALTLEN_DIGEST);
}

std::vector<unsigned char> sign_with_salt(EVP_PKEY* key, std::string_view message,
                                          int salt_length) {
    MdCtx ctx(EVP_MD_CTX_new());
    REQUIRE(ctx != nullptr);
    EVP_PKEY_CTX* pctx = nullptr;
    REQUIRE(EVP_DigestSignInit(ctx.get(), &pctx, EVP_sha256(), nullptr, key) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, salt_length) == 1);
    REQUIRE(EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) == 1);
    const auto* bytes = reinterpret_cast<const unsigned char*>(message.data());
    std::size_t length = 0;
    REQUIRE(EVP_DigestSign(ctx.get(), nullptr, &length, bytes, message.size()) == 1);
    std::vector<unsigned char> signature(length);
    REQUIRE(EVP_DigestSign(ctx.get(), signature.data(), &length, bytes, message.size()) == 1);
    signature.resize(length);
    return signature;
}

// Returning the message rather than the optional. clang-tidy's optional dataflow does not read
// Catch2's REQUIRE as a terminator, so a REQUIRE-then-dereference is an unchecked access to it.
std::string load_failure(hotpath::RequestSigner& signer) {
    return signer.load().value_or("<the key loaded without error>");
}

struct Vector {
    std::string timestamp;
    std::string method;
    std::string path;
    std::string signature;

    [[nodiscard]] std::string message() const { return timestamp + method + path; }
};

std::vector<Vector> read_vectors() {
    std::ifstream file(signing_dir() / "vectors.tsv");
    REQUIRE(file.is_open());
    std::vector<Vector> vectors;
    std::string line;
    while (std::getline(file, line)) {
        Vector row;
        std::size_t start = 0;
        const std::array<std::string*, 4> fields = {&row.timestamp, &row.method, &row.path,
                                                    &row.signature};
        for (std::string* field : fields) {
            const std::size_t tab = line.find('\t', start);
            *field = line.substr(start, tab - start);
            start = tab == std::string::npos ? line.size() : tab + 1;
        }
        vectors.push_back(std::move(row));
    }
    REQUIRE(vectors.size() >= 5);
    return vectors;
}

// A directory this test owns, cleared each run so a stale file cannot be re-verified as a fresh
// one, and named by the build tree so two presets do not overwrite each other.
class TempKeyDir {
public:
    TempKeyDir() : path_(std::filesystem::temp_directory_path() / "hotpath_signer_test") {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    TempKeyDir(const TempKeyDir&) = delete;
    TempKeyDir& operator=(const TempKeyDir&) = delete;
    TempKeyDir(TempKeyDir&&) = delete;
    TempKeyDir& operator=(TempKeyDir&&) = delete;

    ~TempKeyDir() { std::filesystem::remove_all(path_); }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("signatures from the Python verify against the message this port builds") {
    const Pkey public_key = read_public_pem(signing_dir() / "public_key.pem");

    for (const Vector& row : read_vectors()) {
        INFO(row.message());
        CHECK(verifies(public_key.get(), row.message(), row.signature));
    }
}

TEST_CASE("the Python's signatures are rejected under any other message") {
    const Pkey public_key = read_public_pem(signing_dir() / "public_key.pem");
    const std::vector<Vector> vectors = read_vectors();
    const Vector& row = vectors.front();

    CHECK_FALSE(verifies(public_key.get(), row.method + row.timestamp + row.path, row.signature));
    CHECK_FALSE(verifies(public_key.get(), row.timestamp + row.path + row.method, row.signature));
    CHECK_FALSE(verifies(public_key.get(), row.timestamp + " " + row.method + " " + row.path,
                         row.signature));
    CHECK_FALSE(verifies(public_key.get(), row.timestamp + row.method + row.path + "?limit=1",
                         row.signature));
}

TEST_CASE("this signer's output verifies, and the same message twice does not repeat") {
    const TempKeyDir keys;
    const Pkey key = generate_key("RSA", 2048, nullptr);
    write_private_pem(key.get(), keys.file("rsa.pem"));

    hotpath::RequestSigner signer(keys.file("rsa.pem"));
    REQUIRE_FALSE(signer.load().has_value());

    const std::string first = signer.sign("1703123456789", "GET", "/trade-api/v2/portfolio/orders");
    const std::string second =
        signer.sign("1703123456789", "GET", "/trade-api/v2/portfolio/orders");

    CHECK(verifies(key.get(), "1703123456789GET/trade-api/v2/portfolio/orders", first));
    CHECK(verifies(key.get(), "1703123456789GET/trade-api/v2/portfolio/orders", second));
    CHECK(first != second);
}

TEST_CASE("a tampered request does not verify against the signature of the real one") {
    const TempKeyDir keys;
    const Pkey key = generate_key("RSA", 2048, nullptr);
    write_private_pem(key.get(), keys.file("rsa.pem"));

    hotpath::RequestSigner signer(keys.file("rsa.pem"));
    REQUIRE_FALSE(signer.load().has_value());
    const std::string signature =
        signer.sign("1703123456789", "GET", "/trade-api/v2/portfolio/orders");

    CHECK_FALSE(verifies(key.get(), "1703123456789POST/trade-api/v2/portfolio/orders", signature));
    CHECK_FALSE(verifies(key.get(), "1703123456789GET/trade-api/v2/portfolio/balance", signature));
    CHECK_FALSE(verifies(key.get(), "1703123456790GET/trade-api/v2/portfolio/orders", signature));
}

TEST_CASE("the WebSocket handshake signs its own fixed message") {
    const TempKeyDir keys;
    const Pkey key = generate_key("RSA", 2048, nullptr);
    write_private_pem(key.get(), keys.file("rsa.pem"));

    hotpath::RequestSigner signer(keys.file("rsa.pem"));
    REQUIRE_FALSE(signer.load().has_value());
    const std::string signature = signer.sign_websocket_auth("1703123456789");

    CHECK(verifies(key.get(), "1703123456789GET/trade-api/ws/v2", signature));
    CHECK_FALSE(verifies(key.get(), "1703123456789GET/trade-api/v2/portfolio/orders", signature));
}

// The test that keeps the one above from being vacuous. OpenSSL's own default salt is the widest
// the modulus allows, so if the verifier accepted any salt length every assertion here would pass
// against a signer that never set one.
TEST_CASE("a salt wider than the digest is refused") {
    const Pkey key = generate_key("RSA", 2048, nullptr);
    const std::string message = "1703123456789GET/trade-api/v2/portfolio/orders";

    const std::vector<unsigned char> widest =
        sign_with_salt(key.get(), message, RSA_PSS_SALTLEN_MAX);
    const std::vector<unsigned char> digest_width =
        sign_with_salt(key.get(), message, RSA_PSS_SALTLEN_DIGEST);

    CHECK_FALSE(verifies(key.get(), message, widest, RSA_PSS_SALTLEN_DIGEST));
    CHECK(verifies(key.get(), message, digest_width, RSA_PSS_SALTLEN_DIGEST));
}

TEST_CASE("a key that is not RSA is refused at load") {
    const TempKeyDir keys;
    const Pkey key = generate_key("EC", 0, "P-256");
    write_private_pem(key.get(), keys.file("ec.pem"));

    hotpath::RequestSigner signer(keys.file("ec.pem"));
    const std::string failure = load_failure(signer);

    INFO(failure);
    CHECK(failure.find("does not contain an RSA private key") != std::string::npos);
}

TEST_CASE("a missing or unreadable key is named rather than thrown") {
    const TempKeyDir keys;
    {
        hotpath::RequestSigner signer(keys.file("absent.pem"));
        const std::string failure = load_failure(signer);
        INFO(failure);
        CHECK(failure.find("cannot read") != std::string::npos);
    }
    {
        std::ofstream(keys.file("garbage.pem")) << "-----BEGIN PRIVATE KEY-----\nnope\n";
        hotpath::RequestSigner signer(keys.file("garbage.pem"));
        const std::string failure = load_failure(signer);
        INFO(failure);
        CHECK(failure.find("not an unencrypted PEM private key") != std::string::npos);
    }
}

// Not an assertion. This writes the C++ half of the cross-check for
// `generate_signing_fixture.py --verify-cpp`, which is run by hand because the value is different
// every run and gating it would put `cryptography` on all three CI runners.
TEST_CASE("the signatures this port produces are written out for the Python to verify") {
    const TempKeyDir keys;
    const Pkey key = generate_key("RSA", 2048, nullptr);
    write_private_pem(key.get(), keys.file("rsa.pem"));

    hotpath::RequestSigner signer(keys.file("rsa.pem"));
    REQUIRE_FALSE(signer.load().has_value());

    const std::filesystem::path out(HOTPATH_SIGNER_CROSS_CHECK_DIR);
    std::filesystem::create_directories(out);
    write_public_pem(key.get(), out / "cpp_public_key.pem");

    std::ofstream signatures(out / "cpp_signatures.tsv");
    REQUIRE(signatures.is_open());
    for (const Vector& row : read_vectors()) {
        const std::string signature = row.path == hotpath::RequestSigner::kWebSocketAuthPath
                                          ? signer.sign_websocket_auth(row.timestamp)
                                          : signer.sign(row.timestamp, row.method, row.path);
        CHECK(verifies(key.get(), row.message(), signature));
        signatures << row.message() << '\t' << signature << '\n';
    }
}
