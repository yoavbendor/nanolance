// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Databend's JSONB, as the `jsonb` crate (0.5, features "databend" only, as Lance builds it) writes
// and reads it:
//
//   container  = u32 header (BE): 0x80000000 | n (array), 0x40000000 | n (object),
//                0x20000000 (a scalar at the top level), then the JEntries (BE u32: type in
//                0x70000000, length in 0x0FFFFFFF), then the data, in entry order. An object has n
//                key entries (strings, in byte order: the crate's objects are BTreeMaps) and then n
//                value entries.
//   JEntry type: 0x00 null, 0x10 string, 0x20 number, 0x30 false, 0x40 true, 0x50 container,
//                0x60 extension.
//   number     = 0x00 zero | 0x10 NaN | 0x20 inf | 0x30 -inf | 0x40 int (i8/i16/i32/i64 BE, the
//                smallest that fits) | 0x50 uint (u8..u64 likewise) | 0x60 float (f64 BE) |
//                0x70 decimal (value BE, then scale).
//
// Without the crate's arbitrary_precision feature, an integer literal is a u64 when it fits, else an
// i64 when negative and fitting, else a float; anything with a fraction or an exponent is a float.

#include <nanolance/jsonb.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace nano_lance::jsonb {
namespace {

constexpr std::uint32_t kArray = 0x80000000U;
constexpr std::uint32_t kObject = 0x40000000U;
constexpr std::uint32_t kScalar = 0x20000000U;
constexpr std::uint32_t kHeaderType = 0xE0000000U;
constexpr std::uint32_t kHeaderLen = 0x1FFFFFFFU;
constexpr std::uint32_t kEntryIsOffset = 0x80000000U;
constexpr std::uint32_t kEntryType = 0x70000000U;
constexpr std::uint32_t kEntryLen = 0x0FFFFFFFU;
constexpr std::uint32_t kNullTag = 0x00000000U;
constexpr std::uint32_t kStringTag = 0x10000000U;
constexpr std::uint32_t kNumberTag = 0x20000000U;
constexpr std::uint32_t kFalseTag = 0x30000000U;
constexpr std::uint32_t kTrueTag = 0x40000000U;
constexpr std::uint32_t kContainerTag = 0x50000000U;

std::uint32_t read_be32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<std::uint32_t>(u[0]) << 24U) | (static_cast<std::uint32_t>(u[1]) << 16U) |
           (static_cast<std::uint32_t>(u[2]) << 8U) | static_cast<std::uint32_t>(u[3]);
}

void put_be32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v >> 24U));
    out.push_back(static_cast<char>(v >> 16U));
    out.push_back(static_cast<char>(v >> 8U));
    out.push_back(static_cast<char>(v));
}

void set_be32(std::string& out, std::size_t at, std::uint32_t v) {
    out[at] = static_cast<char>(v >> 24U);
    out[at + 1] = static_cast<char>(v >> 16U);
    out[at + 2] = static_cast<char>(v >> 8U);
    out[at + 3] = static_cast<char>(v);
}

template <typename T>
void put_be(std::string& out, T v) {
    using U = std::make_unsigned_t<T>;
    const auto u = static_cast<U>(v);
    for (int shift = static_cast<int>(sizeof(T) * 8) - 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>(u >> shift));
    }
}

// ── parsing (the crate's lenient mode) ─────────────────────────────────────────────────────────────

struct Value {
    enum class Kind { Null, False, True, Number, String, Array, Object } kind = Kind::Null;
    Number number;
    std::string text;
    std::vector<Value> items;
    std::map<std::string, Value> members;  // byte order, as the crate's BTreeMap
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse(Value& out, std::string& error) {
        skip();
        if (!value(out, error, 0)) {
            return false;
        }
        skip();
        if (i_ != s_.size()) {
            return fail("trailing characters", error);
        }
        return true;
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;

    bool fail(const std::string& why, std::string& error) const {
        error = why + ", pos " + std::to_string(i_ + 1);
        return false;
    }
    void skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }
    bool word(std::string_view w) {
        if (s_.substr(i_, w.size()) == w) {
            i_ += w.size();
            return true;
        }
        return false;
    }

