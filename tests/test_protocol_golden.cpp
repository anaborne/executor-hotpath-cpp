// The contract test. Each frame under tests/golden/frames was produced by running
// prediction-market-infra's own `ipc/protocol.py` under orjson 3.12.0 (tests/golden/SOURCE.txt
// records the checkout and the versions), so a byte comparison against one compares against the
// Python itself.
//
// Encoding is checked whole, length prefix included, because the prefix is part of the frame and a
// decoder that agreed about the body and disagreed about the four bytes in front of it would still
// desynchronize the stream.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hotpath/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

std::vector<std::byte> read_golden(std::string_view name) {
    const std::filesystem::path path =
        std::filesystem::path(HOTPATH_GOLDEN_DIR) / "frames" / (std::string(name) + ".frame");
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> frame(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        frame[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return frame;
}

// Compared as text rather than as bytes so a failure prints the two frames side by side instead of
// two vector lengths.
std::string as_text(std::span<const std::byte> frame) {
    return {reinterpret_cast<const char*>(frame.data()), frame.size()};
}

std::span<const std::byte> body_of(const std::vector<std::byte>& frame) {
    return std::span<const std::byte>(frame).subspan(hotpath::kFrameLengthBytes);
}

hotpath::WakeMessage required_fields() {
    hotpath::WakeMessage message;
    message.schema_version = hotpath::kSchemaVersion;
    message.correlation_id = "0f9e6a2c-7d41-4b8e-9a55-1c2d3e4f5a6b";
    message.market_ticker = "KXBTCD-26AUG2917-T112750";
    message.asset = "BTC";
    message.direction = hotpath::Direction::Yes;
    message.kalshi_price = 0.56;
    message.model_probability = 0.62;
    message.fee = 0.02;
    message.edge = 0.06;
    message.decision_ts_ms = 1787289600123;
    message.sent_at_ms = 1787289600124;
    message.sent_at_ns = 412737100845213;
    return message;
}

void check_round_trip(std::string_view name, const hotpath::WakeMessage& message) {
    const std::vector<std::byte> golden = read_golden(name);

    CHECK(as_text(hotpath::encode_frame(message)) == as_text(golden));

    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        hotpath::decode_wake_message(body_of(golden));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value() == message);
}

}  // namespace

TEST_CASE("a message carrying only the required fields encodes every default", "[golden]") {
    check_round_trip("wake_defaults", required_fields());
}

TEST_CASE("a fully populated v4 message round trips", "[golden]") {
    hotpath::WakeMessage message = required_fields();
    message.recv_ns = 412737100311907;
    message.clock_domain = "Ivory.local:1787289000";
    message.dry_run = true;
    message.wire_price_yes_dollars = 0.56;
    message.exchange_index = 2;
    message.available_size_contracts = 143.0;
    message.price_ranges = {
        {.start = 0.0, .end = 0.05, .step = 0.001},
        {.start = 0.05, .end = 0.95, .step = 0.01},
        {.start = 0.95, .end = 1.0, .step = 0.001},
    };
    message.correlation_group = "btc-majors";

    check_round_trip("wake_full_yes", message);
}

TEST_CASE("a no-side fire carries the YES-side wire price", "[golden]") {
    hotpath::WakeMessage message = required_fields();
    message.direction = hotpath::Direction::No;
    message.kalshi_price = 0.30;
    message.wire_price_yes_dollars = 0.70;

    check_round_trip("wake_no_side", message);
}

TEST_CASE("strings needing escapes encode as orjson escapes them", "[golden]") {
    hotpath::WakeMessage message = required_fields();
    message.correlation_id = "quote:\" backslash:\\ tab:\t newline:\n";
    message.asset = std::string("\x00\x1f\b\f\r", 5);
    message.market_ticker = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x9a\x80";
    message.clock_domain = "host:\xc3\xbc";
    message.correlation_group = "\xe4\xb8\xad\xe6\x96\x87";

    check_round_trip("wake_escapes", message);
}

