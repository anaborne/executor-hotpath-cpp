// Decoder behaviour, written against what orjson and `cls(**data)` actually do rather than against
// what a JSON parser is usually permissive about. Every rejection here was observed in Python
// first; the six places this decoder is stricter than the Python are marked and are the same six
// listed in PORT-FIDELITY.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hotpath/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

hotpath::Decoded<hotpath::WakeMessage> decode(std::string_view body) {
    return hotpath::decode_wake_message(bytes_of(body));
}

// Every required field, as the shortest body the decoder will accept. Tests that care about one
// field append it, which keeps the interesting part of each case one line long.
constexpr std::string_view kMinimalFields =
    R"("schema_version":4,"correlation_id":"c","market_ticker":"T","asset":"BTC",)"
    R"("direction":"yes","kalshi_price":0.5,"model_probability":0.6,"fee":0.01,"edge":0.09,)"
    R"("decision_ts_ms":1,"sent_at_ms":2,"sent_at_ns":3)";

std::string minimal_body(std::string_view extra_fields = {}) {
    std::string body = "{";
    body += kMinimalFields;
    if (!extra_fields.empty()) {
        body += ",";
        body += extra_fields;
    }
    body += "}";
    return body;
}

}  // namespace

TEST_CASE("the minimal body decodes and every optional field takes its default", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(minimal_body());

    REQUIRE(decoded.has_value());
    CHECK(decoded.value().direction == hotpath::Direction::Yes);
    CHECK(decoded.value().exchange_index == -1);
    CHECK(decoded.value().price_ranges.size() == 1);
}

TEST_CASE("an unexpected field is refused, so a version bump changes what decodes", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(minimal_body(R"("v5_field":1)"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::UnknownField);
    CHECK(decoded.error().message.find("v5_field") != std::string::npos);
}

TEST_CASE("a missing required field is named in the error", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(
        R"({"schema_version":4,"correlation_id":"c","market_ticker":"T",)"
        R"("direction":"yes","kalshi_price":0.5,"model_probability":0.6,"fee":0.01,"edge":0.09,)"
        R"("decision_ts_ms":1,"sent_at_ms":2,"sent_at_ns":3})");

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::MissingField);
    CHECK(decoded.error().message.find("asset") != std::string::npos);
}

TEST_CASE("the last of two duplicate keys wins, as orjson does", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        decode(R"({"asset":"ETH",)" + std::string(kMinimalFields) + "}");

    REQUIRE(decoded.has_value());
    CHECK(decoded.value().asset == "BTC");
}

TEST_CASE("a direction outside the literal is refused", "[protocol]") {
    // Stricter than Python, which does not validate a Literal at runtime and would carry "maybe"
    // into the executor.
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(
        R"({"schema_version":4,"correlation_id":"c","market_ticker":"T","asset":"BTC",)"
        R"("direction":"maybe","kalshi_price":0.5,"model_probability":0.6,"fee":0.01,"edge":0.09,)"
        R"("decision_ts_ms":1,"sent_at_ms":2,"sent_at_ns":3})");

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::UnknownEnumValue);
}