    bool value(Value& out, std::string& error, int depth) {
        if (depth > 512) {
            return fail("recursion limit exceeded", error);
        }
        if (i_ >= s_.size()) {
            return fail("EOF while parsing a value", error);
        }
        const char c = s_[i_];
        if (c == '{') {
            return object(out, error, depth);
        }
        if (c == '[') {
            return array(out, error, depth);
        }
        if (c == '"' || c == '\'') {
            out.kind = Value::Kind::String;
            return string(out.text, error);
        }
        if (word("null")) {
            out.kind = Value::Kind::Null;
            return true;
        }
        if (word("true")) {
            out.kind = Value::Kind::True;
            return true;
        }
        if (word("false")) {
            out.kind = Value::Kind::False;
            return true;
        }
        out.kind = Value::Kind::Number;
        return number(out.number, error);
    }

    bool object(Value& out, std::string& error, int depth) {
        out.kind = Value::Kind::Object;
        ++i_;
        skip();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skip();
            std::string key;
            if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
                if (!string(key, error)) {
                    return false;
                }
            } else {
                // An unquoted key: letters, digits, '_' and '$' (and any non-ASCII byte).
                const auto start = i_;
                while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) != 0 || s_[i_] == '_' ||
                                          s_[i_] == '$' || static_cast<unsigned char>(s_[i_]) >= 0x80)) {
                    ++i_;
                }
                if (i_ == start) {
                    return fail("object attribute name cannot be invalid char", error);
                }
                key.assign(s_.substr(start, i_ - start));
            }
            skip();
            if (i_ >= s_.size() || s_[i_] != ':') {
                return fail("expected `:`", error);
            }
            ++i_;
            skip();
            Value v;
            if (!value(v, error, depth + 1)) {
                return false;
            }
            if (out.members.count(key) != 0U) {
                return fail("duplicate object attribute \"" + key + "\"", error);
            }
            out.members.emplace(std::move(key), std::move(v));
            skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("expected `,` or `}`", error);
        }
    }

    bool array(Value& out, std::string& error, int depth) {
        out.kind = Value::Kind::Array;
        ++i_;
        skip();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            skip();
            Value v;
            if (!value(v, error, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(v));
            skip();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("expected `,` or `]`", error);
        }
    }

    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    bool hex4(std::uint32_t& out, std::string& error) {
        if (i_ + 4 > s_.size()) {
            return fail("EOF while parsing a string", error);
        }
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4U;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid unicode code point", error);
            }
        }
        return true;
    }

    bool string(std::string& out, std::string& error) {
        const char quote = s_[i_++];
        for (;;) {
            if (i_ >= s_.size()) {
                return fail("EOF while parsing a string", error);
            }
            const char c = s_[i_++];
            if (c == quote) {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) {
                return fail("EOF while parsing a string", error);
            }
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\'': out.push_back('\''); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp, error)) {
                        return false;
                    }
                    if (cp >= 0xD800U && cp <= 0xDBFFU && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        const auto save = i_;
                        i_ += 2;
                        std::uint32_t low = 0;
                        if (hex4(low, error) && low >= 0xDC00U && low <= 0xDFFFU) {
                            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
                        } else {
                            i_ = save;
                        }
                    }
                    utf8(out, cp);
                    break;
                }
                default: {
                    char hex[3];
                    std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned>(static_cast<unsigned char>(e)));
                    --i_;
                    return fail(std::string("invalid escaped '") + hex + "'", error);
                }
            }
        }
    }

    bool number(Number& out, std::string& error) {
        const auto start = i_;
        bool negative = false;
        if (s_[i_] == '+' || s_[i_] == '-') {
            negative = s_[i_] == '-';
            ++i_;
        }
        if (word("NaN")) {
            out = {Number::Kind::Float, 0, 0, std::numeric_limits<double>::quiet_NaN()};
            return true;
        }
        if (word("Infinity")) {
            out = {Number::Kind::Float, 0, 0, negative ? -INFINITY : INFINITY};
            return true;
        }
        // Hexadecimal integers: 0x...
        if (i_ + 1 < s_.size() && s_[i_] == '0' && (s_[i_ + 1] == 'x' || s_[i_ + 1] == 'X')) {
            i_ += 2;
            const auto digits = i_;
            while (i_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[i_])) != 0) {
                ++i_;
            }
            std::uint64_t v = 0;
            const auto [p, ec] = std::from_chars(s_.data() + digits, s_.data() + i_, v, 16);
            if (i_ == digits || ec != std::errc()) {
                return fail("invalid number", error);
            }
            if (!negative) {
                out = {Number::Kind::UInt, 0, v, 0};
            } else if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U) {
                out = {Number::Kind::Int, static_cast<std::int64_t>(0 - v), 0, 0};
            } else {
                out = {Number::Kind::Float, 0, 0, -static_cast<double>(v)};
            }
            return true;
        }
        const auto int_start = i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_])) != 0) {
            ++i_;
        }
        const auto int_digits = i_ - int_start;
        bool fraction = false;
        bool exponent = false;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            const auto f = i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_])) != 0) {
                ++i_;
            }
            // "123." is the integer 123 to the crate; ".5" is a float.
            fraction = i_ > f;
            if (i_ == f && int_digits == 0) {
                i_ = start;
                return fail("expected value", error);
            }
        }
        if (int_digits == 0 && !fraction) {
            i_ = start;
            return fail("expected value", error);
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
                ++i_;
            }
            const auto e = i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_])) != 0) {
                ++i_;
            }
            if (i_ == e) {
                return fail("invalid number", error);
            }
            exponent = true;
        }
        if (!fraction && !exponent) {
            std::uint64_t v = 0;
            const auto [p, ec] = std::from_chars(s_.data() + int_start, s_.data() + int_start + int_digits, v);
            if (ec == std::errc()) {
                if (!negative) {
                    out = {Number::Kind::UInt, 0, v, 0};
                    return true;
                }
                if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U) {
                    out = {Number::Kind::Int, static_cast<std::int64_t>(0 - v), 0, 0};
                    return true;
                }
            }
        }
        // A float: from the digits, with the sign (a leading '+' is not something strtod takes).
        std::string digits(s_.substr(int_start, i_ - int_start));
        if (!digits.empty() && digits.front() == '.') {
            digits.insert(digits.begin(), '0');
        }
        if (!digits.empty() && digits.back() == '.') {
            digits.push_back('0');
        }
        double v = 0;
        const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
        if (ec != std::errc() && ec != std::errc::result_out_of_range) {
            return fail("invalid number", error);
        }
        if (ec == std::errc::result_out_of_range) {
            v = std::strtod(digits.c_str(), nullptr);
        }
        out = {Number::Kind::Float, 0, 0, negative ? -v : v};
        return true;
    }
};