TEST_CASE("floats encode with orjson's digits and notation", "[golden]") {
    hotpath::WakeMessage message = required_fields();
    message.kalshi_price = 0.1 + 0.2;
    message.model_probability = 1.0 / 3.0;
    message.fee = 1e-6;
    message.edge = -0.0;
    message.wire_price_yes_dollars = 0.9999999999999999;
    message.available_size_contracts = 1e16;
    message.price_ranges = {
        {.start = 0.0, .end = 1.0, .step = 0.01},
        {.start = 1e-5,
         .end = std::numeric_limits<double>::denorm_min(),
         .step = std::numeric_limits<double>::max()},
    };

    check_round_trip("wake_awkward_floats", message);
}

TEST_CASE("an accepted ack carries a null reason", "[golden]") {
    const hotpath::WakeAck ack{
        .schema_version = hotpath::kSchemaVersion,
        .correlation_id = "0f9e6a2c-7d41-4b8e-9a55-1c2d3e4f5a6b",
        .received_at_ms = 1787289600125,
        .status = hotpath::WakeAckStatus::Accepted,
        .reason = std::nullopt,
    };
    const std::vector<std::byte> golden = read_golden("ack_accepted");

    CHECK(as_text(hotpath::encode_frame(ack)) == as_text(golden));

    const hotpath::Decoded<hotpath::WakeAck> decoded = hotpath::decode_wake_ack(body_of(golden));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value() == ack);
}

TEST_CASE("a rejected ack carries the reason and no correlation id", "[golden]") {
    const hotpath::WakeAck ack{
        .schema_version = hotpath::kSchemaVersion,
        .correlation_id = "",
        .received_at_ms = 1787289600125,
        .status = hotpath::WakeAckStatus::Rejected,
        .reason = "WakeMessage.__init__() got an unexpected keyword argument 'wat'",
    };
    const std::vector<std::byte> golden = read_golden("ack_rejected");

    CHECK(as_text(hotpath::encode_frame(ack)) == as_text(golden));

    const hotpath::Decoded<hotpath::WakeAck> decoded = hotpath::decode_wake_ack(body_of(golden));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value() == ack);
}

// The rolling-restart cases: a poller running an older ref sends a frame with fewer keys, and every
// field added since has to arrive at its Python default. Two of those defaults are refusals rather
// than neutral values, so an old poller causes a refused fire and not a wrong order.
TEST_CASE("a v1 frame decodes with every later field at its default", "[golden]") {
    const std::vector<std::byte> golden = read_golden("legacy_v1");
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        hotpath::decode_wake_message(body_of(golden));

    REQUIRE(decoded.has_value());
    const hotpath::WakeMessage& message = decoded.value();
    CHECK(message.schema_version == 1);
    CHECK(message.correlation_id == "corr-v1");
    CHECK(message.recv_ns == 0);
    CHECK(message.clock_domain.empty());
    CHECK_FALSE(message.dry_run);
    CHECK(message.wire_price_yes_dollars == 0.0);
    CHECK(message.exchange_index == -1);
    CHECK(message.available_size_contracts == 0.0);
    CHECK(message.price_ranges == std::vector<hotpath::PriceRange>{hotpath::kDefaultPriceRange});
    CHECK(message.correlation_group.empty());
}

TEST_CASE("a v2 frame decodes with an untradeable wire price", "[golden]") {
    const std::vector<std::byte> golden = read_golden("legacy_v2");
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        hotpath::decode_wake_message(body_of(golden));

    REQUIRE(decoded.has_value());
    CHECK(decoded.value().correlation_id == "corr-v2");
    CHECK(decoded.value().clock_domain == "host-a:1787290000");
    CHECK(decoded.value().wire_price_yes_dollars == 0.0);
    CHECK(decoded.value().exchange_index == -1);
    CHECK(decoded.value().correlation_group.empty());
}

TEST_CASE("a v3 frame decodes with an unknown correlation group", "[golden]") {
    const std::vector<std::byte> golden = read_golden("legacy_v3");
    const hotpath::Decoded<hotpath::WakeMessage> decoded =
        hotpath::decode_wake_message(body_of(golden));

    REQUIRE(decoded.has_value());
    CHECK(decoded.value().correlation_id == "corr-v3");
    CHECK(decoded.value().wire_price_yes_dollars == 0.56);
    CHECK(decoded.value().exchange_index == 2);
    CHECK(decoded.value().correlation_group.empty());
}
