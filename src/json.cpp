#include "json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "hotpath/protocol.hpp"

namespace hotpath::json {
namespace {

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr std::byte as_byte(char c) noexcept {
    return static_cast<std::byte>(static_cast<unsigned char>(c));
}

// A `DecodeError` message goes back out on the wire inside a rejected ack, and the frame that
// produced it is not trusted to be UTF-8. A byte lifted out of one goes out as hex, because a raw
// 0xFF spliced into the reason makes the whole ack undecodable to the poller.
std::string hex_byte(char c) {
    static constexpr std::string_view kByteHexDigits = "0123456789abcdef";
    const auto value = static_cast<std::size_t>(static_cast<unsigned char>(c));
    return std::string{kByteHexDigits[value >> 4U], kByteHexDigits[value & 0x0FU]};
}

// `std::string_view::compare(pos, ...)` throws when `pos` is past the end, which the parser never
// does but the compiler cannot know, and one potentially-throwing call is enough to cost `at_null`
// its noexcept.
constexpr bool starts_with(std::string_view text, std::size_t pos,
                           std::string_view token) noexcept {
    return pos <= text.size() && text.size() - pos >= token.size() &&
           std::string_view(text.data() + pos, token.size()) == token;
}

// The exponent range orjson lays out in fixed notation. Outside it the output is scientific, and
// the boundaries are exact: 1e15 prints as 1000000000000000.0 and 1e16 as 1e+16.
constexpr int kFixedNotationMinExponent = -5;
constexpr int kFixedNotationMaxExponent = 16;

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

}  // namespace

std::size_t format_double(double value, std::span<char> out) noexcept {
    // orjson writes NaN and both infinities as `null`. No decoded frame can carry one, since the
    // reader refuses the tokens, so this is reached only by a message constructed in C++.
    if (!std::isfinite(value)) {
        static constexpr std::string_view kNull = "null";
        std::memcpy(out.data(), kNull.data(), kNull.size());
        return kNull.size();
    }

    std::array<char, 48> scientific{};
    const std::to_chars_result written =
        std::to_chars(scientific.data(), scientific.data() + scientific.size(), value,
                      std::chars_format::scientific);
    const std::string_view text(scientific.data(),
                                static_cast<std::size_t>(written.ptr - scientific.data()));

    std::size_t cursor = 0;
    const bool negative = text.front() == '-';
    if (negative) {
        cursor = 1;
    }

    std::array<char, 32> digits{};
    std::size_t digit_count = 0;
    for (; cursor < text.size() && text[cursor] != 'e'; ++cursor) {
        if (text[cursor] != '.') {
            digits[digit_count] = text[cursor];
            ++digit_count;
        }
    }

    int exponent = 0;
    const char* exponent_begin = text.data() + cursor + 1;
    if (*exponent_begin == '+') {
        ++exponent_begin;
    }
    std::from_chars(exponent_begin, text.data() + text.size(), exponent);

    std::size_t length = 0;
    const auto put = [&out, &length](char c) {
        out[length] = c;
        ++length;
    };
    const auto put_digits = [&put, &digits](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
            put(digits[i]);
        }
    };

    if (negative) {
        put('-');
    }

    if (exponent >= kFixedNotationMinExponent && exponent < kFixedNotationMaxExponent) {
        if (exponent >= 0) {
            const auto whole = static_cast<std::size_t>(exponent) + 1;
            for (std::size_t i = 0; i < whole; ++i) {
                put(i < digit_count ? digits[i] : '0');
            }
            put('.');
            if (digit_count > whole) {
                put_digits(whole, digit_count);
            } else {
                put('0');
            }
        } else {
            put('0');
            put('.');
            for (int i = 0; i < -exponent - 1; ++i) {
                put('0');
            }
            put_digits(0, digit_count);
        }
        return length;
    }

    put(digits[0]);
    if (digit_count > 1) {
        put('.');
        put_digits(1, digit_count);
    }
    put('e');
    put(exponent >= 0 ? '+' : '-');
    std::array<char, 8> magnitude{};
    const std::to_chars_result exponent_text =
        std::to_chars(magnitude.data(), magnitude.data() + magnitude.size(),
                      exponent >= 0 ? exponent : -exponent);
    for (const char* c = magnitude.data(); c != exponent_text.ptr; ++c) {
        put(*c);
    }
    return length;
}

