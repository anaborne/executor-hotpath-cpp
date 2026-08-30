#pragma once

// Wire protocol for the poller->executor wake channel, ported from `ipc/protocol.py` in
// prediction-market-infra. A 4-byte big-endian length prefix followed by an orjson-encoded body.
//
// The Python dataclasses are the specification, and two of their properties are load-bearing here.
// orjson serializes a dataclass in field-declaration order, so the member order below is the wire
// key order and reordering these declarations is a wire change. And `WakeMessage.from_dict` is
// `cls(**data)`, which raises on an unexpected keyword, so a decoder that ignored an unknown key
// would accept frames the Python rejects.
//
// Every field added after v1 carries the Python default, because a v4 decoder has to read a
// v1/v2/v3 frame during a rolling restart. Two of those defaults are refusal values rather than
// neutral ones: `wire_price_yes_dollars = 0.0` is not a tradeable price and
// `correlation_group = ""` means unknown, so an older poller degrades to a refused fire instead of
// a mispriced order or a wrong exposure cap.
//
// This decoder is stricter than Python's in six places, each recorded in PORT-FIDELITY.md.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hotpath {

inline constexpr std::int64_t kSchemaVersion = 4;
inline constexpr std::size_t kFrameLengthBytes = 4;
inline constexpr std::uint32_t kMaxFrameBytes = 64 * 1024;

enum class Direction : std::uint8_t { Yes, No };

enum class WakeAckStatus : std::uint8_t { Accepted, Rejected };

// One `[start, end, step]` triple of the market's price grid. Python carries these as bare lists
// and only unpacks them at fire time, in `_snap_to_grid`; this type fixes the arity at decode.
struct PriceRange {
    double start = 0.0;
    double end = 0.0;
    double step = 0.0;

    bool operator==(const PriceRange&) const = default;
};

inline constexpr PriceRange kDefaultPriceRange{.start = 0.0, .end = 1.0, .step = 0.01};

struct WakeMessage {
    std::int64_t schema_version = kSchemaVersion;
    std::string correlation_id;
    std::string market_ticker;
    std::string asset;
    Direction direction = Direction::Yes;
    double kalshi_price = 0.0;
    double model_probability = 0.0;
    double fee = 0.0;
    double edge = 0.0;
    std::int64_t decision_ts_ms = 0;
    std::int64_t sent_at_ms = 0;
    std::int64_t sent_at_ns = 0;
    std::int64_t recv_ns = 0;
    std::string clock_domain;
    bool dry_run = false;
    double wire_price_yes_dollars = 0.0;
    std::int64_t exchange_index = -1;
    double available_size_contracts = 0.0;
    std::vector<PriceRange> price_ranges{kDefaultPriceRange};
    std::string correlation_group;

    bool operator==(const WakeMessage&) const = default;

    // Whether this message's `recv_ns` may be subtracted from a local monotonic reading. Both
    // conditions hold or neither is meaningful: something stamped the frame, and it was the same
    // host and the same boot. Comparing across either boundary yields no error, only a wrong
    // number.
    [[nodiscard]] bool is_clock_comparable_to(std::string_view local_clock_domain) const noexcept;
};

struct WakeAck {
    std::int64_t schema_version = kSchemaVersion;
    std::string correlation_id;
    std::int64_t received_at_ms = 0;
    WakeAckStatus status = WakeAckStatus::Accepted;
    std::optional<std::string> reason;

    bool operator==(const WakeAck&) const = default;
};

enum class DecodeErrorCode : std::uint8_t {
    MalformedJson,
    UnknownField,
    MissingField,
    WrongType,
    OutOfRange,
    UnknownEnumValue,
    MalformedPriceRange,
    MalformedFrame,
    FrameTooLarge,
};

struct DecodeError {
    DecodeErrorCode code = DecodeErrorCode::MalformedJson;
    std::string message;
};

// A decoded value or the reason there is not one. `WakeAck.reason` is a string on the wire, so the
// message travels back to the poller and has to be worth reading there.
template <typename T>
class Decoded {
public:
    static Decoded ok(T value) { return Decoded(std::move(value)); }

    static Decoded fail(DecodeErrorCode code, std::string message) {
        return Decoded(DecodeError{.code = code, .message = std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(slot_); }

    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const T& value() const { return std::get<T>(slot_); }

    [[nodiscard]] const DecodeError& error() const { return std::get<DecodeError>(slot_); }

private:
    explicit Decoded(T value) : slot_(std::move(value)) {}

    explicit Decoded(DecodeError error) : slot_(std::move(error)) {}

    std::variant<T, DecodeError> slot_;
};

[[nodiscard]] std::vector<std::byte> encode_frame(const WakeMessage& message);

[[nodiscard]] std::vector<std::byte> encode_frame(const WakeAck& ack);

// Overwrites `out` rather than returning a fresh buffer, so the ack path can hold one buffer for
// the life of a connection instead of allocating per wake.
void encode_frame_into(const WakeMessage& message, std::vector<std::byte>& out);

void encode_frame_into(const WakeAck& ack, std::vector<std::byte>& out);

// The declared body length of a frame whose 4-byte prefix is `prefix`. Rejects anything above
// `kMaxFrameBytes`, so a corrupt prefix cannot become an unbounded read.
[[nodiscard]] Decoded<std::uint32_t> decode_frame_length(std::span<const std::byte> prefix);

[[nodiscard]] Decoded<WakeMessage> decode_wake_message(std::span<const std::byte> body);

[[nodiscard]] Decoded<WakeAck> decode_wake_ack(std::span<const std::byte> body);

}  // namespace hotpath