TEST_CASE("an integer field holding a fraction is refused", "[protocol]") {
    // Stricter than Python, where a dataclass performs no runtime type check at all.
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(minimal_body(R"("recv_ns":1.5)"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::WrongType);
}

TEST_CASE("a string field holding a number is refused", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        decode(minimal_body(R"("clock_domain":7)"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::WrongType);
}

TEST_CASE("an integer past int64 is refused", "[protocol]") {
    // Stricter than Python, whose ints are unbounded. 2^63 is the first value that cannot be an
    // int64, and a timestamp field is the only place one could plausibly arrive.
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        decode(minimal_body(R"("recv_ns":9223372036854775808)"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::OutOfRange);
}

TEST_CASE("a double that overflows is refused, matching orjson", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(minimal_body(R"("fee":1e400)"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::OutOfRange);
}

TEST_CASE("a double that underflows is refused, where orjson returns zero", "[protocol]") {
    // The one divergence that rejects a frame orjson accepts. Recorded in PORT-FIDELITY.md: no
    // price, probability, fee, or contract count can reach 1e-400, and distinguishing underflow
    // from overflow costs a second parse of every number that reaches this branch.
    const hotpath::Decoded<hotpath::WakeMessage> decoded = decode(minimal_body(R"("fee":1e-400)"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::OutOfRange);
}

TEST_CASE("the number grammar is JSON's and not strtod's", "[protocol]") {
    CHECK_FALSE(decode(minimal_body(R"("recv_ns":01)")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("fee":.5)")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("fee":1.)")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("fee":NaN)")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("fee":Infinity)")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("fee":1e)")).has_value());
    CHECK(decode(minimal_body(R"("fee":1e+3)")).has_value());
    CHECK(decode(minimal_body(R"("fee":-0.0)")).has_value());
}

TEST_CASE("content after the document is refused", "[protocol]") {
    CHECK_FALSE(decode(minimal_body() + "x").has_value());
    CHECK(decode(minimal_body() + "  \n").has_value());
}

TEST_CASE("an empty body is refused rather than treated as an empty object", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        hotpath::decode_wake_message(std::span<const std::byte>{});

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::MalformedJson);
}

TEST_CASE("a raw control character inside a string is refused", "[protocol]") {
    CHECK_FALSE(decode(minimal_body("\"clock_domain\":\"a\tb\"")).has_value());
    CHECK(decode(minimal_body(R"("clock_domain":"a\tb")")).has_value());
}

TEST_CASE("escapes decode to the bytes they name", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        decode(minimal_body(R"("clock_domain":"\"\\\/\b\f\n\r\tAé😀")"));

    REQUIRE(decoded.has_value());
    CHECK(decoded.value().clock_domain == "\"\\/\b\f\n\r\tA\xc3\xa9\xf0\x9f\x98\x80");
}

TEST_CASE("an unpaired surrogate escape is refused", "[protocol]") {
    CHECK_FALSE(decode(minimal_body(R"("clock_domain":"\ud800")")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("clock_domain":"\ud800x")")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("clock_domain":"\udc00")")).has_value());
    CHECK(decode(minimal_body(R"("clock_domain":"😀")")).has_value());
}

TEST_CASE("invalid UTF-8 is refused, including the encodings a decoder might survive",
          "[protocol]") {
    CHECK_FALSE(decode(minimal_body("\"asset\":\"\xff\"")).has_value());
    CHECK_FALSE(decode(minimal_body("\"asset\":\"\xc0\xaf\"")).has_value());          // overlong /
    CHECK_FALSE(decode(minimal_body("\"asset\":\"\xed\xa0\x80\"")).has_value());      // surrogate
    CHECK_FALSE(decode(minimal_body("\"asset\":\"\xf5\x80\x80\x80\"")).has_value());  // > U+10FFFF
    CHECK_FALSE(decode(minimal_body("\"asset\":\"\xe2\x82\"")).has_value());          // truncated
    CHECK(decode(minimal_body("\"asset\":\"\xe2\x82\xac\"")).has_value());
}

TEST_CASE("price_ranges arity is fixed at decode", "[protocol]") {
    // Stricter than Python, which carries whatever list it was given and only unpacks the triple
    // at fire time, inside `_snap_to_grid`.
    CHECK_FALSE(decode(minimal_body(R"("price_ranges":[[0.0,1.0]])")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("price_ranges":[[0.0,1.0,0.01,0.02]])")).has_value());
    CHECK_FALSE(decode(minimal_body(R"("price_ranges":[[0.0,1.0,"x"]])")).has_value());
    CHECK(decode(minimal_body(R"("price_ranges":[])")).has_value());
}

TEST_CASE("an empty price grid decodes to an empty vector and not to the default", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        decode(minimal_body(R"("price_ranges":[])"));

    REQUIRE(decoded.has_value());
    CHECK(decoded.value().price_ranges.empty());
}

TEST_CASE("a length prefix is four bytes and capped", "[protocol]") {
    const std::array<std::byte, 4> small{std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}};
    const hotpath::Decoded<std::uint32_t> length = hotpath::decode_frame_length(small);
    REQUIRE(length.has_value());
    CHECK(length.value() == 256);

    const std::array<std::byte, 4> at_cap{std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}};
    CHECK(hotpath::decode_frame_length(at_cap).has_value());

    const std::array<std::byte, 4> over{std::byte{0}, std::byte{1}, std::byte{0}, std::byte{1}};
    const hotpath::Decoded<std::uint32_t> refused = hotpath::decode_frame_length(over);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == hotpath::DecodeErrorCode::FrameTooLarge);
    CHECK(refused.error().message == "frame length 65537 exceeds max 65536");

    const std::array<std::byte, 3> truncated{std::byte{0}, std::byte{0}, std::byte{0}};
    CHECK_FALSE(hotpath::decode_frame_length(truncated).has_value());
}

TEST_CASE("the encode buffer is reused rather than grown", "[protocol]") {
    std::vector<std::byte> buffer;
    const hotpath::WakeAck first{.schema_version = hotpath::kSchemaVersion,
                                 .correlation_id = "a-long-correlation-id-0000000000",
                                 .received_at_ms = 1,
                                 .status = hotpath::WakeAckStatus::Accepted,
                                 .reason = std::nullopt};
    hotpath::encode_frame_into(first, buffer);
    const std::size_t first_size = buffer.size();

    const hotpath::WakeAck second{.schema_version = hotpath::kSchemaVersion,
                                  .correlation_id = "b",
                                  .received_at_ms = 2,
                                  .status = hotpath::WakeAckStatus::Rejected,
                                  .reason = "no"};
    hotpath::encode_frame_into(second, buffer);

    CHECK(buffer.size() < first_size);
    CHECK(hotpath::decode_wake_ack(
              std::span<const std::byte>(buffer).subspan(hotpath::kFrameLengthBytes))
              .value() == second);
}

TEST_CASE("an ack status outside the literal is refused", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeAck> decoded = hotpath::decode_wake_ack(bytes_of(
        R"({"schema_version":4,"correlation_id":"c","received_at_ms":1,"status":"maybe"})"));

    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == hotpath::DecodeErrorCode::UnknownEnumValue);
}

TEST_CASE("an ack reason is optional and may be null", "[protocol]") {
    const hotpath::Decoded<hotpath::WakeAck> without = hotpath::decode_wake_ack(
        bytes_of(R"({"schema_version":4,"correlation_id":"c","received_at_ms":1,)"
                 R"("status":"accepted"})"));
    REQUIRE(without.has_value());
    CHECK_FALSE(without.value().reason.has_value());

    const hotpath::Decoded<hotpath::WakeAck> null_reason = hotpath::decode_wake_ack(
        bytes_of(R"({"schema_version":4,"correlation_id":"c","received_at_ms":1,)"
                 R"("status":"accepted","reason":null})"));
    REQUIRE(null_reason.has_value());
    CHECK_FALSE(null_reason.value().reason.has_value());

    const hotpath::Decoded<hotpath::WakeAck> with = hotpath::decode_wake_ack(
        bytes_of(R"({"schema_version":4,"correlation_id":"c","received_at_ms":1,)"
                 R"("status":"rejected","reason":"malformed frame"})"));
    REQUIRE(with.has_value());
    CHECK(with.value().reason == "malformed frame");
}

TEST_CASE("a monotonic stamp is comparable only within one clock domain", "[protocol]") {
    hotpath::WakeMessage message;
    message.recv_ns = 5000000000;
    message.clock_domain = "host-a:1787290000";

    CHECK(message.is_clock_comparable_to("host-a:1787290000"));
    // A different host, or the same host after a reboot: the counters are unrelated and their
    // difference is a number with no meaning.
    CHECK_FALSE(message.is_clock_comparable_to("host-b:1787290000"));
    CHECK_FALSE(message.is_clock_comparable_to("host-a:1787290060"));

    message.recv_ns = 0;
    CHECK_FALSE(message.is_clock_comparable_to("host-a:1787290000"));

    // Two processes that both failed to identify themselves are not thereby comparable.
    message.recv_ns = 5000000000;
    message.clock_domain = "";
    CHECK_FALSE(message.is_clock_comparable_to(""));
}