// ── encoding (the crate's Encoder) ─────────────────────────────────────────────────────────────────

void encode_number(std::string& out, const Number& n) {
    if (n.kind == Number::Kind::Float) {
        if (std::isnan(n.f)) {
            out.push_back(0x10);
        } else if (std::isinf(n.f)) {
            out.push_back(n.f < 0 ? 0x30 : 0x20);
        } else {
            out.push_back(0x60);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &n.f, sizeof(bits));
            put_be(out, bits);
        }
        return;
    }
    if (n.kind == Number::Kind::UInt) {
        const auto v = n.u;
        if (v == 0U) {
            out.push_back(0x00);
        } else {
            out.push_back(0x50);
            if (v <= 0xFFU) {
                put_be(out, static_cast<std::uint8_t>(v));
            } else if (v <= 0xFFFFU) {
                put_be(out, static_cast<std::uint16_t>(v));
            } else if (v <= 0xFFFFFFFFU) {
                put_be(out, static_cast<std::uint32_t>(v));
            } else {
                put_be(out, v);
            }
        }
        return;
    }
    const auto v = n.i;
    if (v == 0) {
        out.push_back(0x00);
        return;
    }
    out.push_back(0x40);
    if (v >= INT8_MIN && v <= INT8_MAX) {
        put_be(out, static_cast<std::int8_t>(v));
    } else if (v >= INT16_MIN && v <= INT16_MAX) {
        put_be(out, static_cast<std::int16_t>(v));
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
        put_be(out, static_cast<std::int32_t>(v));
    } else {
        put_be(out, v);
    }
}

