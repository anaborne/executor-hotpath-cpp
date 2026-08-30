#pragma once

// The JSON layer the wire protocol is made of, sized to that protocol and nothing more.
//
// Hand-written rather than pulled in, for one reason that survived the trade-off and one that did
// not. The one that did: the encoder has to be byte-identical to orjson, and no general-purpose C++
// serializer is, because orjson's float formatting is Ryu's shortest round-trip digits laid out
// with its own fixed/scientific threshold (`format_double` below reproduces it). A library would
// have to be corrected at the same points anyway. The one that did not: parsing is not obviously
// faster here than simdjson would be, and nothing in this file has been measured against it.
//
// Behaviour is matched against orjson 3.12.0, the version prediction-market-infra's uv.lock pins,
// by observation and not by reading its source. Duplicate keys take the last value. Leading zeros,
// a bare `.5`, a trailing `1.`, NaN, Infinity, raw control characters inside a string, invalid
// UTF-8, and an unpaired surrogate escape are all rejected there, so they are rejected here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hotpath/protocol.hpp"

namespace hotpath::json {

// Longest output `format_double` can produce is 25 bytes, from a 17-digit mantissa laid out in
// fixed notation at the smallest exponent that still uses it.
inline constexpr std::size_t kDoubleBufferSize = 32;

// orjson's rendering of a double, byte for byte: the shortest digit string that round-trips, laid
// out fixed when the decimal exponent is in [-5, 16) and scientific otherwise, always with a
// fractional digit in fixed notation and never with one in scientific. The rule was derived by
// observation against orjson 3.12.0 and is held there by `tests/golden/doubles.tsv`.
//
// NaN and the infinities are written as `null`, as orjson writes them.
//
// `out` must be at least `kDoubleBufferSize` long. Returns the number of bytes written.
[[nodiscard]] std::size_t format_double(double value, std::span<char> out) noexcept;

void append(std::vector<std::byte>& out, std::string_view text);

void append_string(std::vector<std::byte>& out, std::string_view text);

void append_int(std::vector<std::byte>& out, std::int64_t value);

void append_double(std::vector<std::byte>& out, double value);

// A pull parser over one JSON document. Every method returns false on failure and leaves the reason
// in `error()`; the first failure is the one kept, so a caller may finish its current structure
// before checking. The error code is the protocol's because the two distinctions the protocol needs
// to make, malformed against wrong-typed, are ones only the parser can see.
class Reader {
public:
    explicit Reader(std::string_view text) noexcept;

    [[nodiscard]] bool begin_object();

    // Advances to the next member of the open object. `present` is false at the closing brace, at
    // which point the object is closed and the reader has returned to its enclosing level.
    [[nodiscard]] bool next_member(std::string& key, bool& present);

    [[nodiscard]] bool begin_array();

    [[nodiscard]] bool next_element(bool& present);

    [[nodiscard]] bool read_string(std::string& out);

    [[nodiscard]] bool read_int(std::int64_t& out);

    [[nodiscard]] bool read_double(double& out);

    [[nodiscard]] bool read_bool(bool& out);

    [[nodiscard]] bool read_null();

    [[nodiscard]] bool at_null() noexcept;

    // Consumes trailing whitespace and fails if anything else follows the document, as orjson
    // does with `{"a":1}x`.
    [[nodiscard]] bool at_document_end();

    [[nodiscard]] const DecodeError& error() const noexcept { return error_; }

private:
    // Deeper than any frame this protocol defines: an object holding an array of arrays is three.
    // A cap is the parser's own protection against a hostile document, since the descent is driven
    // by the caller's stack.
    static constexpr std::size_t kMaxDepth = 8;

    bool fail(DecodeErrorCode code, std::string message);
    void skip_whitespace() noexcept;
    [[nodiscard]] bool peek(char& c) noexcept;
    [[nodiscard]] bool expect(char c);
    [[nodiscard]] bool push_level();
    [[nodiscard]] bool scan_number(std::string_view& token, bool& integral);
    [[nodiscard]] bool scan_string(std::string& out);
    [[nodiscard]] bool append_escape(std::string& out);
    [[nodiscard]] bool append_code_point(std::string& out);
    [[nodiscard]] bool validate_utf8_run(std::size_t& index);

    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;
    std::array<bool, kMaxDepth> first_{};
    DecodeError error_;
    bool failed_ = false;
};

}  // namespace hotpath::json
