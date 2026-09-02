#include "hotpath/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"

namespace hotpath {
namespace {

constexpr std::uint32_t kSchemaVersionBit = 1U << 0U;
constexpr std::uint32_t kCorrelationIdBit = 1U << 1U;
constexpr std::uint32_t kMarketTickerBit = 1U << 2U;
constexpr std::uint32_t kAssetBit = 1U << 3U;
constexpr std::uint32_t kDirectionBit = 1U << 4U;
constexpr std::uint32_t kKalshiPriceBit = 1U << 5U;
constexpr std::uint32_t kModelProbabilityBit = 1U << 6U;
constexpr std::uint32_t kFeeBit = 1U << 7U;
constexpr std::uint32_t kEdgeBit = 1U << 8U;
constexpr std::uint32_t kDecisionTsMsBit = 1U << 9U;
constexpr std::uint32_t kSentAtMsBit = 1U << 10U;
constexpr std::uint32_t kSentAtNsBit = 1U << 11U;
constexpr std::uint32_t kReceivedAtMsBit = 1U << 12U;
constexpr std::uint32_t kStatusBit = 1U << 13U;

using RequiredField = std::pair<std::uint32_t, std::string_view>;

constexpr std::array<RequiredField, 12> kWakeMessageRequired{{
    {kSchemaVersionBit, "schema_version"},
    {kCorrelationIdBit, "correlation_id"},
    {kMarketTickerBit, "market_ticker"},
    {kAssetBit, "asset"},
    {kDirectionBit, "direction"},
    {kKalshiPriceBit, "kalshi_price"},
    {kModelProbabilityBit, "model_probability"},
    {kFeeBit, "fee"},
    {kEdgeBit, "edge"},
    {kDecisionTsMsBit, "decision_ts_ms"},
    {kSentAtMsBit, "sent_at_ms"},
    {kSentAtNsBit, "sent_at_ns"},
}};

constexpr std::array<RequiredField, 4> kWakeAckRequired{{
    {kSchemaVersionBit, "schema_version"},
    {kCorrelationIdBit, "correlation_id"},
    {kReceivedAtMsBit, "received_at_ms"},
    {kStatusBit, "status"},
}};

template <typename T, std::size_t N>
Decoded<T> missing_field(std::uint32_t seen, const std::array<RequiredField, N>& required) {
    for (const auto& [bit, name] : required) {
        if ((seen & bit) == 0U) {
            return Decoded<T>::fail(DecodeErrorCode::MissingField,
                                    "missing required field: " + std::string(name));
        }
    }
    return Decoded<T>::fail(DecodeErrorCode::MissingField, "missing required field");
}

template <typename T>
Decoded<T> reader_failure(const json::Reader& reader) {
    return Decoded<T>::fail(reader.error().code, reader.error().message);
}

std::string_view text_of(std::span<const std::byte> body) {
    if (body.empty()) {
        return {};
    }
    // The one place bytes off a socket become characters. clang-tidy's reinterpret-cast check is
    // disabled for exactly this: a wire reader is made of the operations it bans.
    return {reinterpret_cast<const char*>(body.data()), body.size()};
}

void write_length_prefix(std::vector<std::byte>& out) {
    const auto length = static_cast<std::uint32_t>(out.size() - kFrameLengthBytes);
    out[0] = static_cast<std::byte>((length >> 24U) & 0xFFU);
    out[1] = static_cast<std::byte>((length >> 16U) & 0xFFU);
    out[2] = static_cast<std::byte>((length >> 8U) & 0xFFU);
    out[3] = static_cast<std::byte>(length & 0xFFU);
}

std::string_view name_of(Direction direction) noexcept {
    return direction == Direction::Yes ? "yes" : "no";
}

std::string_view name_of(WakeAckStatus status) noexcept {
    return status == WakeAckStatus::Accepted ? "accepted" : "rejected";
}

constexpr std::uint32_t kWakeMessageRequiredMask =
    kSchemaVersionBit | kCorrelationIdBit | kMarketTickerBit | kAssetBit | kDirectionBit |
    kKalshiPriceBit | kModelProbabilityBit | kFeeBit | kEdgeBit | kDecisionTsMsBit | kSentAtMsBit |
    kSentAtNsBit;

constexpr std::uint32_t kWakeAckRequiredMask =
    kSchemaVersionBit | kCorrelationIdBit | kReceivedAtMsBit | kStatusBit;

DecodeError price_range_arity_error() {
    return DecodeError{.code = DecodeErrorCode::MalformedPriceRange,
                       .message = "price_ranges entry is not [start, end, step]"};
}

std::optional<DecodeError> read_price_ranges(json::Reader& reader,
                                             std::vector<PriceRange>& ranges) {
    ranges.clear();
    if (!reader.begin_array()) {
        return reader.error();
    }
    while (true) {
        bool has_entry = false;
        if (!reader.next_element(has_entry)) {
            return reader.error();
        }
        if (!has_entry) {
            return std::nullopt;
        }
        if (!reader.begin_array()) {
            return reader.error();
        }

        std::array<double, 3> bounds{};
        for (double& bound : bounds) {
            bool has_bound = false;
            if (!reader.next_element(has_bound)) {
                return reader.error();
            }
            if (!has_bound) {
                return price_range_arity_error();
            }
            if (!reader.read_double(bound)) {
                return reader.error();
            }
        }
        bool extra = false;
        if (!reader.next_element(extra)) {
            return reader.error();
        }
        if (extra) {
            return price_range_arity_error();
        }
        ranges.push_back(PriceRange{.start = bounds[0], .end = bounds[1], .step = bounds[2]});
    }
}

std::optional<DecodeError> read_direction(json::Reader& reader, Direction& direction) {
    std::string text;
    if (!reader.read_string(text)) {
        return reader.error();
    }
    if (text == "yes") {
        direction = Direction::Yes;
        return std::nullopt;
    }
    if (text == "no") {
        direction = Direction::No;
        return std::nullopt;
    }
    // Stricter than Python, which does not check a Literal at runtime. PORT-FIDELITY.md says why.
    return DecodeError{.code = DecodeErrorCode::UnknownEnumValue,
                       .message = "direction must be yes or no, got " + text};
}

std::optional<DecodeError> read_status(json::Reader& reader, WakeAckStatus& status) {
    std::string text;
    if (!reader.read_string(text)) {
        return reader.error();
    }
    if (text == "accepted") {
        status = WakeAckStatus::Accepted;
        return std::nullopt;
    }
    if (text == "rejected") {
        status = WakeAckStatus::Rejected;
        return std::nullopt;
    }
    return DecodeError{.code = DecodeErrorCode::UnknownEnumValue,
                       .message = "status must be accepted or rejected, got " + text};
}

std::optional<DecodeError> read_wake_field(json::Reader& reader, std::string_view key,
                                           WakeMessage& message, std::uint32_t& seen) {
    bool ok = true;
    if (key == "schema_version") {
        seen |= kSchemaVersionBit;
        ok = reader.read_int(message.schema_version);
    } else if (key == "correlation_id") {
        seen |= kCorrelationIdBit;
        ok = reader.read_string(message.correlation_id);
    } else if (key == "market_ticker") {
        seen |= kMarketTickerBit;
        ok = reader.read_string(message.market_ticker);
    } else if (key == "asset") {
        seen |= kAssetBit;
        ok = reader.read_string(message.asset);
    } else if (key == "direction") {
        seen |= kDirectionBit;
        return read_direction(reader, message.direction);
    } else if (key == "kalshi_price") {
        seen |= kKalshiPriceBit;
        ok = reader.read_double(message.kalshi_price);
    } else if (key == "model_probability") {
        seen |= kModelProbabilityBit;
        ok = reader.read_double(message.model_probability);
    } else if (key == "fee") {
        seen |= kFeeBit;
        ok = reader.read_double(message.fee);
    } else if (key == "edge") {
        seen |= kEdgeBit;
        ok = reader.read_double(message.edge);
    } else if (key == "decision_ts_ms") {
        seen |= kDecisionTsMsBit;
        ok = reader.read_int(message.decision_ts_ms);
    } else if (key == "sent_at_ms") {
        seen |= kSentAtMsBit;
        ok = reader.read_int(message.sent_at_ms);
    } else if (key == "sent_at_ns") {
        seen |= kSentAtNsBit;
        ok = reader.read_int(message.sent_at_ns);
    } else if (key == "recv_ns") {
        ok = reader.read_int(message.recv_ns);
    } else if (key == "clock_domain") {
        ok = reader.read_string(message.clock_domain);
    } else if (key == "dry_run") {
        ok = reader.read_bool(message.dry_run);
    } else if (key == "wire_price_yes_dollars") {
        ok = reader.read_double(message.wire_price_yes_dollars);
    } else if (key == "exchange_index") {
        ok = reader.read_int(message.exchange_index);
    } else if (key == "available_size_contracts") {
        ok = reader.read_double(message.available_size_contracts);
    } else if (key == "price_ranges") {
        return read_price_ranges(reader, message.price_ranges);
    } else if (key == "correlation_group") {
        ok = reader.read_string(message.correlation_group);
    } else {
        // Python's `cls(**data)` raises on an unexpected keyword, so an unknown key is a
        // rejected frame here and not a silent upgrade.
        return DecodeError{.code = DecodeErrorCode::UnknownField,
                           .message = "unexpected field: " + std::string(key)};
    }
    if (ok) {
        return std::nullopt;
    }
    return reader.error();
}

std::optional<DecodeError> read_ack_field(json::Reader& reader, std::string_view key, WakeAck& ack,
                                          std::uint32_t& seen) {
    bool ok = true;
    if (key == "schema_version") {
        seen |= kSchemaVersionBit;
        ok = reader.read_int(ack.schema_version);
    } else if (key == "correlation_id") {
        seen |= kCorrelationIdBit;
        ok = reader.read_string(ack.correlation_id);
    } else if (key == "received_at_ms") {
        seen |= kReceivedAtMsBit;
        ok = reader.read_int(ack.received_at_ms);
    } else if (key == "status") {
        seen |= kStatusBit;
        return read_status(reader, ack.status);
    } else if (key == "reason") {
        if (reader.at_null()) {
            ok = reader.read_null();
            ack.reason.reset();
        } else {
            std::string reason;
            ok = reader.read_string(reason);
            if (ok) {
                ack.reason = std::move(reason);
            }
        }
    } else {
        return DecodeError{.code = DecodeErrorCode::UnknownField,
                           .message = "unexpected field: " + std::string(key)};
    }
    if (ok) {
        return std::nullopt;
    }
    return reader.error();
}

}  // namespace

bool WakeMessage::is_clock_comparable_to(std::string_view local_clock_domain) const noexcept {
    return recv_ns > 0 && !local_clock_domain.empty() && clock_domain == local_clock_domain;
}

// Key order is the Python attribute order, because orjson serializes a dataclass in
// field-declaration order and the golden frames are byte-compared against its output.
void encode_frame_into(const WakeMessage& message, std::vector<std::byte>& out) {
    out.assign(kFrameLengthBytes, std::byte{0});

    json::append(out, R"({"schema_version":)");
    json::append_int(out, message.schema_version);
    json::append(out, R"(,"correlation_id":)");
    json::append_string(out, message.correlation_id);
    json::append(out, R"(,"market_ticker":)");
    json::append_string(out, message.market_ticker);
    json::append(out, R"(,"asset":)");
    json::append_string(out, message.asset);
    json::append(out, R"(,"direction":)");
    json::append_string(out, name_of(message.direction));
    json::append(out, R"(,"kalshi_price":)");
    json::append_double(out, message.kalshi_price);
    json::append(out, R"(,"model_probability":)");
    json::append_double(out, message.model_probability);
    json::append(out, R"(,"fee":)");
    json::append_double(out, message.fee);
    json::append(out, R"(,"edge":)");
    json::append_double(out, message.edge);
    json::append(out, R"(,"decision_ts_ms":)");
    json::append_int(out, message.decision_ts_ms);
    json::append(out, R"(,"sent_at_ms":)");
    json::append_int(out, message.sent_at_ms);
    json::append(out, R"(,"sent_at_ns":)");
    json::append_int(out, message.sent_at_ns);
    json::append(out, R"(,"recv_ns":)");
    json::append_int(out, message.recv_ns);
    json::append(out, R"(,"clock_domain":)");
    json::append_string(out, message.clock_domain);
    json::append(out, R"(,"dry_run":)");
    json::append(out, message.dry_run ? "true" : "false");
    json::append(out, R"(,"wire_price_yes_dollars":)");
    json::append_double(out, message.wire_price_yes_dollars);
    json::append(out, R"(,"exchange_index":)");
    json::append_int(out, message.exchange_index);
    json::append(out, R"(,"available_size_contracts":)");
    json::append_double(out, message.available_size_contracts);

    json::append(out, R"(,"price_ranges":[)");
    bool first = true;
    for (const PriceRange& range : message.price_ranges) {
        if (!first) {
            out.push_back(std::byte{','});
        }
        first = false;
        out.push_back(std::byte{'['});
        json::append_double(out, range.start);
        out.push_back(std::byte{','});
        json::append_double(out, range.end);
        out.push_back(std::byte{','});
        json::append_double(out, range.step);
        out.push_back(std::byte{']'});
    }
    out.push_back(std::byte{']'});

    json::append(out, R"(,"correlation_group":)");
    json::append_string(out, message.correlation_group);
    out.push_back(std::byte{'}'});

    write_length_prefix(out);
}

void encode_frame_into(const WakeAck& ack, std::vector<std::byte>& out) {
    out.assign(kFrameLengthBytes, std::byte{0});

    json::append(out, R"({"schema_version":)");
    json::append_int(out, ack.schema_version);
    json::append(out, R"(,"correlation_id":)");
    json::append_string(out, ack.correlation_id);
    json::append(out, R"(,"received_at_ms":)");
    json::append_int(out, ack.received_at_ms);
    json::append(out, R"(,"status":)");
    json::append_string(out, name_of(ack.status));
    json::append(out, R"(,"reason":)");
    if (ack.reason.has_value()) {
        json::append_string(out, *ack.reason);
    } else {
        json::append(out, "null");
    }
    out.push_back(std::byte{'}'});

    write_length_prefix(out);
}

std::vector<std::byte> encode_frame(const WakeMessage& message) {
    std::vector<std::byte> out;
    encode_frame_into(message, out);
    return out;
}

std::vector<std::byte> encode_frame(const WakeAck& ack) {
    std::vector<std::byte> out;
    encode_frame_into(ack, out);
    return out;
}

Decoded<std::uint32_t> decode_frame_length(std::span<const std::byte> prefix) {
    if (prefix.size() != kFrameLengthBytes) {
        return Decoded<std::uint32_t>::fail(DecodeErrorCode::MalformedFrame,
                                            "length prefix must be 4 bytes");
    }
    std::uint32_t length = 0;
    for (const std::byte byte : prefix) {
        length = (length << 8U) | static_cast<std::uint32_t>(byte);
    }
    if (length > kMaxFrameBytes) {
        return Decoded<std::uint32_t>::fail(DecodeErrorCode::FrameTooLarge,
                                            "frame length " + std::to_string(length) +
                                                " exceeds max " + std::to_string(kMaxFrameBytes));
    }
    return Decoded<std::uint32_t>::ok(length);
}

Decoded<WakeMessage> decode_wake_message(std::span<const std::byte> body) {
    json::Reader reader(text_of(body));
    WakeMessage message;
    std::uint32_t seen = 0;

    if (!reader.begin_object()) {
        return reader_failure<WakeMessage>(reader);
    }

    std::string key;
    while (true) {
        bool present = false;
        if (!reader.next_member(key, present)) {
            return reader_failure<WakeMessage>(reader);
        }
        if (!present) {
            break;
        }
        if (const std::optional<DecodeError> failure =
                read_wake_field(reader, key, message, seen)) {
            return Decoded<WakeMessage>::fail(failure->code, failure->message);
        }
    }

    if (!reader.at_document_end()) {
        return reader_failure<WakeMessage>(reader);
    }
    if ((seen & kWakeMessageRequiredMask) != kWakeMessageRequiredMask) {
        return missing_field<WakeMessage>(seen, kWakeMessageRequired);
    }
    return Decoded<WakeMessage>::ok(std::move(message));
}

Decoded<WakeAck> decode_wake_ack(std::span<const std::byte> body) {
    json::Reader reader(text_of(body));
    WakeAck ack;
    std::uint32_t seen = 0;

    if (!reader.begin_object()) {
        return reader_failure<WakeAck>(reader);
    }

    std::string key;
    while (true) {
        bool present = false;
        if (!reader.next_member(key, present)) {
            return reader_failure<WakeAck>(reader);
        }
        if (!present) {
            break;
        }
        if (const std::optional<DecodeError> failure = read_ack_field(reader, key, ack, seen)) {
            return Decoded<WakeAck>::fail(failure->code, failure->message);
        }
    }

    if (!reader.at_document_end()) {
        return reader_failure<WakeAck>(reader);
    }
    if ((seen & kWakeAckRequiredMask) != kWakeAckRequiredMask) {
        return missing_field<WakeAck>(seen, kWakeAckRequired);
    }
    return Decoded<WakeAck>::ok(std::move(ack));
}

}  // namespace hotpath