void append(std::vector<std::byte>& out, std::string_view text) {
    const std::size_t offset = out.size();
    out.resize(offset + text.size());
    std::memcpy(out.data() + offset, text.data(), text.size());
}

void append_string(std::vector<std::byte>& out, std::string_view text) {
    static constexpr std::string_view kHexDigits = "0123456789abcdef";

    out.push_back(std::byte{'"'});
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (byte) {
            case '"':
                append(out, R"(\")");
                break;
            case '\\':
                append(out, R"(\\)");
                break;
            case '\b':
                append(out, R"(\b)");
                break;
            case '\f':
                append(out, R"(\f)");
                break;
            case '\n':
                append(out, R"(\n)");
                break;
            case '\r':
                append(out, R"(\r)");
                break;
            case '\t':
                append(out, R"(\t)");
                break;
            default:
                if (byte < 0x20U) {
                    append(out, R"(\u00)");
                    out.push_back(as_byte(kHexDigits[byte >> 4U]));
                    out.push_back(as_byte(kHexDigits[byte & 0x0FU]));
                } else {
                    out.push_back(std::byte{byte});
                }
                break;
        }
    }
    out.push_back(std::byte{'"'});
}

void append_int(std::vector<std::byte>& out, std::int64_t value) {
    std::array<char, 24> buffer{};
    const std::to_chars_result written =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    append(out,
           std::string_view(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data())));
}

void append_double(std::vector<std::byte>& out, double value) {
    std::array<char, kDoubleBufferSize> buffer{};
    append(out, std::string_view(buffer.data(), format_double(value, buffer)));
}

Reader::Reader(std::string_view text) noexcept : text_(text) {}

bool Reader::fail(DecodeErrorCode code, std::string message) {
    if (!failed_) {
        failed_ = true;
        error_ = DecodeError{.code = code, .message = std::move(message)};
    }
    return false;
}

void Reader::skip_whitespace() noexcept {
    while (pos_ < text_.size()) {
        const char c = text_[pos_];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return;
        }
        ++pos_;
    }
}

bool Reader::peek(char& c) noexcept {
    skip_whitespace();
    if (pos_ >= text_.size()) {
        return false;
    }
    c = text_[pos_];
    return true;
}

bool Reader::expect(char c) {
    char seen = 0;
    if (!peek(seen)) {
        return fail(DecodeErrorCode::MalformedJson,
                    std::string("expected '") + c + "', got end of input");
    }
    if (seen != c) {
        return fail(DecodeErrorCode::MalformedJson,
                    std::string("expected '") + c + "', got byte 0x" + hex_byte(seen));
    }
    ++pos_;
    return true;
}

bool Reader::push_level() {
    if (depth_ == kMaxDepth) {
        return fail(DecodeErrorCode::MalformedJson, "nesting too deep");
    }
    first_[depth_] = true;
    ++depth_;
    return true;
}

bool Reader::begin_object() {
    return expect('{') && push_level();
}

bool Reader::next_member(std::string& key, bool& present) {
    present = false;
    char c = 0;
    if (!peek(c)) {
        return fail(DecodeErrorCode::MalformedJson, "unterminated object");
    }
    if (c == '}') {
        ++pos_;
        --depth_;
        return true;
    }
    if (first_[depth_ - 1]) {
        first_[depth_ - 1] = false;
    } else if (!expect(',')) {
        return false;
    }
    if (!scan_string(key) || !expect(':')) {
        return false;
    }
    present = true;
    return true;
}

bool Reader::begin_array() {
    return expect('[') && push_level();
}

bool Reader::next_element(bool& present) {
    present = false;
    char c = 0;
    if (!peek(c)) {
        return fail(DecodeErrorCode::MalformedJson, "unterminated array");
    }
    if (c == ']') {
        ++pos_;
        --depth_;
        return true;
    }
    if (first_[depth_ - 1]) {
        first_[depth_ - 1] = false;
    } else if (!expect(',')) {
        return false;
    }
    present = true;
    return true;
}

bool Reader::read_string(std::string& out) {
    return scan_string(out);
}