std::size_t encode_container(std::string& out, const Value& v);

/// Writes a value's data and returns its JEntry.
std::uint32_t encode_entry(std::string& out, const Value& v) {
    const auto start = out.size();
    switch (v.kind) {
        case Value::Kind::Null: return kNullTag;
        case Value::Kind::True: return kTrueTag;
        case Value::Kind::False: return kFalseTag;
        case Value::Kind::Number:
            encode_number(out, v.number);
            return kNumberTag | static_cast<std::uint32_t>(out.size() - start);
        case Value::Kind::String:
            out += v.text;
            return kStringTag | static_cast<std::uint32_t>(v.text.size());
        default:
            return kContainerTag | static_cast<std::uint32_t>(encode_container(out, v));
    }
}

std::size_t encode_container(std::string& out, const Value& v) {
    const auto start = out.size();
    if (v.kind == Value::Kind::Array) {
        put_be32(out, kArray | static_cast<std::uint32_t>(v.items.size()));
        auto entry = out.size();
        out.resize(out.size() + 4 * v.items.size());
        for (const auto& item : v.items) {
            set_be32(out, entry, encode_entry(out, item));
            entry += 4;
        }
    } else if (v.kind == Value::Kind::Object) {
        put_be32(out, kObject | static_cast<std::uint32_t>(v.members.size()));
        auto entry = out.size();
        out.resize(out.size() + 8 * v.members.size());
        for (const auto& [key, value] : v.members) {
            out += key;
            set_be32(out, entry, kStringTag | static_cast<std::uint32_t>(key.size()));
            entry += 4;
        }
        for (const auto& [key, value] : v.members) {
            set_be32(out, entry, encode_entry(out, value));
            entry += 4;
        }
    } else {
        put_be32(out, kScalar);
        const auto entry = out.size();
        out.resize(out.size() + 4);
        set_be32(out, entry, encode_entry(out, v));
    }
    return out.size() - start;
}

// ── printing (RawJsonb::to_string) ─────────────────────────────────────────────────────────────────

void print_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

/// A finite double in shortest round-trip form, laid out as zmij (Lance's float printer) does:
/// positional for decimal exponents -5..15 (with ".0" when integral), else d[.ddd]e(+|-)x.
void print_double(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[64];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    std::string sci(buf, end);  // [-]d[.ddd]e[+-]xx
    std::string sign;
    if (sci.front() == '-') {
        sign = "-";
        sci.erase(0, 1);
    }
    const auto e_pos = sci.find('e');
    std::string mantissa = sci.substr(0, e_pos);
    const int exponent = std::stoi(sci.substr(e_pos + 1));
    std::string digits = mantissa;
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    out += sign;
    if (exponent >= -5 && exponent <= 15) {
        if (exponent >= 0) {
            const auto int_len = static_cast<std::size_t>(exponent) + 1U;
            if (digits.size() <= int_len) {
                out += digits;
                out.append(int_len - digits.size(), '0');
                out += ".0";
            } else {
                out += digits.substr(0, int_len);
                out.push_back('.');
                out += digits.substr(int_len);
            }
        } else {
            out += "0.";
            out.append(static_cast<std::size_t>(-exponent - 1), '0');
            out += digits;
        }
        return;
    }
    out += digits.substr(0, 1);
    if (digits.size() > 1U) {
        out.push_back('.');
        out += digits.substr(1);
    }
    out.push_back('e');
    out += exponent >= 0 ? "+" : "-";
    out += std::to_string(std::abs(exponent));
}