bool Reader::read_int(std::int64_t& out) {
    std::string_view token;
    bool integral = false;
    if (!scan_number(token, integral)) {
        return false;
    }
    if (!integral) {
        return fail(DecodeErrorCode::WrongType, "expected an integer, got " + std::string(token));
    }
    const std::from_chars_result parsed =
        std::from_chars(token.data(), token.data() + token.size(), out);
    if (parsed.ec == std::errc::result_out_of_range) {
        return fail(DecodeErrorCode::OutOfRange, "integer out of range: " + std::string(token));
    }
    if (parsed.ec != std::errc{}) {
        return fail(DecodeErrorCode::MalformedJson, "malformed integer: " + std::string(token));
    }
    return true;
}

bool Reader::read_double(double& out) {
    std::string_view token;
    bool integral = false;
    if (!scan_number(token, integral)) {
        return false;
    }
    const std::from_chars_result parsed =
        std::from_chars(token.data(), token.data() + token.size(), out);
    // Underflow lands here too, where orjson would return a signed zero. That divergence is
    // deliberate and recorded in PORT-FIDELITY.md; distinguishing it costs a second parse on a
    // path no price can reach.
    if (parsed.ec == std::errc::result_out_of_range) {
        return fail(DecodeErrorCode::OutOfRange, "number out of range: " + std::string(token));
    }
    if (parsed.ec != std::errc{}) {
        return fail(DecodeErrorCode::MalformedJson, "malformed number: " + std::string(token));
    }
    return true;
}

bool Reader::read_bool(bool& out) {
    char c = 0;
    if (!peek(c)) {
        return fail(DecodeErrorCode::WrongType, "expected a boolean, got end of input");
    }
    if (starts_with(text_, pos_, "true")) {
        pos_ += 4;
        out = true;
        return true;
    }
    if (starts_with(text_, pos_, "false")) {
        pos_ += 5;
        out = false;
        return true;
    }
    return fail(DecodeErrorCode::WrongType, "expected a boolean");
}

bool Reader::read_null() {
    if (!at_null()) {
        return fail(DecodeErrorCode::WrongType, "expected null");
    }
    pos_ += 4;
    return true;
}

bool Reader::at_null() noexcept {
    skip_whitespace();
    return starts_with(text_, pos_, "null");
}

bool Reader::at_document_end() {
    skip_whitespace();
    if (pos_ != text_.size()) {
        return fail(DecodeErrorCode::MalformedJson, "unexpected content after document");
    }
    return true;
}

bool Reader::scan_number(std::string_view& token, bool& integral) {
    char c = 0;
    if (!peek(c)) {
        return fail(DecodeErrorCode::WrongType, "expected a number, got end of input");
    }
    if (c != '-' && !is_digit(c)) {
        return fail(DecodeErrorCode::WrongType, "expected a number");
    }

    const std::size_t start = pos_;
    integral = true;
    if (c == '-') {
        ++pos_;
    }
    if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
        return fail(DecodeErrorCode::MalformedJson, "no digit after sign");
    }
    if (text_[pos_] == '0') {
        ++pos_;
        if (pos_ < text_.size() && is_digit(text_[pos_])) {
            return fail(DecodeErrorCode::MalformedJson, "number with leading zero is not allowed");
        }
    } else {
        while (pos_ < text_.size() && is_digit(text_[pos_])) {
            ++pos_;
        }
    }

    if (pos_ < text_.size() && text_[pos_] == '.') {
        integral = false;
        ++pos_;
        if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
            return fail(DecodeErrorCode::MalformedJson, "no digit after decimal point");
        }
        while (pos_ < text_.size() && is_digit(text_[pos_])) {
            ++pos_;
        }
    }

    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
        integral = false;
        ++pos_;
        if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
            ++pos_;
        }
        if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
            return fail(DecodeErrorCode::MalformedJson, "no digit after exponent");
        }
        while (pos_ < text_.size() && is_digit(text_[pos_])) {
            ++pos_;
        }
    }

    token = text_.substr(start, pos_ - start);
    return true;
}