bool decode_number(std::string_view p, Number& out) {
    if (p.empty()) {
        return false;
    }
    const auto ty = static_cast<unsigned char>(p[0]);
    const auto len = p.size() - 1U;
    const char* d = p.data() + 1;
    auto be = [&](std::size_t n) {
        std::uint64_t v = 0;
        for (std::size_t k = 0; k < n; ++k) {
            v = (v << 8U) | static_cast<unsigned char>(d[k]);
        }
        return v;
    };
    switch (ty) {
        case 0x00: out = {Number::Kind::UInt, 0, 0, 0}; return true;
        case 0x10: out = {Number::Kind::Float, 0, 0, std::numeric_limits<double>::quiet_NaN()}; return true;
        case 0x20: out = {Number::Kind::Float, 0, 0, INFINITY}; return true;
        case 0x30: out = {Number::Kind::Float, 0, 0, -INFINITY}; return true;
        case 0x40: {
            if (len != 1 && len != 2 && len != 4 && len != 8) {
                return false;
            }
            const auto raw = be(len);
            std::int64_t v = 0;
            if (len == 1) v = static_cast<std::int8_t>(raw);
            else if (len == 2) v = static_cast<std::int16_t>(raw);
            else if (len == 4) v = static_cast<std::int32_t>(raw);
            else v = static_cast<std::int64_t>(raw);
            out = {Number::Kind::Int, v, 0, 0};
            return true;
        }
        case 0x50:
            if (len != 1 && len != 2 && len != 4 && len != 8) {
                return false;
            }
            out = {Number::Kind::UInt, 0, be(len), 0};
            return true;
        case 0x60: {
            if (len != 8) {
                return false;
            }
            const auto bits = be(8);
            double f = 0;
            std::memcpy(&f, &bits, sizeof(f));
            out = {Number::Kind::Float, 0, 0, f};
            return true;
        }
        case 0x70: {
            // A decimal (value, scale): read as a float, which is all the json_* functions need.
            if (len != 9) {
                return false;  // 128- and 256-bit decimals: not something Lance writes
            }
            const auto value = static_cast<std::int64_t>(be(8));
            const auto scale = static_cast<unsigned char>(d[8]);
            out = {Number::Kind::Float, 0, 0, static_cast<double>(value) / std::pow(10.0, scale)};
            return true;
        }
        default: return false;
    }
}

void print_number(std::string& out, const Number& n) {
    if (n.kind == Number::Kind::Int) {
        out += std::to_string(n.i);
    } else if (n.kind == Number::Kind::UInt) {
        out += std::to_string(n.u);
    } else {
        print_double(out, n.f);
    }
}

}  // namespace

bool encode(std::string_view text, std::string& out, std::string& error) {
    Value v;
    Parser parser(text);
    if (!parser.parse(v, error)) {
        return false;
    }
    out.clear();
    encode_container(out, v);
    return true;
}

// ── Item ───────────────────────────────────────────────────────────────────────────────────────────

bool Item::root(std::string_view jsonb, Item& out) {
    if (jsonb.size() < 4U) {
        return false;
    }
    const auto header = read_be32(jsonb.data());
    out = Item{};
    out.count_ = header & kHeaderLen;
    switch (header & kHeaderType) {
        case kArray:
            out.type_ = Type::Array;
            out.payload_ = jsonb;
            return jsonb.size() >= 4U + 4U * out.count_;
        case kObject:
            out.type_ = Type::Object;
            out.payload_ = jsonb;
            return jsonb.size() >= 4U + 8U * out.count_;
        case kScalar: {
            Item container;
            container.payload_ = jsonb;
            container.count_ = 1;
            return jsonb.size() >= 8U && container.entry(0, 1, out);
        }
        default:
            return false;
    }
}