bool Reader::scan_string(std::string& out) {
    char c = 0;
    if (!peek(c)) {
        return fail(DecodeErrorCode::WrongType, "expected a string, got end of input");
    }
    if (c != '"') {
        return fail(DecodeErrorCode::WrongType, "expected a string");
    }
    ++pos_;

    out.clear();
    std::size_t run = pos_;
    while (true) {
        if (pos_ >= text_.size()) {
            return fail(DecodeErrorCode::MalformedJson, "unterminated string");
        }
        const auto byte = static_cast<unsigned char>(text_[pos_]);
        if (byte == '"') {
            out.append(text_.substr(run, pos_ - run));
            ++pos_;
            return true;
        }
        if (byte == '\\') {
            out.append(text_.substr(run, pos_ - run));
            ++pos_;
            if (!append_escape(out)) {
                return false;
            }
            run = pos_;
            continue;
        }
        if (byte < 0x20U) {
            return fail(DecodeErrorCode::MalformedJson, "unexpected control character in string");
        }
        if (byte < 0x80U) {
            ++pos_;
            continue;
        }
        if (!validate_utf8_run(pos_)) {
            return false;
        }
    }
}

bool Reader::append_escape(std::string& out) {
    if (pos_ >= text_.size()) {
        return fail(DecodeErrorCode::MalformedJson, "unterminated escape sequence");
    }
    const char c = text_[pos_];
    ++pos_;
    switch (c) {
        case '"':
        case '\\':
        case '/':
            out.push_back(c);
            return true;
        case 'b':
            out.push_back('\b');
            return true;
        case 'f':
            out.push_back('\f');
            return true;
        case 'n':
            out.push_back('\n');
            return true;
        case 'r':
            out.push_back('\r');
            return true;
        case 't':
            out.push_back('\t');
            return true;
        case 'u':
            return append_code_point(out);
        default:
            return fail(DecodeErrorCode::MalformedJson,
                        std::string("invalid escape: byte 0x") + hex_byte(c));
    }
}

bool Reader::append_code_point(std::string& out) {
    const auto read_hex4 = [this](std::uint32_t& value) {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text_[pos_ + i];
            std::uint32_t digit = 0;
            if (is_digit(c)) {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A') + 10;
            } else {
                return false;
            }
            value = (value << 4U) | digit;
        }
        pos_ += 4;
        return true;
    };

    std::uint32_t code_point = 0;
    if (!read_hex4(code_point)) {
        return fail(DecodeErrorCode::MalformedJson, "invalid \\u escape");
    }

    if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
        return fail(DecodeErrorCode::MalformedJson, "no high surrogate in string");
    }
    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
        std::uint32_t low = 0;
        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
            return fail(DecodeErrorCode::MalformedJson, "no low surrogate in string");
        }
        pos_ += 2;
        if (!read_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
            return fail(DecodeErrorCode::MalformedJson, "no low surrogate in string");
        }
        code_point = 0x10000 + ((code_point - 0xD800) << 10U) + (low - 0xDC00);
    }

    append_utf8(out, code_point);
    return true;
}

// Rejects the sequences a UTF-8 decoder must reject and not merely survive: overlong encodings, the
// surrogate range, and anything past U+10FFFF. orjson raises on all three, and a decoder that
// accepted them here would hand the executor a correlation_id the poller cannot have sent.
bool Reader::validate_utf8_run(std::size_t& index) {
    const auto byte_at = [this](std::size_t i) { return static_cast<unsigned char>(text_[i]); };
    const auto is_continuation = [&byte_at](std::size_t i, unsigned char low, unsigned char high) {
        return byte_at(i) >= low && byte_at(i) <= high;
    };

    const unsigned char lead = byte_at(index);
    std::size_t length = 0;
    unsigned char second_low = 0x80;
    unsigned char second_high = 0xBF;

    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) {
            second_low = 0xA0;
        } else if (lead == 0xED) {
            second_high = 0x9F;
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) {
            second_low = 0x90;
        } else if (lead == 0xF4) {
            second_high = 0x8F;
        }
    } else {
        return fail(DecodeErrorCode::MalformedJson, "str is not valid UTF-8");
    }

    if (index + length > text_.size()) {
        return fail(DecodeErrorCode::MalformedJson, "str is not valid UTF-8");
    }
    if (!is_continuation(index + 1, second_low, second_high)) {
        return fail(DecodeErrorCode::MalformedJson, "str is not valid UTF-8");
    }
    for (std::size_t i = 2; i < length; ++i) {
        if (!is_continuation(index + i, 0x80, 0xBF)) {
            return fail(DecodeErrorCode::MalformedJson, "str is not valid UTF-8");
        }
    }

    index += length;
    return true;
}

}  // namespace hotpath::json