/// Entry `index` of this container (whose header announces `entries` JEntries).
bool Item::entry(std::size_t index, std::size_t entries, Item& out) const {
    const char* base = payload_.data();
    const std::size_t data_start = 4U + 4U * entries;
    if (index >= entries || payload_.size() < data_start) {
        return false;
    }
    // The data of entry k starts after the data of entries 0..k-1 (lengths, or end offsets).
    std::size_t offset = data_start;
    std::uint32_t raw = 0;
    for (std::size_t k = 0; k <= index; ++k) {
        raw = read_be32(base + 4U + 4U * k);
        if (k == index) {
            break;
        }
        offset = (raw & kEntryIsOffset) != 0U ? data_start + (raw & kEntryLen) : offset + (raw & kEntryLen);
    }
    const std::size_t end = (raw & kEntryIsOffset) != 0U ? data_start + (raw & kEntryLen) : offset + (raw & kEntryLen);
    if (end > payload_.size() || end < offset) {
        return false;
    }
    const auto data = payload_.substr(offset, end - offset);
    out = Item{};
    out.tag_ = raw & kEntryType;
    switch (out.tag_) {
        case kNullTag: out.type_ = Type::Null; return true;
        case kTrueTag: out.type_ = Type::True; return true;
        case kFalseTag: out.type_ = Type::False; return true;
        case kStringTag: out.type_ = Type::String; out.payload_ = data; return true;
        case kNumberTag: out.type_ = Type::Number; out.payload_ = data; return true;
        case kContainerTag: return root(data, out);
        default: out.type_ = Type::Other; out.payload_ = data; return true;
    }
}

bool Item::at(std::size_t index, Item& out) const {
    return type_ == Type::Array && entry(index, count_, out);
}

bool Item::key_at(std::size_t index, std::string_view& out) const {
    Item key;
    if (type_ != Type::Object || !entry(index, 2U * count_, key) || key.type_ != Type::String) {
        return false;
    }
    out = key.payload_;
    return true;
}

bool Item::value_at(std::size_t index, Item& out) const {
    return type_ == Type::Object && index < count_ && entry(count_ + index, 2U * count_, out);
}

bool Item::get(std::string_view key, Item& out) const {
    if (type_ != Type::Object) {
        return false;
    }
    for (std::size_t k = 0; k < count_; ++k) {
        std::string_view name;
        if (key_at(k, name) && name == key) {
            return value_at(k, out);
        }
    }
    return false;
}

bool Item::number(Number& out) const {
    return type_ == Type::Number && decode_number(payload_, out);
}

std::string Item::document() const {
    if (type_ == Type::Array || type_ == Type::Object) {
        return std::string(payload_);
    }
    std::string out;
    put_be32(out, kScalar);
    put_be32(out, tag_ | static_cast<std::uint32_t>(payload_.size()));
    out += payload_;
    return out;
}

bool Item::text(std::string& out, std::string& error) const {
    switch (type_) {
        case Type::Null: out += "null"; return true;
        case Type::True: out += "true"; return true;
        case Type::False: out += "false"; return true;
        case Type::String: print_string(out, payload_); return true;
        case Type::Number: {
            Number n;
            if (!number(n)) {
                error = "invalid JSONB number";
                return false;
            }
            print_number(out, n);
            return true;
        }
        case Type::Array: {
            out.push_back('[');
            for (std::size_t k = 0; k < count_; ++k) {
                Item item;
                if (!at(k, item)) {
                    error = "invalid JSONB array";
                    return false;
                }
                if (k > 0U) {
                    out.push_back(',');
                }
                if (!item.text(out, error)) {
                    return false;
                }
            }
            out.push_back(']');
            return true;
        }
        case Type::Object: {
            out.push_back('{');
            for (std::size_t k = 0; k < count_; ++k) {
                std::string_view key;
                Item value;
                if (!key_at(k, key) || !value_at(k, value)) {
                    error = "invalid JSONB object";
                    return false;
                }
                if (k > 0U) {
                    out.push_back(',');
                }
                print_string(out, key);
                out.push_back(':');
                if (!value.text(out, error)) {
                    return false;
                }
            }
            out.push_back('}');
            return true;
        }
        default:
            error = "JSONB extension values (binary, date, timestamp, interval) are not supported";
            return false;
    }
}

bool to_text(std::string_view jsonb, std::string& out, std::string& error) {
    Item root;
    if (!Item::root(jsonb, root)) {
        error = "invalid JSONB value";
        return false;
    }
    out.clear();
    return root.text(out, error);
}

// ── JSONPath ───────────────────────────────────────────────────────────────────────────────────────

bool select(const Item& root, std::string_view path, std::vector<Item>& out, std::string& error) {
    out.clear();
    std::size_t i = 0;
    auto skip = [&] {
        while (i < path.size() && path[i] == ' ') {
            ++i;
        }
    };
    skip();
    if (i >= path.size() || path[i] != '$') {
        error = "Invalid JSONPath '" + std::string(path) + "': a path starts with $";
        return false;
    }
    ++i;
    std::vector<Item> current = {root};
    while (true) {
        skip();
        if (i >= path.size()) {
            break;
        }
        std::vector<Item> next;
        auto by_key = [&](std::string_view key) {
            for (const auto& item : current) {
                Item v;
                if (item.get(key, v)) {
                    next.push_back(v);
                }
            }
        };
        auto all = [&] {
            for (const auto& item : current) {
                for (std::size_t k = 0; k < item.size(); ++k) {
                    Item v;
                    if ((item.type() == Item::Type::Array && item.at(k, v)) ||
                        (item.type() == Item::Type::Object && item.value_at(k, v))) {
                        next.push_back(v);
                    }
                }
            }
        };
        if (path[i] == '.') {
            ++i;
            if (i < path.size() && path[i] == '*') {
                ++i;
                all();
            } else {
                const auto start = i;
                while (i < path.size() && path[i] != '.' && path[i] != '[' && path[i] != ' ') {
                    ++i;
                }
                if (i == start) {
                    error = "Invalid JSONPath '" + std::string(path) + "'";
                    return false;
                }
                std::string_view key = path.substr(start, i - start);
                if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
                    key = key.substr(1, key.size() - 2);
                }
                by_key(key);
            }
        } else if (path[i] == '[') {
            ++i;
            skip();
            if (i < path.size() && path[i] == '*') {
                ++i;
                all();
            } else if (i < path.size() && (path[i] == '\'' || path[i] == '"')) {
                const char quote = path[i++];
                const auto start = i;
                while (i < path.size() && path[i] != quote) {
                    ++i;
                }
                by_key(path.substr(start, i - start));
                ++i;
            } else {
                const auto start = i;
                bool negative = false;
                if (i < path.size() && path[i] == '-') {
                    negative = true;
                    ++i;
                }
                std::size_t index = 0;
                const auto [p, ec] = std::from_chars(path.data() + i, path.data() + path.size(), index);
                if (ec != std::errc()) {
                    error = "Invalid JSONPath '" + std::string(path) + "'";
                    return false;
                }
                i = static_cast<std::size_t>(p - path.data());
                (void)start;
                for (const auto& item : current) {
                    Item v;
                    if (item.type() == Item::Type::Array) {
                        if (negative ? index >= 1U && index <= item.size() && item.at(item.size() - index, v)
                                     : item.at(index, v)) {
                            next.push_back(v);
                        }
                    }
                }
            }
            skip();
            if (i >= path.size() || path[i] != ']') {
                error = "Invalid JSONPath '" + std::string(path) + "': expected ']'";
                return false;
            }
            ++i;
        } else {
            error = "Invalid JSONPath '" + std::string(path) + "'";
            return false;
        }
        current = std::move(next);
    }
    out = std::move(current);
    return true;
}

}  // namespace nano_lance::jsonb
