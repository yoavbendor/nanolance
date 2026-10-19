// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// SQL expressions over Arrow batches: a small recursive-descent parser for the filter dialect Lance
// uses (see expr.hpp) and a row-at-a-time evaluator over nanoarrow array views. Correctness first:
// the evaluator walks the tree per row, which costs tens of nanoseconds a row -- small next to the
// decode that produced the batch.

#include "nanolance/expr.hpp"
#include "nanolance/jsonb.hpp"
#include "float_text.hpp"
#include "int128.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace nano_lance::expr {

// ── values ──────────────────────────────────────────────────────────────────────────────────────

namespace detail {

enum class Kind { Null, Bool, Int, UInt, Double, String, Binary, Date, Timestamp, List, Decimal, Duration };

/// A decimal's unscaled value: 256-bit two's complement, least significant 64-bit limb first.
using Limbs256 = std::array<std::uint64_t, 4>;

/// An unsigned integer of 1024 bits (32-bit limbs, least significant first): exact decimal
/// arithmetic on a 256-bit value rescaled by up to 10^160 stays inside it.
struct Magnitude {
    std::array<std::uint32_t, 32> w{};
};

void mag_mul_small(Magnitude& m, std::uint32_t f) {
    std::uint64_t carry = 0;
    for (auto& limb : m.w) {
        const std::uint64_t t = static_cast<std::uint64_t>(limb) * f + carry;
        limb = static_cast<std::uint32_t>(t);
        carry = t >> 32;
    }
}

void mag_add_small(Magnitude& m, std::uint32_t f) {
    std::uint64_t carry = f;
    for (auto& limb : m.w) {
        const std::uint64_t t = static_cast<std::uint64_t>(limb) + carry;
        limb = static_cast<std::uint32_t>(t);
        carry = t >> 32;
    }
}

/// Divides by d (at most 2^32) in place; returns the remainder.
std::uint32_t mag_div_small(Magnitude& m, std::uint32_t d) {
    std::uint64_t rem = 0;
    for (auto it = m.w.rbegin(); it != m.w.rend(); ++it) {
        const std::uint64_t cur = (rem << 32) | *it;
        *it = static_cast<std::uint32_t>(cur / d);
        rem = cur % d;
    }
    return static_cast<std::uint32_t>(rem);
}

std::uint32_t pow10_small(int k) {
    std::uint32_t p = 1;
    for (int i = 0; i < k; ++i) {
        p *= 10;
    }
    return p;
}

void mag_mul_pow10(Magnitude& m, int k) {
    for (; k >= 9; k -= 9) {
        mag_mul_small(m, 1000000000U);
    }
    if (k > 0) {
        mag_mul_small(m, pow10_small(k));
    }
}

/// Divides by 10^k, rounding half away from zero (how pylance rounds decimal text and casts).
void mag_div_pow10_round(Magnitude& m, int k) {
    if (k <= 0) {
        return;
    }
    // Floor division composes exactly, so dividing by 10^(k-1) first leaves the dropped digit last.
    int rest = k - 1;
    for (; rest >= 9; rest -= 9) {
        mag_div_small(m, 1000000000U);
    }
    if (rest > 0) {
        mag_div_small(m, pow10_small(rest));
    }
    if (mag_div_small(m, 10) >= 5) {
        mag_add_small(m, 1);
    }
}

int mag_cmp(const Magnitude& a, const Magnitude& b) {
    for (auto i = a.w.size(); i-- > 0;) {
        if (a.w[i] != b.w[i]) {
            return a.w[i] < b.w[i] ? -1 : 1;
        }
    }
    return 0;
}

/// The digits of m (no sign, no point).
std::string mag_digits(Magnitude m) {
    std::vector<std::uint32_t> chunks;
    do {
        chunks.push_back(mag_div_small(m, 1000000000U));
    } while (std::any_of(m.w.begin(), m.w.end(), [](std::uint32_t x) { return x != 0; }));
    std::string out = std::to_string(chunks.back());
    for (auto it = chunks.rbegin() + 1; it != chunks.rend(); ++it) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%09u", static_cast<unsigned>(*it));
        out += buf;
    }
    return out;
}

/// m with `scale` digits after the point, as an arrow decimal shows it ("2.500", "0.999").
std::string mag_text(const Magnitude& m, int scale, bool negative) {
    std::string digits = mag_digits(m);
    if (scale > 0) {
        if (digits.size() <= static_cast<std::size_t>(scale)) {
            digits.insert(0, static_cast<std::size_t>(scale) + 1 - digits.size(), '0');
        }
        digits.insert(digits.size() - static_cast<std::size_t>(scale), ".");
    }
    return (negative ? "-" : "") + digits;
}

bool limbs_negative(const Limbs256& l) { return (l[3] >> 63) != 0; }

Magnitude limbs_magnitude(Limbs256 l) {
    if (limbs_negative(l)) {
        for (auto& x : l) {
            x = ~x;
        }
        for (auto& x : l) {  // +1, with carry
            if (++x != 0) {
                break;
            }
        }
    }
    Magnitude m;
    for (std::size_t k = 0; k < 4; ++k) {
        m.w[2 * k] = static_cast<std::uint32_t>(l[k]);
        m.w[2 * k + 1] = static_cast<std::uint32_t>(l[k] >> 32);
    }
    return m;
}

/// The two's complement limbs of a magnitude below 2^255, negated when `negative`.
Limbs256 magnitude_limbs(const Magnitude& m, bool negative) {
    Limbs256 l{};
    for (std::size_t k = 0; k < 4; ++k) {
        l[k] = static_cast<std::uint64_t>(m.w[2 * k]) | (static_cast<std::uint64_t>(m.w[2 * k + 1]) << 32);
    }
    if (negative) {
        for (auto& x : l) {
            x = ~x;
        }
        for (auto& x : l) {
            if (++x != 0) {
                break;
            }
        }
    }
    return l;
}

/// Parses number text as written ("-2.50", "12", "1e3", "0.0004") into digits and scale: the
/// value is magnitude * 10^-scale, so "2.50" is 250 at scale 2. False when it is not a number
/// this exact (more than 76 digits, or a scale beyond +-160).
bool parse_decimal_text(std::string_view text, Magnitude& m, bool& negative, int& scale) {
    std::size_t p = 0;
    negative = false;
    if (p < text.size() && (text[p] == '-' || text[p] == '+')) {
        negative = text[p] == '-';
        ++p;
    }
    int digits = 0;
    int frac = 0;
    bool point = false;
    m = Magnitude{};
    for (; p < text.size(); ++p) {
        const char c = text[p];
        if (c == '.' && !point) {
            point = true;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 76) {
                return false;
            }
            mag_mul_small(m, 10);
            mag_add_small(m, static_cast<std::uint32_t>(c - '0'));
            frac += point ? 1 : 0;
        } else {
            break;
        }
    }
    if (digits == 0) {
        return false;
    }
    int exponent = 0;
    if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
        ++p;
        bool exp_negative = false;
        if (p < text.size() && (text[p] == '-' || text[p] == '+')) {
            exp_negative = text[p] == '-';
            ++p;
        }
        if (p >= text.size()) {
            return false;
        }
        for (; p < text.size(); ++p) {
            if (text[p] < '0' || text[p] > '9' || exponent > 1000) {
                return false;
            }
            exponent = exponent * 10 + (text[p] - '0');
        }
        exponent = exp_negative ? -exponent : exponent;
    }
    if (p != text.size()) {
        return false;
    }
    scale = frac - exponent;
    if (scale < -160 || scale > 160) {
        return false;
    }
    if (scale < 0) {
        mag_mul_pow10(m, -scale);
        scale = 0;
    }
    return true;
}

/// The double nearest to the decimal (correctly rounded, through strtod on its exact digits).
double decimal_as_double(const Limbs256& limbs, std::int32_t scale) {
    const std::string text = (limbs_negative(limbs) ? "-" : "") + mag_digits(limbs_magnitude(limbs)) + "e" +
                             std::to_string(-static_cast<std::int64_t>(scale));
    return std::strtod(text.c_str(), nullptr);
}

/// `m` (negative when `negative`, with `scale`) as a decimal of `precision` digits: the rounded
/// limbs at `to_scale`, or false with an error worded as arrow's cast error.
bool fit_decimal(Magnitude m, bool negative, int scale, int to_scale, int precision, const char* width,
                 Limbs256& out, std::string& error) {
    if (to_scale >= scale) {
        mag_mul_pow10(m, to_scale - scale);
    } else {
        mag_div_pow10_round(m, scale - to_scale);
    }
    Magnitude limit;
    limit.w[0] = 1;
    mag_mul_pow10(limit, precision);
    if (mag_cmp(m, limit) >= 0) {
        Magnitude nines = limit;  // limit - 1 (a power of ten, so the subtraction never runs out)
        for (auto& x : nines.w) {
            if (x-- != 0) {
                break;
            }
        }
        error = mag_text(m, to_scale, negative) + " is too large to store in a " + width + " of precision " +
                std::to_string(precision) + ". Max is " + mag_text(nines, to_scale, negative);
        return false;
    }
    out = magnitude_limbs(m, negative);
    return true;
}

// ── value ───────────────────────────────────────────────────

/// One value of an expression. Strings view either the batch's buffers, a literal, or `own`.
struct Value {
    Kind kind = Kind::Null;
    bool b = false;
    std::int64_t i = 0;   // Int; Date (days); Timestamp (nanoseconds)
    std::uint64_t u = 0;  // UInt
    double d = 0.0;       // Double
    std::string_view s;   // String / Binary
    std::shared_ptr<std::string> own;
    std::shared_ptr<std::vector<Value>> items;  // List
    Limbs256 dec{};       // Decimal: the unscaled value
    std::int32_t scale = 0;  // Decimal: digits after the point (the value is dec * 10^-scale)
    ArrowTimeUnit unit = NANOARROW_TIME_UNIT_SECOND;  // Duration: the unit of `i`

    static Value decimal(const Limbs256& limbs, std::int32_t scale) {
        Value x;
        x.kind = Kind::Decimal;
        x.dec = limbs;
        x.scale = scale;
        return x;
    }
    /// A duration of `count` units (count is in `i`; the unit is kept beside it).
    static Value duration(std::int64_t count, ArrowTimeUnit unit) {
        Value x;
        x.kind = Kind::Duration;
        x.i = count;
        x.unit = unit;
        return x;
    }

    static Value null() { return {}; }
    static Value boolean(bool v) {
        Value x;
        x.kind = Kind::Bool;
        x.b = v;
        return x;
    }
    static Value integer(std::int64_t v) {
        Value x;
        x.kind = Kind::Int;
        x.i = v;
        return x;
    }
    static Value uinteger(std::uint64_t v) {
        Value x;
        x.kind = Kind::UInt;
        x.u = v;
        return x;
    }
    static Value real(double v) {
        Value x;
        x.kind = Kind::Double;
        x.d = v;
        return x;
    }
    static Value string(std::string v, Kind k = Kind::String) {
        Value x;
        x.kind = k;
        x.own = std::make_shared<std::string>(std::move(v));
        x.s = *x.own;
        return x;
    }
    static Value view(std::string_view v, Kind k = Kind::String) {
        Value x;
        x.kind = k;
        x.s = v;
        return x;
    }
    static Value date(std::int64_t days) {
        Value x;
        x.kind = Kind::Date;
        x.i = days;
        return x;
    }
    static Value timestamp(std::int64_t ns) {
        Value x;
        x.kind = Kind::Timestamp;
        x.i = ns;
        x.unit = NANOARROW_TIME_UNIT_NANO;  // a timestamp column's values carry its unit (temporal_arith)
        return x;
    }
    static Value list(std::vector<Value> values) {
        Value x;
        x.kind = Kind::List;
        x.items = std::make_shared<std::vector<Value>>(std::move(values));
        return x;
    }
    bool is_null() const { return kind == Kind::Null; }
    bool numeric() const { return kind == Kind::Int || kind == Kind::UInt || kind == Kind::Double; }
    bool duration() const { return kind == Kind::Duration; }
    bool temporal() const { return kind == Kind::Date || kind == Kind::Timestamp; }
    double as_double() const {
        switch (kind) {
            case Kind::Int: return static_cast<double>(i);
            case Kind::UInt: return static_cast<double>(u);
            case Kind::Double: return d;
            case Kind::Decimal: return decimal_as_double(dec, scale);
            default: return 0.0;
        }
    }
    std::int64_t as_ns() const { return kind == Kind::Date ? i * 86400LL * 1000000000LL : i; }
};

constexpr std::int64_t kNsPerDay = 86400LL * 1000000000LL;

/// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
inline std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

inline void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

/// "YYYY-MM-DD[( |T)HH:MM[:SS[.fraction]]][Z]" as nanoseconds since the epoch; `date_only` is set
/// when there was no time part.
inline bool parse_datetime(std::string_view text, std::int64_t& ns, bool& date_only) {
    auto digits = [&](std::size_t pos, std::size_t n, int& out) {
        if (pos + n > text.size()) {
            return false;
        }
        out = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const char c = text[pos + k];
            if (c < '0' || c > '9') {
                return false;
            }
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int y = 0;
    int mo = 0;
    int d = 0;
    if (!digits(0, 4, y) || text.size() < 10 || text[4] != '-' || !digits(5, 2, mo) || text[7] != '-' ||
        !digits(8, 2, d) || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return false;
    }
    std::int64_t total = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * kNsPerDay;
    date_only = text.size() == 10;
    if (!date_only) {
        if (text[10] != ' ' && text[10] != 'T') {
            return false;
        }
        int h = 0;
        int mi = 0;
        int s = 0;
        if (!digits(11, 2, h) || text.size() < 16 || text[13] != ':' || !digits(14, 2, mi)) {
            return false;
        }
        std::size_t pos = 16;
        std::int64_t frac_ns = 0;
        if (pos < text.size() && text[pos] == ':') {
            if (!digits(pos + 1, 2, s)) {
                return false;
            }
            pos += 3;
            if (pos < text.size() && text[pos] == '.') {
                ++pos;
                std::int64_t scale = 100000000;
                while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
                    frac_ns += (text[pos] - '0') * scale;
                    scale /= 10;
                    ++pos;
                }
            }
        }
        if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
            ++pos;
        }
        if (pos != text.size()) {
            return false;
        }
        total += (static_cast<std::int64_t>(h) * 3600 + mi * 60 + s) * 1000000000LL + frac_ns;
    }
    ns = total;
    return true;
}

/// The arrow_cast name of a time unit: 'Second', 'Millisecond', ...
inline const char* unit_name(ArrowTimeUnit unit) {
    switch (unit) {
        case NANOARROW_TIME_UNIT_SECOND: return "Second";
        case NANOARROW_TIME_UNIT_MILLI: return "Millisecond";
        case NANOARROW_TIME_UNIT_MICRO: return "Microsecond";
        default: return "Nanosecond";
    }
}

inline std::string format_value(const Value& v) {
    switch (v.kind) {
        case Kind::Null: return "NULL";
        case Kind::Bool: return v.b ? "TRUE" : "FALSE";
        case Kind::Int: return std::to_string(v.i);
        case Kind::UInt: return std::to_string(v.u);
        case Kind::Double: {
            std::ostringstream os;
            os << v.d;
            return os.str();
        }
        case Kind::String:
        case Kind::Binary: {
            std::string out = "'";
            for (const char c : v.s) {
                out += c;
                if (c == '\'') {
                    out += '\'';
                }
            }
            return out + "'";
        }
        case Kind::Date: {
            std::int64_t y = 0;
            unsigned m = 0;
            unsigned d = 0;
            civil_from_days(v.i, y, m, d);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "DATE '%04lld-%02u-%02u'", static_cast<long long>(y), m % 100U, d % 100U);
            return buf;
        }
        case Kind::Timestamp: return "TIMESTAMP_NS(" + std::to_string(v.i) + ")";
        case Kind::List: {
            std::string out = "[";
            for (std::size_t k = 0; k < v.items->size(); ++k) {
                out += (k > 0 ? ", " : "") + format_value((*v.items)[k]);
            }
            return out + "]";
        }
        case Kind::Decimal: return mag_text(limbs_magnitude(v.dec), v.scale, limbs_negative(v.dec));
        case Kind::Duration: return "DURATION(" + std::to_string(v.i) + " " + unit_name(v.unit) + ")";
    }
    return "?";
}

}  // namespace detail

using namespace detail;

// ── the tree ────────────────────────────────────────────────────────────────────────────────────

enum class Op {
    Literal, Column, And, Or, Not, Eq, Ne, Lt, Le, Gt, Ge, IsNull, IsNotNull, IsTrue, IsFalse, IsNotTrue, IsNotFalse,
    In, NotIn, Between, NotBetween, Like, NotLike, ILike, NotILike, Add, Sub, Mul, Div, Mod, Neg, Concat, Func, Cast
};

struct ColumnRef {
    static constexpr std::int64_t kNoElement = INT64_MIN;
    std::vector<std::string> path;  // as written: "s.child" is {"s", "child"}; "l[1]" is {"l", "[1]"}
    // List-element steps (`l[1]`, 1-based, negative from the end): elements[k] is the index applied at
    // path[k], or kNoElement for a struct field. Empty when the path has no list-element step.
    std::vector<std::int64_t> elements;
    // Bound: child indices from the batch down (-1 for a list-element step), the leaf's storage type,
    // and its timestamp unit.
    std::vector<int64_t> indices;
    ArrowType type = NANOARROW_TYPE_UNINITIALIZED;
    ArrowTimeUnit unit = NANOARROW_TIME_UNIT_SECOND;
    // A list column (an array function's argument): its elements' type.
    ArrowType item_type = NANOARROW_TYPE_UNINITIALIZED;
    ArrowTimeUnit item_unit = NANOARROW_TIME_UNIT_SECOND;
    // Decimal columns: precision and scale (of the leaf, or of a list's items).
    int32_t precision = 0;
    int32_t scale = 0;
    int32_t item_scale = 0;
};

struct Node {
    Op op = Op::Literal;
    Value literal;
    ColumnRef column;
    std::string name;  // Func: lower-cased name; Cast: target type name
    std::string source;  // Literal numbers: the text as written ("-2.50"), for exact arrow_cast folding
    std::vector<std::unique_ptr<Node>> args;
    std::shared_ptr<const std::regex> regex;  // regexp_*: the pattern, when it is a literal
    enum class Family : std::uint8_t { Other, Json, Regexp } family = Family::Other;  // Func, set at parse
};

namespace {

// ── tokens ──────────────────────────────────────────────────────────────────────────────────────

enum class Tok { End, Ident, QuotedIdent, Number, String, Op, LParen, RParen, Comma, Dot, LBracket, RBracket };

/// arrow_cast(<number literal>, 'Decimal128(p, s)' or 'Decimal256(p, s)'), folded exactly from the
/// literal's text (so 2.555 rounds to 2.56, not through a double). Null with `error` set otherwise.
std::unique_ptr<Node> fold_decimal_cast(const Node& arg, const std::string& type, std::string& error) {
    int width = 0;
    int precision = 0;
    int scale = 0;
    if (std::sscanf(type.c_str(), "Decimal%d(%d, %d)", &width, &precision, &scale) != 3 ||
        (width != 128 && width != 256) || precision < 1 || precision > (width == 128 ? 38 : 76) || scale < -160 ||
        scale > 160) {
        error = "arrow_cast to '" + type + "' is not supported in nanolance filters";
        return nullptr;
    }
    const bool number = arg.op == Op::Literal && (arg.literal.kind == Kind::Int || arg.literal.kind == Kind::UInt ||
                                                  arg.literal.kind == Kind::Double);
    if (!number || arg.source.empty()) {
        error = "arrow_cast to '" + type + "' takes a number literal in nanolance filters";
        return nullptr;
    }
    Magnitude m;
    bool negative = false;
    int source_scale = 0;
    if (!parse_decimal_text(arg.source, m, negative, source_scale)) {
        error = "cannot read '" + arg.source + "' as a decimal";
        return nullptr;
    }
    Limbs256 limbs{};
    if (!fit_decimal(m, negative, source_scale, scale, precision, width == 128 ? "Decimal128" : "Decimal256", limbs,
                     error)) {
        return nullptr;
    }
    auto n = std::make_unique<Node>();
    n->op = Op::Literal;
    n->literal = Value::decimal(limbs, scale);
    n->name = type;  // the declared type, for the comparison rule in coerce()
    return n;
}

struct Token {
    Tok kind = Tok::End;
    std::string text;
    std::size_t pos = 0;
};

bool tokenize(std::string_view sql, std::vector<Token>& out, std::string& error) {
    std::size_t i = 0;
    while (i < sql.size()) {
        const char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        Token t;
        t.pos = i;
        if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
            std::size_t j = i;
            while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) != 0 || sql[j] == '_')) {
                ++j;
            }
            t.kind = Tok::Ident;
            t.text = std::string(sql.substr(i, j - i));
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                   (c == '.' && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])) != 0)) {
            std::size_t j = i;
            while (j < sql.size() && (std::isdigit(static_cast<unsigned char>(sql[j])) != 0 || sql[j] == '.')) {
                ++j;
            }
            if (j < sql.size() && (sql[j] == 'e' || sql[j] == 'E')) {
                std::size_t k = j + 1;
                if (k < sql.size() && (sql[k] == '+' || sql[k] == '-')) {
                    ++k;
                }
                if (k < sql.size() && std::isdigit(static_cast<unsigned char>(sql[k])) != 0) {
                    j = k;
                    while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j])) != 0) {
                        ++j;
                    }
                }
            }
            t.kind = Tok::Number;
            t.text = std::string(sql.substr(i, j - i));
            i = j;
        } else if (c == '\'' || c == '"' || c == '`') {
            const char quote = c;
            std::string text;
            std::size_t j = i + 1;
            bool closed = false;
            while (j < sql.size()) {
                if (sql[j] == quote) {
                    if (j + 1 < sql.size() && sql[j + 1] == quote) {
                        text += quote;
                        j += 2;
                        continue;
                    }
                    closed = true;
                    ++j;
                    break;
                }
                text += sql[j++];
            }
            if (!closed) {
                error = "unterminated quote at position " + std::to_string(i);
                return false;
            }
            t.kind = quote == '\'' ? Tok::String : Tok::QuotedIdent;
            t.text = std::move(text);
            i = j;
        } else if (c == '(') {
            t.kind = Tok::LParen;
            ++i;
        } else if (c == ')') {
            t.kind = Tok::RParen;
            ++i;
        } else if (c == ',') {
            t.kind = Tok::Comma;
            ++i;
        } else if (c == '[') {
            t.kind = Tok::LBracket;
            ++i;
        } else if (c == ']') {
            t.kind = Tok::RBracket;
            ++i;
        } else if (c == '.') {
            t.kind = Tok::Dot;
            ++i;
        } else {
            static const char* two[] = {"<=", ">=", "<>", "!=", "==", "||", "::"};
            t.kind = Tok::Op;
            for (const char* op : two) {
                if (sql.substr(i, 2) == op) {
                    t.text = op;
                }
            }
            if (t.text.empty()) {
                if (std::strchr("=<>+-*/%", c) == nullptr) {
                    // The whole UTF-8 character, not its first byte (half a character is not text).
                    std::size_t len = 1;
                    const auto lead = static_cast<unsigned char>(c);
                    if (lead >= 0xF0U) len = 4; else if (lead >= 0xE0U) len = 3; else if (lead >= 0xC0U) len = 2;
                    error = "unexpected character '" + std::string(sql.substr(i, len)) + "' at position " +
                            std::to_string(i);
                    return false;
                }
                t.text = std::string(1, c);
            }
            i += t.text.size();
        }
        out.push_back(std::move(t));
    }
    Token end;
    end.pos = sql.size();
    out.push_back(end);
    return true;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ── parser ──────────────────────────────────────────────────────────────────────────────────────

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string& error) : t_(std::move(tokens)), error_(error) {}

    std::unique_ptr<Node> parse() {
        auto n = parse_or();
        if (n && peek().kind != Tok::End) {
            fail("unexpected '" + describe(peek()) + "'");
            return nullptr;
        }
        return n;
    }

private:
    std::vector<Token> t_;
    std::size_t at_ = 0;
    std::string& error_;

    const Token& peek(std::size_t ahead = 0) const { return t_[std::min(at_ + ahead, t_.size() - 1)]; }
    Token next() { return t_[std::min(at_++, t_.size() - 1)]; }
    static std::string describe(const Token& t) { return t.kind == Tok::End ? "end of expression" : t.text; }
    bool keyword(const char* kw, std::size_t ahead = 0) const {
        const auto& t = peek(ahead);
        return t.kind == Tok::Ident && upper(t.text) == kw;
    }
    bool accept_keyword(const char* kw) {
        if (keyword(kw)) {
            ++at_;
            return true;
        }
        return false;
    }
    bool accept_op(const char* op) {
        if (peek().kind == Tok::Op && peek().text == op) {
            ++at_;
            return true;
        }
        return false;
    }
    std::nullptr_t fail(const std::string& why) {
        if (error_.empty()) {
            error_ = "invalid filter: " + why + " (at position " + std::to_string(peek().pos) + ")";
        }
        return nullptr;
    }

    static std::unique_ptr<Node> make(Op op, std::unique_ptr<Node> a = nullptr, std::unique_ptr<Node> b = nullptr) {
        auto n = std::make_unique<Node>();
        n->op = op;
        if (a) {
            n->args.push_back(std::move(a));
        }
        if (b) {
            n->args.push_back(std::move(b));
        }
        return n;
    }

    std::unique_ptr<Node> parse_or() {
        auto left = parse_and();
        while (left && accept_keyword("OR")) {
            auto right = parse_and();
            if (!right) {
                return nullptr;
            }
            left = make(Op::Or, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> parse_and() {
        auto left = parse_not();
        while (left && accept_keyword("AND")) {
            auto right = parse_not();
            if (!right) {
                return nullptr;
            }
            left = make(Op::And, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> parse_not() {
        if (accept_keyword("NOT")) {
            auto inner = parse_not();
            return inner ? make(Op::Not, std::move(inner)) : nullptr;
        }
        return parse_predicate();
    }

    std::unique_ptr<Node> parse_predicate() {
        auto left = parse_additive();
        if (!left) {
            return nullptr;
        }
        static const std::pair<const char*, Op> cmp[] = {{"=", Op::Eq},  {"==", Op::Eq}, {"!=", Op::Ne}, {"<>", Op::Ne},
                                                         {"<", Op::Lt},  {"<=", Op::Le}, {">", Op::Gt},  {">=", Op::Ge}};
        for (const auto& [text, op] : cmp) {
            if (accept_op(text)) {
                auto right = parse_additive();
                return right ? make(op, std::move(left), std::move(right)) : nullptr;
            }
        }
        if (accept_keyword("IS")) {
            const bool negated = accept_keyword("NOT");
            if (accept_keyword("NULL")) {
                return make(negated ? Op::IsNotNull : Op::IsNull, std::move(left));
            }
            if (accept_keyword("TRUE")) {
                return make(negated ? Op::IsNotTrue : Op::IsTrue, std::move(left));
            }
            if (accept_keyword("FALSE")) {
                return make(negated ? Op::IsNotFalse : Op::IsFalse, std::move(left));
            }
            return fail("expected NULL, TRUE or FALSE after IS");
        }
        const bool negated = keyword("NOT") && (keyword("IN", 1) || keyword("BETWEEN", 1) || keyword("LIKE", 1) ||
                                                keyword("ILIKE", 1));
        if (negated) {
            ++at_;
        }
        if (accept_keyword("IN")) {
            if (peek().kind != Tok::LParen) {
                return fail("expected '(' after IN");
            }
            next();
            auto n = make(negated ? Op::NotIn : Op::In, std::move(left));
            if (peek().kind != Tok::RParen) {
                for (;;) {
                    auto item = parse_additive();
                    if (!item) {
                        return nullptr;
                    }
                    n->args.push_back(std::move(item));
                    if (peek().kind == Tok::Comma) {
                        next();
                        continue;
                    }
                    break;
                }
            }
            if (next().kind != Tok::RParen) {
                return fail("expected ')' to close IN (...)");
            }
            return n;
        }
        if (accept_keyword("BETWEEN")) {
            auto lo = parse_additive();
            if (!lo || !accept_keyword("AND")) {
                return lo ? fail("expected AND in BETWEEN") : nullptr;
            }
            auto hi = parse_additive();
            if (!hi) {
                return nullptr;
            }
            auto n = make(negated ? Op::NotBetween : Op::Between, std::move(left), std::move(lo));
            n->args.push_back(std::move(hi));
            return n;
        }
        const bool like = keyword("LIKE");
        if (like || keyword("ILIKE")) {
            ++at_;
            auto pattern = parse_additive();
            if (!pattern) {
                return nullptr;
            }
            const Op op = like ? (negated ? Op::NotLike : Op::Like) : (negated ? Op::NotILike : Op::ILike);
            return make(op, std::move(left), std::move(pattern));
        }
        if (negated) {
            return fail("expected IN, BETWEEN or LIKE after NOT");
        }
        return left;
    }

    std::unique_ptr<Node> parse_additive() {
        auto left = parse_multiplicative();
        while (left) {
            Op op;
            if (accept_op("+")) {
                op = Op::Add;
            } else if (accept_op("-")) {
                op = Op::Sub;
            } else if (accept_op("||")) {
                op = Op::Concat;
            } else {
                break;
            }
            auto right = parse_multiplicative();
            if (!right) {
                return nullptr;
            }
            left = make(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> parse_multiplicative() {
        auto left = parse_unary();
        while (left) {
            Op op;
            if (accept_op("*")) {
                op = Op::Mul;
            } else if (accept_op("/")) {
                op = Op::Div;
            } else if (accept_op("%")) {
                op = Op::Mod;
            } else {
                break;
            }
            auto right = parse_unary();
            if (!right) {
                return nullptr;
            }
            left = make(op, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Node> parse_unary() {
        if (accept_op("-")) {
            auto inner = parse_unary();
            if (!inner) {
                return nullptr;
            }
            if (inner->op == Op::Literal && inner->literal.kind == Kind::Int) {
                inner->literal.i = -inner->literal.i;
                inner->source.insert(0, "-");
                return inner;
            }
            if (inner->op == Op::Literal && inner->literal.kind == Kind::Double) {
                inner->literal.d = -inner->literal.d;
                inner->source.insert(0, "-");
                return inner;
            }
            return make(Op::Neg, std::move(inner));
        }
        if (accept_op("+")) {
            return parse_unary();
        }
        return parse_postfix();
    }

    /// `expr::type`, PostgreSQL's cast (it binds tighter than a unary minus, as there).
    std::unique_ptr<Node> parse_postfix() {
        auto n = parse_primary();
        while (n && accept_op("::")) {
            std::string type;
            while (peek().kind == Tok::Ident) {
                type += (type.empty() ? "" : " ") + lower(next().text);
            }
            if (type.empty()) {
                return fail("expected a type after '::'");
            }
            if (peek().kind == Tok::LParen) {  // timestamp(6), decimal(10, 2): the size is ignored
                while (peek().kind != Tok::RParen && peek().kind != Tok::End) {
                    next();
                }
                next();
            }
            auto cast = make(Op::Cast, std::move(n));
            cast->name = type;
            n = std::move(cast);
        }
        return n;
    }

    /// A number as written: a float if it has a point or exponent, else an integer.
    static std::unique_ptr<Node> number_literal(const std::string& text) {
        if (text.find_first_of(".eE") != std::string::npos) {
            return literal(Value::real(std::strtod(text.c_str(), nullptr)));
        }
        std::uint64_t u = 0;
        const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), u);
        if (ec != std::errc() || p != text.data() + text.size()) {
            return literal(Value::real(std::strtod(text.c_str(), nullptr)));
        }
        return literal(u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                           ? Value::integer(static_cast<std::int64_t>(u))
                           : Value::uinteger(u));
    }

    static std::unique_ptr<Node> literal(Value v) {
        auto n = std::make_unique<Node>();
        n->op = Op::Literal;
        n->literal = std::move(v);
        return n;
    }

    std::unique_ptr<Node> parse_primary() {
        const Token t = peek();
        if (t.kind == Tok::Number && t.text == "0" && peek(1).kind == Tok::Ident && peek(1).pos == t.pos + 1 &&
            (peek(1).text[0] == 'x' || peek(1).text[0] == 'X')) {
            // 0x<hex>: a binary literal, as DataFusion reads it.
            next();
            const std::string hex = next().text.substr(1);
            if (hex.empty() || hex.size() % 2 != 0 ||
                hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                return fail("invalid hex literal 0x" + hex);
            }
            std::string bytes;
            for (std::size_t k = 0; k < hex.size(); k += 2) {
                bytes += static_cast<char>(std::stoi(hex.substr(k, 2), nullptr, 16));
            }
            return literal(Value::string(bytes, Kind::Binary));
        }
        if (t.kind == Tok::Number) {
            next();
            auto n = number_literal(t.text);
            n->source = t.text;
            return n;
        }
        if (t.kind == Tok::String) {
            next();
            return literal(Value::string(t.text));
        }
        if (t.kind == Tok::Ident && upper(t.text) == "ARRAY" && peek(1).kind == Tok::LBracket) {
            next();  // ARRAY['a', 'b']: the array literal below
        }
        if (peek().kind == Tok::LBracket) {
            next();
            std::vector<Value> items;
            if (peek().kind != Tok::RBracket) {
                for (;;) {
                    auto item = parse_unary();
                    if (!item) {
                        return nullptr;
                    }
                    if (item->op != Op::Literal) {
                        return fail("an array literal holds literals only");
                    }
                    items.push_back(item->literal);
                    if (peek().kind == Tok::Comma) {
                        next();
                        continue;
                    }
                    break;
                }
            }
            if (next().kind != Tok::RBracket) {
                return fail("expected ']' to close an array literal");
            }
            return literal(Value::list(std::move(items)));
        }
        if (t.kind == Tok::LParen) {
            next();
            auto inner = parse_or();
            if (!inner) {
                return nullptr;
            }
            if (next().kind != Tok::RParen) {
                return fail("expected ')'");
            }
            return inner;
        }
        if (t.kind == Tok::QuotedIdent) {
            return parse_column();
        }
        if (t.kind != Tok::Ident) {
            return fail("unexpected '" + describe(t) + "'");
        }
        const std::string kw = upper(t.text);
        if (kw == "TRUE" || kw == "FALSE") {
            next();
            return literal(Value::boolean(kw == "TRUE"));
        }
        if (kw == "NULL") {
            next();
            return literal(Value::null());
        }
        if ((kw == "X") && peek(1).kind == Tok::String && peek(1).pos == t.pos + 1) {
            next();
            const Token hex = next();
            std::string bytes;
            if (hex.text.size() % 2 != 0) {
                return fail("a binary literal needs an even number of hex digits");
            }
            for (std::size_t k = 0; k < hex.text.size(); k += 2) {
                const auto nibble = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = nibble(hex.text[k]);
                const int lo = nibble(hex.text[k + 1]);
                if (hi < 0 || lo < 0) {
                    return fail("invalid hex digit in a binary literal");
                }
                bytes += static_cast<char>(hi * 16 + lo);
            }
            return literal(Value::string(bytes, Kind::Binary));
        }
        if ((kw == "DATE" || kw == "TIMESTAMP") && (peek(1).kind == Tok::String || peek(1).kind == Tok::QuotedIdent)) {
            next();
            const Token s = next();
            std::int64_t ns = 0;
            bool date_only = false;
            if (!parse_datetime(s.text, ns, date_only) || (kw == "DATE" && !date_only)) {
                return fail("invalid " + kw + " literal '" + s.text + "'");
            }
            return literal(kw == "DATE" ? Value::date(ns / kNsPerDay) : Value::timestamp(ns));
        }
        if (kw == "CAST" && peek(1).kind == Tok::LParen) {
            next();
            next();
            auto inner = parse_or();
            if (!inner || !accept_keyword("AS")) {
                return inner ? fail("expected AS in CAST") : nullptr;
            }
            std::string type;
            while (peek().kind == Tok::Ident) {
                type += (type.empty() ? "" : " ") + lower(next().text);
            }
            std::vector<std::string> sizes;
            if (peek().kind == Tok::LParen) {  // VARCHAR(10): ignored; DECIMAL(10, 2): kept
                while (peek().kind != Tok::RParen && peek().kind != Tok::End) {
                    const Token tok = next();
                    if (tok.kind == Tok::Number) {
                        sizes.push_back(std::string(tok.text));
                    }
                }
                next();
            }
            if (next().kind != Tok::RParen) {
                return fail("expected ')' to close CAST");
            }
            auto n = make(Op::Cast, std::move(inner));
            n->name = type;
            if (type == "decimal" && sizes.size() == 2) {
                n->name = "decimal " + sizes[0] + " " + sizes[1];
            }
            return n;
        }
        if (peek(1).kind == Tok::LParen) {
            next();
            next();
            auto n = std::make_unique<Node>();
            n->op = Op::Func;
            n->name = lower(t.text);
            if (peek().kind != Tok::RParen) {
                for (;;) {
                    auto arg = parse_or();
                    if (!arg) {
                        return nullptr;
                    }
                    n->args.push_back(std::move(arg));
                    if (peek().kind == Tok::Comma) {
                        next();
                        continue;
                    }
                    break;
                }
            }
            if (next().kind != Tok::RParen) {
                return fail("expected ')' to close " + t.text + "(...)");
            }
            return rewrite_function(std::move(n));
        }
        return parse_column();
    }

    /// Functions that are another construct: pyarrow's arithmetic names, arrow_cast, the clock.
    std::unique_ptr<Node> rewrite_function(std::unique_ptr<Node> n) {
        static const std::unordered_map<std::string, Op> arithmetic = {
            {"add", Op::Add}, {"add_checked", Op::Add}, {"subtract", Op::Sub}, {"subtract_checked", Op::Sub},
            {"multiply", Op::Mul}, {"multiply_checked", Op::Mul}, {"divide", Op::Div}, {"divide_checked", Op::Div},
        };
        if (const auto it = arithmetic.find(n->name); it != arithmetic.end() && n->args.size() == 2U) {
            return make(it->second, std::move(n->args[0]), std::move(n->args[1]));
        }
        if ((n->name == "negate" || n->name == "negate_checked") && n->args.size() == 1U) {
            return make(Op::Neg, std::move(n->args[0]));
        }
        const auto now_ns = [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        };
        if (n->name == "current_date" && n->args.empty()) {
            const auto ns = now_ns();
            return literal(Value::date(ns / kNsPerDay - (ns % kNsPerDay < 0 ? 1 : 0)));
        }
        if ((n->name == "now" || n->name == "current_timestamp") && n->args.empty()) {
            return literal(Value::timestamp(now_ns()));
        }
        if (n->name == "arrow_cast") {
            if (n->args.size() != 2U || n->args[1]->op != Op::Literal || n->args[1]->literal.kind != Kind::String) {
                return fail("arrow_cast(expression, 'Type') takes a type name");
            }
            const std::string type(n->args[1]->literal.s);
            if (type.rfind("Decimal128(", 0) == 0 || type.rfind("Decimal256(", 0) == 0) {
                std::string error;
                auto folded = fold_decimal_cast(*n->args[0], type, error);
                if (folded || n->args[0]->op == Op::Literal || type.rfind("Decimal256(", 0) == 0) {
                    return folded ? std::move(folded) : fail(error);
                }
                // A column: a cast to decimal(p, s) in a projection. A filter refuses it when it evaluates it.
                int precision = 0;
                int scale = 0;
                if (std::sscanf(type.c_str(), "Decimal128(%d, %d)", &precision, &scale) != 2) {
                    return fail(error);
                }
                auto cast = make(Op::Cast, std::move(n->args[0]));
                cast->name = "decimal " + std::to_string(precision) + " " + std::to_string(scale);
                cast->source = type;
                return cast;
            }
            if (type.rfind("Duration(", 0) == 0) {
                // arrow_cast(<integer literal>, 'Duration(Unit)'): the count in that unit.
                const auto close = type.find(')');
                const std::string word = close == std::string::npos ? "" : type.substr(9, close - 9);
                const auto& arg = *n->args[0];
                ArrowTimeUnit unit = NANOARROW_TIME_UNIT_SECOND;
                bool known = true;
                if (word == "Second") {
                    unit = NANOARROW_TIME_UNIT_SECOND;
                } else if (word == "Millisecond") {
                    unit = NANOARROW_TIME_UNIT_MILLI;
                } else if (word == "Microsecond") {
                    unit = NANOARROW_TIME_UNIT_MICRO;
                } else if (word == "Nanosecond") {
                    unit = NANOARROW_TIME_UNIT_NANO;
                } else {
                    known = false;
                }
                if (!known || close + 1 != type.size()) {
                    return fail("arrow_cast to '" + type + "' is not supported in nanolance filters");
                }
                if (arg.op != Op::Literal || arg.literal.kind != Kind::Int) {
                    return fail("arrow_cast to '" + type + "' takes an integer literal in nanolance filters");
                }
                auto lit = std::make_unique<Node>();
                lit->op = Op::Literal;
                lit->literal = Value::duration(arg.literal.i, unit);
                return lit;
            }
            static const std::vector<std::pair<std::string, std::string>> types = {
                {"Int8", "int8"}, {"Int16", "int16"}, {"Int32", "int32"}, {"Int64", "int64"},
                {"UInt8", "uint8"}, {"UInt16", "uint16"}, {"UInt32", "uint32"}, {"UInt64", "uint64"},
                {"Float16", "double"}, {"Float32", "float"}, {"Float64", "double"},
                {"Utf8", "string"}, {"LargeUtf8", "string"}, {"Utf8View", "string"},
                {"Binary", "binary"}, {"LargeBinary", "binary"}, {"BinaryView", "binary"},
                {"Boolean", "boolean"}, {"Date32", "date"}, {"Date64", "date"},
            };
            std::string target;
            // FixedSizeBinary(n): compared as binary, but a literal of another length fails as it does in
            // DataFusion (its FixedSizeBinaryBuilder refuses the value).
            if (type.rfind("FixedSizeBinary(", 0) == 0 && type.back() == ')') {
                const std::string width = type.substr(16, type.size() - 17);
                if (width.empty() || width.size() > 9 || width.find_first_not_of("0123456789") != std::string::npos) {
                    return fail("arrow_cast to '" + type + "' is not supported in nanolance filters");
                }
                const Node& value = *n->args[0];
                if (value.op == Op::Literal && value.literal.kind == Kind::Binary &&
                    value.literal.s.size() != std::stoul(width)) {
                    return fail("Invalid argument error: Byte slice does not have the same length as "
                                "FixedSizeBinaryBuilder value lengths");
                }
                target = "binary";
            }
            for (const auto& [arrow, sql] : types) {
                if (type == arrow) {
                    target = sql;
                }
            }
            if (target.empty() && type.rfind("Timestamp(", 0) == 0) {
                target = "timestamp";
            }
            if (target.empty()) {
                return fail("arrow_cast to '" + type + "' is not supported in nanolance filters");
            }
            auto cast = make(Op::Cast, std::move(n->args[0]));
            cast->name = target;
            return cast;
        }
        if (n->name.rfind("json_", 0) == 0) {
            n->family = Node::Family::Json;
        }
        if (n->name == "regexp_match" || n->name == "regexp_like") {
            n->family = Node::Family::Regexp;
        }
        if ((n->name == "regexp_match" || n->name == "regexp_like") && n->args.size() >= 2U &&
            n->args[1]->op == Op::Literal && n->args[1]->literal.kind == Kind::String) {
            auto flags = std::regex::ECMAScript;
            if (n->args.size() == 3U && n->args[2]->op == Op::Literal && n->args[2]->literal.kind == Kind::String &&
                std::string_view(n->args[2]->literal.s).find('i') != std::string_view::npos) {
                flags |= std::regex::icase;
            }
            try {
                n->regex = std::make_shared<const std::regex>(std::string(n->args[1]->literal.s), flags);
            } catch (const std::regex_error& e) {
                return fail("invalid regular expression '" + std::string(n->args[1]->literal.s) + "': " + e.what());
            }
        }
        return n;
    }

    /// Appends one step to a column path: a field name, or (element != kNoElement) a list element.
    static void push_step(ColumnRef& c, std::string text, std::int64_t element = ColumnRef::kNoElement) {
        if (element != ColumnRef::kNoElement && c.elements.empty()) {
            c.elements.assign(c.path.size(), ColumnRef::kNoElement);
        }
        c.path.push_back(std::move(text));
        if (!c.elements.empty()) {
            c.elements.push_back(element);
        }
    }

    /// `name.field`, `name['field']` and `name[1]` (a 1-based list element, negative from the end).
    std::unique_ptr<Node> parse_column() {
        auto n = std::make_unique<Node>();
        n->op = Op::Column;
        push_step(n->column, next().text);
        for (;;) {
            if (peek().kind == Tok::Dot && (peek(1).kind == Tok::Ident || peek(1).kind == Tok::QuotedIdent)) {
                next();
                push_step(n->column, next().text);
                continue;
            }
            if (peek().kind != Tok::LBracket) {
                break;
            }
            next();
            if (peek().kind == Tok::String || peek().kind == Tok::QuotedIdent) {
                push_step(n->column, next().text);
            } else {
                const bool negative = accept_op("-");
                if (peek().kind != Tok::Number || peek().text.find_first_not_of("0123456789") != std::string::npos ||
                    peek().text.size() > 18) {
                    return fail("list index must be an integer literal, not '" + describe(peek()) + "'");
                }
                std::int64_t index = 0;
                std::from_chars(peek().text.data(), peek().text.data() + peek().text.size(), index);
                push_step(n->column, "[" + std::string(negative ? "-" : "") + next().text + "]",
                          negative ? -index : index);
            }
            if (next().kind != Tok::RBracket) {
                return fail("expected ']' to close the index");
            }
        }
        return n;
    }
};

// ── printing ────────────────────────────────────────────────────────────────────────────────────

const char* op_text(Op op) {
    switch (op) {
        case Op::And: return "AND";
        case Op::Or: return "OR";
        case Op::Eq: return "=";
        case Op::Ne: return "!=";
        case Op::Lt: return "<";
        case Op::Le: return "<=";
        case Op::Gt: return ">";
        case Op::Ge: return ">=";
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Mod: return "%";
        case Op::Concat: return "||";
        case Op::Like: return "LIKE";
        case Op::NotLike: return "NOT LIKE";
        case Op::ILike: return "ILIKE";
        case Op::NotILike: return "NOT ILIKE";
        default: return "?";
    }
}

std::string print(const Node& n) {
    auto arg = [&](std::size_t k) { return print(*n.args[k]); };
    switch (n.op) {
        case Op::Literal: return format_value(n.literal);
        case Op::Column: {
            std::string out;
            for (std::size_t k = 0; k < n.column.path.size(); ++k) {
                out += (out.empty() || (!n.column.elements.empty() && n.column.elements[k] != ColumnRef::kNoElement)
                            ? "" : ".") + n.column.path[k];
            }
            return out;
        }
        case Op::Not: return "(NOT " + arg(0) + ")";
        case Op::Neg: return "(-" + arg(0) + ")";
        case Op::IsNull: return "(" + arg(0) + " IS NULL)";
        case Op::IsNotNull: return "(" + arg(0) + " IS NOT NULL)";
        case Op::IsTrue: return "(" + arg(0) + " IS TRUE)";
        case Op::IsFalse: return "(" + arg(0) + " IS FALSE)";
        case Op::IsNotTrue: return "(" + arg(0) + " IS NOT TRUE)";
        case Op::IsNotFalse: return "(" + arg(0) + " IS NOT FALSE)";
        case Op::In:
        case Op::NotIn: {
            std::string out = "(" + arg(0) + (n.op == Op::In ? " IN (" : " NOT IN (");
            for (std::size_t k = 1; k < n.args.size(); ++k) {
                out += (k > 1 ? ", " : "") + arg(k);
            }
            return out + "))";
        }
        case Op::Between: return "(" + arg(0) + " BETWEEN " + arg(1) + " AND " + arg(2) + ")";
        case Op::NotBetween: return "(" + arg(0) + " NOT BETWEEN " + arg(1) + " AND " + arg(2) + ")";
        case Op::Func: {
            std::string out = n.name + "(";
            for (std::size_t k = 0; k < n.args.size(); ++k) {
                out += (k > 0 ? ", " : "") + arg(k);
            }
            return out + ")";
        }
        case Op::Cast: return "CAST(" + arg(0) + " AS " + n.name + ")";
        default: return "(" + arg(0) + " " + op_text(n.op) + " " + arg(1) + ")";
    }
}

void collect_columns(const Node& n, std::vector<std::string>& out) {
    if (n.op == Op::Column) {
        // The column read: the path up to its first list element (`l[1].x` reads `l`).
        std::string full;
        for (std::size_t k = 0; k < n.column.path.size(); ++k) {
            if (!n.column.elements.empty() && n.column.elements[k] != ColumnRef::kNoElement) {
                break;
            }
            full += (full.empty() ? "" : ".") + n.column.path[k];
        }
        if (std::find(out.begin(), out.end(), full) == out.end()) {
            out.push_back(full);
        }
    }
    for (const auto& a : n.args) {
        collect_columns(*a, out);
    }
}

}  // namespace

// ── binding ─────────────────────────────────────────────────────────────────────────────────────

struct Expression::Binding {
    ArrowSchema schema{};
    ~Binding() {
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }
};

namespace {

bool equals_ci(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

/// Index of the child of `schema` named `name`: exact, else case-insensitive (unique), else -1.
int64_t find_child(const ArrowSchema& schema, const std::string& name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name != nullptr && name == schema.children[i]->name) {
            return i;
        }
    }
    int64_t found = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name != nullptr && equals_ci(name, schema.children[i]->name)) {
            if (found >= 0) {
                return -1;
            }
            found = i;
        }
    }
    return found;
}

bool scalar_type(ArrowType t) {
    switch (t) {
        case NANOARROW_TYPE_BOOL:
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
        case NANOARROW_TYPE_HALF_FLOAT:
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE:
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_FIXED_SIZE_BINARY:
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_DATE64:
        case NANOARROW_TYPE_TIMESTAMP:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64:
        case NANOARROW_TYPE_DURATION:
        case NANOARROW_TYPE_DECIMAL128:
        case NANOARROW_TYPE_DECIMAL256:
            return true;
        default:
            return false;
    }
}

bool is_list_type(ArrowType t) {
    return t == NANOARROW_TYPE_LIST || t == NANOARROW_TYPE_LARGE_LIST || t == NANOARROW_TYPE_FIXED_SIZE_LIST;
}

bool is_array_function(const std::string& name) {
    return name == "array_has_any" || name == "array_has_all" || name == "array_has" || name == "array_contains" ||
           name == "list_has_any" || name == "list_has_all" || name == "list_has" || name == "list_contains";
}

bool bind_node(Node& n, const ArrowSchema& schema, bool under_null_test, std::string& error,
               bool under_array_function = false) {
    if (n.op == Op::Column) {
        auto& c = n.column;
        c.indices.clear();
        const ArrowSchema* at = &schema;
        // "a.b" may be a column named "a.b" as well as a struct path.
        std::string joined;
        for (std::size_t k = 0; k < c.path.size(); ++k) {
            const bool element = !c.elements.empty() && c.elements[k] != ColumnRef::kNoElement;
            joined += (joined.empty() || element ? "" : ".") + c.path[k];
        }
        const int64_t whole = c.path.size() > 1 && c.elements.empty() ? find_child(schema, joined) : -1;
        if (whole >= 0) {
            c.indices.push_back(whole);
            at = schema.children[whole];
        } else {
            for (std::size_t k = 0; k < c.path.size(); ++k) {
                if (!c.elements.empty() && c.elements[k] != ColumnRef::kNoElement) {
                    // `l[1]`: a list whose items are not lists (pylance refuses nested indexing too).
                    ArrowSchemaView lv{};
                    ArrowError lerr;
                    if (ArrowSchemaViewInit(&lv, at, &lerr) != NANOARROW_OK || !is_list_type(lv.type) ||
                        at->n_children != 1) {
                        error = "column '" + joined + "' is not a list, so it cannot take " + c.path[k];
                        return false;
                    }
                    ArrowSchemaView item{};
                    if (ArrowSchemaViewInit(&item, at->children[0], &lerr) != NANOARROW_OK || is_list_type(item.type)) {
                        error = "column '" + joined + "' is a list of lists, which filters cannot index";
                        return false;
                    }
                    c.indices.push_back(-1);
                    at = at->children[0];
                    continue;
                }
                const int64_t found = find_child(*at, c.path[k]);
                if (found < 0) {
                    error = "column '" + joined + "' not found";
                    return false;
                }
                c.indices.push_back(found);
                at = at->children[found];
            }
        }
        ArrowSchemaView sv{};  // time_unit is only set for temporal types: zero, not garbage, otherwise
        ArrowError aerr;
        if (ArrowSchemaViewInit(&sv, at, &aerr) != NANOARROW_OK) {
            error = std::string("cannot read the type of column '") + joined + "': " + aerr.message;
            return false;
        }
        c.type = sv.type;
        c.unit = sv.time_unit;
        c.precision = sv.decimal_precision;
        c.scale = sv.decimal_scale;
        c.item_type = NANOARROW_TYPE_UNINITIALIZED;
        if (under_array_function && is_list_type(sv.type)) {
            ArrowSchemaView item{};
            if (ArrowSchemaViewInit(&item, at->children[0], &aerr) != NANOARROW_OK || !scalar_type(item.type)) {
                error = "column '" + joined + "' is a list of a type filters cannot compare";
                return false;
            }
            c.item_type = item.type;
            c.item_unit = item.time_unit;
            c.item_scale = item.decimal_scale;
            return true;
        }
        if (!scalar_type(sv.type) && !under_null_test) {
            error = "column '" + joined + "' has a type filters cannot compare (only IS NULL / IS NOT NULL)";
            return false;
        }
        return true;
    }
    const bool null_test = n.op == Op::IsNull || n.op == Op::IsNotNull ||
                           (n.op == Op::Func && (n.name == "is_null" || n.name == "is_valid"));
    const bool array_function = n.op == Op::Func && (is_array_function(n.name) || n.name == "_cast_list_f16");
    for (auto& a : n.args) {
        const bool first = a.get() == n.args.front().get();
        if (!bind_node(*a, schema, null_test && first, error, array_function && first)) {
            return false;
        }
    }
    if (n.op == Op::Func) {
        static const std::unordered_map<std::string, std::pair<int, int>> arity = {
            {"lower", {1, 1}},      {"upper", {1, 1}},       {"length", {1, 1}},   {"char_length", {1, 1}},
            {"character_length", {1, 1}}, {"abs", {1, 1}},   {"coalesce", {1, 64}}, {"starts_with", {2, 2}},
            {"ends_with", {2, 2}},  {"contains", {2, 2}},    {"is_null", {1, 1}},  {"is_valid", {1, 1}},
            {"invert", {1, 1}},     {"and_", {2, 2}},        {"or_", {2, 2}},      {"equal", {2, 2}},
            {"array_has_any", {2, 2}}, {"array_has_all", {2, 2}}, {"array_has", {2, 2}}, {"array_contains", {2, 2}},
            {"list_has_any", {2, 2}},  {"list_has_all", {2, 2}},  {"list_has", {2, 2}},  {"list_contains", {2, 2}},
            {"regexp_match", {2, 3}},  {"regexp_like", {2, 3}},
            {"json_get", {2, 2}},      {"json_get_string", {2, 2}}, {"json_get_int", {2, 2}},
            {"json_get_float", {2, 2}}, {"json_get_bool", {2, 2}},  {"json_extract", {2, 2}},
            {"json_exists", {2, 2}},   {"json_array_contains", {3, 3}}, {"json_array_length", {2, 2}},
            {"_cast_list_f16", {1, 1}},
        };
        const auto it = arity.find(n.name);
        if (it == arity.end()) {
            error = "function '" + n.name + "' is not supported in nanolance filters";
            return false;
        }
        const auto count = static_cast<int>(n.args.size());
        if (count < it->second.first || count > it->second.second) {
            error = "wrong number of arguments to " + n.name + "()";
            return false;
        }
        if (n.name == "_cast_list_f16") {  // pylance's words for a list that is not of floats
            const auto& a = *n.args[0];
            if (a.op != Op::Column || !is_list_type(a.column.type)) {
                error = "cast_list_f16 only supports FixedSizeList/List arguments";
                return false;
            }
            if (a.column.item_type != NANOARROW_TYPE_FLOAT && a.column.item_type != NANOARROW_TYPE_HALF_FLOAT) {
                error = "cast_list_f16 only supports list of float32 or float16";
                return false;
            }
        }
    }
    return true;
}

bool temporal_type(ArrowType t) {
    return t == NANOARROW_TYPE_DATE32 || t == NANOARROW_TYPE_DATE64 || t == NANOARROW_TYPE_TIMESTAMP;
}

/// `d` rounded to the nearest float32 (out of range: +-inf), held as a double.
double round_to_float32(double d) {
    // Past the midpoint between FLT_MAX and 2^128, float32 rounding gives infinity.
    if (std::isfinite(d) && std::fabs(d) >= 0x1.ffffffp127) {
        return std::copysign(std::numeric_limits<double>::infinity(), d);
    }
    if (std::isfinite(d) && std::fabs(d) > std::numeric_limits<float>::max()) {
        return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), d);
    }
    return static_cast<float>(d);
}

/// As DataFusion coerces a comparison's literals to its column's type: a string literal compared
/// with a date or timestamp column becomes a date / timestamp literal, and a number compared with
/// a float32 column is rounded to float32 (so `x = 0.1` matches the float32 0.1).
/// A literal as DataFusion shows it in a type error: `Int64(2)`, `Float64(2.5)` (2.0 is `Float64(2)`,
/// the shortest form), `Utf8("x")`; empty for any other kind.
std::string datafusion_literal(const Value& v) {
    switch (v.kind) {
        case Kind::Int: return "Int64(" + std::to_string(v.i) + ")";
        case Kind::UInt: return "UInt64(" + std::to_string(v.u) + ")";
        case Kind::Double: return "Float64(" + float_text::to_text(v.d) + ")";
        case Kind::String: return "Utf8(\"" + std::string(v.s) + "\")";
        default: return {};
    }
}

/// A decimal operand's type as the planner sees it: a decimal column, or a folded
/// arrow_cast(number, 'Decimal..') literal.
struct DecimalType {
    bool wide = false;  // Decimal256
    std::int32_t precision = 0;
    std::int32_t scale = 0;
};

bool decimal_type_of(const Node& n, DecimalType& t) {
    if (n.op == Op::Column && (n.column.type == NANOARROW_TYPE_DECIMAL128 || n.column.type == NANOARROW_TYPE_DECIMAL256)) {
        t = DecimalType{n.column.type == NANOARROW_TYPE_DECIMAL256, n.column.precision, n.column.scale};
        return true;
    }
    int width = 0;
    int precision = 0;
    int scale = 0;
    if (n.op == Op::Literal && n.literal.kind == Kind::Decimal &&
        std::sscanf(n.name.c_str(), "Decimal%d(%d, %d)", &width, &precision, &scale) == 3) {
        t = DecimalType{width == 256, precision, scale};
        return true;
    }
    return false;
}

std::string decimal_type_text(const DecimalType& t) {
    return std::string(t.wide ? "Decimal256(" : "Decimal128(") + std::to_string(t.precision) + ", " +
           std::to_string(t.scale) + ")";
}

/// DataFusion coerces two decimals of different widths to one type with the larger integer digits and
/// the larger scale, and refuses it when it needs more than 76 digits. Same-width pairs are not refused.
bool decimals_clash(const Node& a, const Node& b, std::string& x_text, std::string& y_text) {
    DecimalType x;
    DecimalType y;
    if (!decimal_type_of(a, x) || !decimal_type_of(b, y) || x.wide == y.wide) {
        return false;
    }
    const std::int32_t integer_digits = std::max(x.precision - x.scale, y.precision - y.scale);
    if (integer_digits + std::max(x.scale, y.scale) <= 76) {
        return false;
    }
    x_text = decimal_type_text(x);
    y_text = decimal_type_text(y);
    return true;
}

bool coerce(Node& n, std::string& error) {
    for (auto& a : n.args) {
        if (!coerce(*a, error)) {
            return false;
        }
    }
    const bool compares = n.op == Op::Eq || n.op == Op::Ne || n.op == Op::Lt || n.op == Op::Le || n.op == Op::Gt ||
                          n.op == Op::Ge || n.op == Op::In || n.op == Op::NotIn || n.op == Op::Between ||
                          n.op == Op::NotBetween;
    if (!compares) {
        return true;
    }
    // Decimals of different widths whose common type needs more than 76 digits: DataFusion's refusals.
    std::string x;
    std::string y;
    if (n.args.size() == 2 && decimals_clash(*n.args[0], *n.args[1], x, y) &&
        (n.op == Op::Eq || n.op == Op::Ne || n.op == Op::Lt || n.op == Op::Le || n.op == Op::Gt || n.op == Op::Ge)) {
        error = "Cannot infer common argument type for comparison operation " + x + " " + op_text(n.op) + " " + y;
        return false;
    }
    if (n.op == Op::In || n.op == Op::NotIn) {
        for (std::size_t k = 1; k < n.args.size(); ++k) {
            if (decimals_clash(*n.args[0], *n.args[k], x, y)) {
                error = "Can not find compatible types to compare " + x + " with [" + y + "]";
                return false;
            }
        }
    }
    if (n.op == Op::Between || n.op == Op::NotBetween) {
        for (std::size_t k = 1; k < n.args.size() && k <= 2; ++k) {
            if (decimals_clash(*n.args[0], *n.args[k], x, y)) {
                error = "Failed to coerce types " + x + " and " + y + " in BETWEEN expression.";
                return false;
            }
        }
    }
    // pylance refuses a number or string literal against a duration column (arrow_cast(x, 'Duration(..)')
    // is the way to write one), and words the refusal with the literal as DataFusion types it.
    for (const auto& a : n.args) {
        if (a->op != Op::Column || a->column.type != NANOARROW_TYPE_DURATION) {
            continue;
        }
        for (const auto& b : n.args) {
            if (b->op != Op::Literal) {
                continue;
            }
            const std::string shown = datafusion_literal(b->literal);
            if (shown.empty()) {
                continue;
            }
            error = "Received literal " + shown + " and could not convert to literal of type 'Duration(" +
                    unit_name(a->column.unit) + ")'";
            return false;
        }
    }
    // pylance refuses a bare float literal against a decimal column (arrow_cast(x, 'Float64') is fine).
    for (const auto& a : n.args) {
        if (a->op != Op::Column || (a->column.type != NANOARROW_TYPE_DECIMAL128 &&
                                    a->column.type != NANOARROW_TYPE_DECIMAL256)) {
            continue;
        }
        for (const auto& b : n.args) {
            if (b->op == Op::Literal && b->literal.kind == Kind::Double) {
                error = "Received literal " + datafusion_literal(b->literal) + " and could not convert to literal of type '" +
                        (a->column.type == NANOARROW_TYPE_DECIMAL128 ? "Decimal128" : "Decimal256") + "(" +
                        std::to_string(a->column.precision) + ", " + std::to_string(a->column.scale) + ")'";
                return false;
            }
        }
    }
    const Node* column = nullptr;
    for (const auto& a : n.args) {
        if (a->op == Op::Column && (temporal_type(a->column.type) || a->column.type == NANOARROW_TYPE_FLOAT)) {
            column = a.get();
        }
    }
    if (column == nullptr) {
        return true;
    }
    if (column->column.type == NANOARROW_TYPE_FLOAT) {
        for (auto& a : n.args) {
            if (a->op == Op::Literal && a->literal.numeric()) {
                a->literal = Value::real(round_to_float32(a->literal.as_double()));
            }
        }
        return true;
    }
    for (auto& a : n.args) {
        if (a->op == Op::Literal && a->literal.kind == Kind::String) {
            std::int64_t ns = 0;
            bool date_only = false;
            if (!parse_datetime(a->literal.s, ns, date_only)) {
                error = "cannot compare column with '" + std::string(a->literal.s) + "': not a date or timestamp";
                return false;
            }
            a->literal = column->column.type == NANOARROW_TYPE_DATE32 && date_only ? Value::date(ns / kNsPerDay)
                                                                                  : Value::timestamp(ns);
        }
    }
    return true;
}

// ── evaluation ──────────────────────────────────────────────────────────────────────────────────

std::int64_t unit_to_ns(ArrowTimeUnit unit) {
    switch (unit) {
        case NANOARROW_TIME_UNIT_SECOND: return 1000000000LL;
        case NANOARROW_TIME_UNIT_MILLI: return 1000000LL;
        case NANOARROW_TIME_UNIT_MICRO: return 1000LL;
        default: return 1LL;
    }
}

struct Context {
    const ArrowArrayView* batch = nullptr;
    std::string* error = nullptr;
    bool failed = false;
    bool projection = false;  // a projection, not a filter: pylance's temporal arithmetic (temporal_arith)

    Value fail(const std::string& why) {
        if (!failed) {
            failed = true;
            *error = why;
        }
        return Value::null();
    }
};

Value read_scalar(const ArrowArrayView* view, int64_t at, ArrowType type, ArrowTimeUnit unit, int32_t scale);

Value read_column(const ColumnRef& c, int64_t row, Context& ctx) {
    const ArrowArrayView* view = ctx.batch;
    int64_t at = row;
    for (std::size_t step = 0; step < c.indices.size(); ++step) {
        if (view->storage_type == NANOARROW_TYPE_STRUCT && ArrowArrayViewIsNull(view, at)) {
            return Value::null();
        }
        if (!c.elements.empty() && c.elements[step] != ColumnRef::kNoElement) {
            // A list element: `at` becomes its position among the list's children (absolute, as
            // ArrowArrayViewListChildOffset gives it). Out of range, or a null list, is null.
            if (ArrowArrayViewIsNull(view, at)) {
                return Value::null();
            }
            int64_t begin = 0;
            int64_t length = 0;
            if (view->storage_type == NANOARROW_TYPE_FIXED_SIZE_LIST) {
                length = view->layout.child_size_elements;
                begin = (view->offset + at) * length;
            } else {
                begin = ArrowArrayViewListChildOffset(view, at);
                length = ArrowArrayViewListChildOffset(view, at + 1) - begin;
            }
            int64_t k = c.elements[step];
            if (k < 0) {
                k += length + 1;
            }
            if (k < 1 || k > length) {
                return Value::null();
            }
            at = begin + k - 1;
            view = view->children[0];
            continue;
        }
        at += view->offset;
        view = view->children[c.indices[step]];
    }
    if (ArrowArrayViewIsNull(view, at)) {
        return Value::null();
    }
    if (c.item_type != NANOARROW_TYPE_UNINITIALIZED) {
        int64_t begin = 0;
        int64_t end = 0;
        if (c.type == NANOARROW_TYPE_FIXED_SIZE_LIST) {
            const auto size = view->layout.child_size_elements;
            begin = (view->offset + at) * size;
            end = begin + size;
        } else {
            begin = ArrowArrayViewListChildOffset(view, at);
            end = ArrowArrayViewListChildOffset(view, at + 1);
        }
        std::vector<Value> items;
        items.reserve(static_cast<std::size_t>(end - begin));
        for (auto e = begin; e < end; ++e) {
            items.push_back(ArrowArrayViewIsNull(view->children[0], e)
                                ? Value::null()
                                : read_scalar(view->children[0], e, c.item_type, c.item_unit, c.item_scale));
        }
        return Value::list(std::move(items));
    }
    return read_scalar(view, at, c.type, c.unit, c.scale);
}

Value read_scalar(const ArrowArrayView* view, int64_t at, ArrowType type, ArrowTimeUnit unit, int32_t scale) {
    switch (type) {
        case NANOARROW_TYPE_DECIMAL128:
        case NANOARROW_TYPE_DECIMAL256: {
            ArrowDecimal dec{};
            ArrowDecimalInit(&dec, type == NANOARROW_TYPE_DECIMAL128 ? 128 : 256, 0, scale);
            ArrowArrayViewGetDecimalUnsafe(view, at, &dec);
            Limbs256 limbs{dec.words[0], dec.words[1], dec.words[2], dec.words[3]};
            if (type == NANOARROW_TYPE_DECIMAL128) {  // sign-extend the 128-bit value
                const std::uint64_t fill = static_cast<std::int64_t>(dec.words[1]) < 0 ? ~std::uint64_t{0} : 0;
                limbs[2] = fill;
                limbs[3] = fill;
            }
            return Value::decimal(limbs, scale);
        }
        case NANOARROW_TYPE_BOOL: return Value::boolean(ArrowArrayViewGetIntUnsafe(view, at) != 0);
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64:
            return Value::integer(ArrowArrayViewGetIntUnsafe(view, at));
        case NANOARROW_TYPE_DURATION: return Value::duration(ArrowArrayViewGetIntUnsafe(view, at), unit);
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            return Value::uinteger(ArrowArrayViewGetUIntUnsafe(view, at));
        case NANOARROW_TYPE_HALF_FLOAT:
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE:
            return Value::real(ArrowArrayViewGetDoubleUnsafe(view, at));
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            const auto sv = ArrowArrayViewGetStringUnsafe(view, at);
            return Value::view(std::string_view(sv.data, static_cast<std::size_t>(sv.size_bytes)));
        }
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_FIXED_SIZE_BINARY: {
            const auto bv = ArrowArrayViewGetBytesUnsafe(view, at);
            return Value::view(std::string_view(static_cast<const char*>(bv.data.data), static_cast<std::size_t>(bv.size_bytes)),
                               Kind::Binary);
        }
        case NANOARROW_TYPE_DATE32: return Value::date(ArrowArrayViewGetIntUnsafe(view, at));
        case NANOARROW_TYPE_DATE64: return Value::timestamp(ArrowArrayViewGetIntUnsafe(view, at) * 1000000LL);
        case NANOARROW_TYPE_TIMESTAMP: {
            Value t = Value::timestamp(ArrowArrayViewGetIntUnsafe(view, at) * unit_to_ns(unit));
            t.unit = unit;
            return t;
        }
        default:
            // Bound only under a null test, where the value itself is never read: any non-null.
            return Value::boolean(true);
    }
}

/// Floats as Lance's filters order them (scan, BTree and Bitmap alike): IEEE total order with
/// the zeros equal -- -NaN < -inf < ... < -0 = +0 < ... < +inf < +NaN.
int float_way(double x, double y) {
    const bool x_nan = std::isnan(x);
    const bool y_nan = std::isnan(y);
    if (!x_nan && !y_nan) {
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    const int rx = x_nan ? (std::signbit(x) ? -1 : 1) : 0;
    const int ry = y_nan ? (std::signbit(y) ? -1 : 1) : 0;
    return rx < ry ? -1 : (rx > ry ? 1 : 0);
}

/// Exact limbs and scale of an integer or decimal value; false for anything else.
bool exact_limbs(const Value& v, Limbs256& out, int32_t& scale) {
    switch (v.kind) {
        case Kind::Decimal:
            out = v.dec;
            scale = v.scale;
            return true;
        case Kind::Int:
            out = {static_cast<std::uint64_t>(v.i), v.i < 0 ? ~std::uint64_t{0} : 0, v.i < 0 ? ~std::uint64_t{0} : 0,
                   v.i < 0 ? ~std::uint64_t{0} : 0};
            scale = 0;
            return true;
        case Kind::UInt:
            out = {v.u, 0, 0, 0};
            scale = 0;
            return true;
        default: return false;
    }
}

/// Exact three-way comparison of two decimals, each as limbs at its scale: rescaled to the larger
/// scale in full precision, so no digit is lost.
int compare_decimal(const Limbs256& a, int32_t sa, const Limbs256& b, int32_t sb) {
    const bool na = limbs_negative(a);
    const bool nb = limbs_negative(b);
    if (na != nb) {
        return na ? -1 : 1;
    }
    Magnitude ma = limbs_magnitude(a);
    Magnitude mb = limbs_magnitude(b);
    const int32_t s = std::max(sa, sb);
    mag_mul_pow10(ma, s - sa);
    mag_mul_pow10(mb, s - sb);
    const int c = mag_cmp(ma, mb);
    return na ? -c : c;
}

/// The sign of x * k - y for k >= 1, without computing x * k (which can overflow): y is split as
/// qy * k + ry with 0 <= ry < k, and x is compared with qy first.
int compare_scaled(std::int64_t x, std::int64_t k, std::int64_t y) {
    std::int64_t qy = y / k;
    std::int64_t ry = y % k;
    if (ry < 0) {
        ry += k;
        qy -= 1;
    }
    if (x > qy) return 1;
    if (x < qy) return -1;
    return ry > 0 ? -1 : 0;
}

/// Exact three-way comparison of count_a units of unit_a with count_b units of unit_b. Each unit is
/// a power of ten of nanoseconds, so the coarser count is compared against the finer one scaled.
int compare_duration(std::int64_t count_a, ArrowTimeUnit unit_a, std::int64_t count_b, ArrowTimeUnit unit_b) {
    const std::int64_t fa = unit_to_ns(unit_a);
    const std::int64_t fb = unit_to_ns(unit_b);
    if (fa == fb) {
        return count_a < count_b ? -1 : (count_a > count_b ? 1 : 0);
    }
    if (fa > fb) {
        return compare_scaled(count_a, fa / fb, count_b);
    }
    return -compare_scaled(count_b, fb / fa, count_a);
}

/// -1, 0, 1, or nullopt when the two cannot be compared.
std::optional<int> compare(const Value& a, const Value& b) {
    if (a.duration() || b.duration()) {
        if (!a.duration() || !b.duration()) {
            return std::nullopt;  // a duration against a non-duration: refused at bind, never matches here
        }
        return compare_duration(a.i, a.unit, b.i, b.unit);
    }
    if (a.kind == Kind::Decimal || b.kind == Kind::Decimal) {
        // A float is compared as a double (arrow_cast(x, 'Float64')); integers and decimals exactly.
        if (a.kind == Kind::Double || b.kind == Kind::Double) {
            return float_way(a.as_double(), b.as_double());
        }
        Limbs256 x{};
        Limbs256 y{};
        int32_t sx = 0;
        int32_t sy = 0;
        if (!exact_limbs(a, x, sx) || !exact_limbs(b, y, sy)) {
            return std::nullopt;
        }
        return compare_decimal(x, sx, y, sy);
    }
    if (a.numeric() && b.numeric()) {
        if (a.kind == Kind::Double || b.kind == Kind::Double) {
            return float_way(a.as_double(), b.as_double());
        }
        if (a.kind == Kind::Int && b.kind == Kind::Int) {
            return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
        }
        if (a.kind == Kind::UInt && b.kind == Kind::UInt) {
            return a.u < b.u ? -1 : (a.u > b.u ? 1 : 0);
        }
        // Int against UInt.
        if (a.kind == Kind::Int) {
            if (a.i < 0) {
                return -1;
            }
            const auto x = static_cast<std::uint64_t>(a.i);
            return x < b.u ? -1 : (x > b.u ? 1 : 0);
        }
        if (b.i < 0) {
            return 1;
        }
        const auto y = static_cast<std::uint64_t>(b.i);
        return a.u < y ? -1 : (a.u > y ? 1 : 0);
    }
    if ((a.kind == Kind::String || a.kind == Kind::Binary) && (b.kind == Kind::String || b.kind == Kind::Binary)) {
        const int c = a.s.compare(b.s);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.kind == Kind::Bool && b.kind == Kind::Bool) {
        return a.b == b.b ? 0 : (a.b ? 1 : -1);
    }
    if (a.temporal() && b.temporal()) {
        const auto x = a.as_ns();
        const auto y = b.as_ns();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    return std::nullopt;
}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Null: return "null";
        case Kind::Bool: return "boolean";
        case Kind::Int:
        case Kind::UInt: return "integer";
        case Kind::Double: return "float";
        case Kind::String: return "string";
        case Kind::Binary: return "binary";
        case Kind::Date: return "date";
        case Kind::Timestamp: return "timestamp";
        case Kind::List: return "list";
        case Kind::Decimal: return "decimal";
        case Kind::Duration: return "duration";
    }
    return "?";
}

/// SQL LIKE: % any run, _ one character, \ escapes.
bool like(std::string_view s, std::string_view p, bool fold) {
    auto eq = [&](char a, char b) {
        return fold ? std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)) : a == b;
    };
    std::size_t si = 0;
    std::size_t pi = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '\\' && pi + 1 < p.size() && eq(p[pi + 1], s[si])) {
            pi += 2;
            ++si;
        } else if (pi < p.size() && p[pi] != '%' && p[pi] != '\\' && (p[pi] == '_' || eq(p[pi], s[si]))) {
            ++pi;
            ++si;
        } else if (pi < p.size() && p[pi] == '%') {
            star = pi++;
            mark = si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '%') {
        ++pi;
    }
    return pi == p.size();
}

std::size_t utf8_length(std::string_view s) {
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++n;
        }
    }
    return n;
}

Value to_string_value(const Value& v) {
    switch (v.kind) {
        case Kind::String:
        case Kind::Binary: return v;
        case Kind::Null: return v;
        case Kind::Date: {
            std::int64_t y = 0;
            unsigned m = 0;
            unsigned d = 0;
            civil_from_days(v.i, y, m, d);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(y), m % 100U, d % 100U);
            return Value::string(buf);
        }
        default: {
            auto text = format_value(v);
            return Value::string(text);
        }
    }
}

Value eval(const Node& n, int64_t row, Context& ctx);

/// A time unit as pylance prints it in a type: s, ms, µs, ns.
const char* time_unit_abbrev(ArrowTimeUnit unit) {
    switch (unit) {
        case NANOARROW_TIME_UNIT_SECOND: return "s";
        case NANOARROW_TIME_UNIT_MILLI: return "ms";
        case NANOARROW_TIME_UNIT_MICRO: return "µs";
        default: return "ns";
    }
}

/// A projected duration, timestamp or date sum or difference, as pylance 12 types it (EXPR_TYPES C3):
/// duration +/- duration of one unit; timestamp +/- duration (the timestamp's unit); timestamp -
/// timestamp (the coarser unit); date - date (days). Mixed-unit durations are refused: pylance gives
/// a month_day_nano_interval. `handled` is false for other operands (the caller keeps its rules).
Value temporal_arith(Op op, const Value& a, const Value& b, Context& ctx, bool& handled) {
    handled = true;
    const bool addsub = op == Op::Add || op == Op::Sub;
    if (addsub && a.kind == Kind::Duration && b.kind == Kind::Duration) {
        if (a.unit != b.unit) {
            return ctx.fail(std::string("nanolance refuses mixed-unit duration arithmetic (Duration(") +
                            time_unit_abbrev(a.unit) + ") " + op_text(op) + " Duration(" + time_unit_abbrev(b.unit) +
                            ")): pylance returns a month_day_nano_interval");
        }
        const auto x = static_cast<std::uint64_t>(a.i);
        const auto y = static_cast<std::uint64_t>(b.i);
        return Value::duration(static_cast<std::int64_t>(op == Op::Add ? x + y : x - y), a.unit);
    }
    const bool ts_dur = a.kind == Kind::Timestamp && b.kind == Kind::Duration;
    if (addsub && (ts_dur || (op == Op::Add && a.kind == Kind::Duration && b.kind == Kind::Timestamp))) {
        const Value& ts = ts_dur ? a : b;
        const Value& d = ts_dur ? b : a;
        const auto base = static_cast<std::uint64_t>(ts.i);
        const auto shift = static_cast<std::uint64_t>(d.i) * static_cast<std::uint64_t>(unit_to_ns(d.unit));
        Value r = Value::timestamp(static_cast<std::int64_t>(op == Op::Add ? base + shift : base - shift));
        r.unit = ts.unit;
        return r;
    }
    if (op == Op::Sub && a.kind == Kind::Timestamp && b.kind == Kind::Timestamp) {
        // both sides are cut to the coarser unit, then subtracted
        const ArrowTimeUnit unit = std::min(a.unit, b.unit);
        const std::int64_t per = unit_to_ns(unit);
        return Value::duration(a.i / per - b.i / per, unit);
    }
    if (op == Op::Sub && a.kind == Kind::Date && b.kind == Kind::Date) {
        return Value::integer(a.i - b.i);
    }
    if (addsub && (a.kind == Kind::Date || b.kind == Kind::Date) && (a.kind == Kind::Duration || b.kind == Kind::Duration)) {
        const auto name = [](const Value& v) {
            return v.kind == Kind::Date ? std::string("Date32") : "Duration(" + std::string(time_unit_abbrev(v.unit)) + ")";
        };
        return ctx.fail("Invalid user input: Error during planning: Cannot coerce arithmetic expression " + name(a) +
                        " " + op_text(op) + " " + name(b) + " to valid types");
    }
    handled = false;
    return Value::null();
}

Value arith(Op op, const Value& a, const Value& b, Context& ctx) {
    if (a.is_null() || b.is_null()) {
        return Value::null();
    }
    if (ctx.projection) {
        bool handled = false;
        Value r = temporal_arith(op, a, b, ctx, handled);
        if (handled) {
            return r;
        }
    }
    if (!a.numeric() || !b.numeric()) {
        return ctx.fail(std::string("cannot apply '") + op_text(op) + "' to " + kind_name(a.kind) + " and " +
                        kind_name(b.kind));
    }
    if (a.kind == Kind::Double || b.kind == Kind::Double) {
        const double x = a.as_double();
        const double y = b.as_double();
        switch (op) {
            case Op::Add: return Value::real(x + y);
            case Op::Sub: return Value::real(x - y);
            case Op::Mul: return Value::real(x * y);
            case Op::Div: return Value::real(x / y);
            default: return Value::real(std::fmod(x, y));
        }
    }
    const std::int64_t x = a.kind == Kind::Int ? a.i : static_cast<std::int64_t>(a.u);
    const std::int64_t y = b.kind == Kind::Int ? b.i : static_cast<std::int64_t>(b.u);
    switch (op) {
        case Op::Add: return Value::integer(static_cast<std::int64_t>(static_cast<std::uint64_t>(x) + static_cast<std::uint64_t>(y)));
        case Op::Sub: return Value::integer(static_cast<std::int64_t>(static_cast<std::uint64_t>(x) - static_cast<std::uint64_t>(y)));
        case Op::Mul: return Value::integer(static_cast<std::int64_t>(static_cast<std::uint64_t>(x) * static_cast<std::uint64_t>(y)));
        case Op::Div:
            if (y == 0) {
                return ctx.fail("division by zero");
            }
            return Value::integer(x / y);
        default:
            if (y == 0) {
                return ctx.fail("division by zero");
            }
            return Value::integer(x % y);
    }
}

Value cast_value(const Value& v, const std::string& type, Context& ctx) {
    if (v.is_null()) {
        return v;
    }
    const auto first_word = type.substr(0, type.find(' '));
    const auto first = first_word == "datetime" ? std::string("timestamp") : first_word;
    if (first == "int" || first == "integer" || first == "bigint" || first == "smallint" || first == "tinyint" ||
        first == "int8" || first == "int16" || first == "int32" || first == "int64" || first == "uint8" ||
        first == "uint16" || first == "uint32" || first == "uint64") {
        if (v.kind == Kind::Int) {
            return v;
        }
        if (v.kind == Kind::Duration) {
            return Value::integer(v.i);
        }
        if (v.kind == Kind::UInt) {
            return Value::integer(static_cast<std::int64_t>(v.u));
        }
        if (v.kind == Kind::Double) {
            return Value::integer(static_cast<std::int64_t>(v.d));
        }
        if (v.kind == Kind::Bool) {
            return Value::integer(v.b ? 1 : 0);
        }
        if (v.kind == Kind::String) {
            std::int64_t out = 0;
            const auto [p, ec] = std::from_chars(v.s.data(), v.s.data() + v.s.size(), out);
            if (ec == std::errc() && p == v.s.data() + v.s.size()) {
                return Value::integer(out);
            }
        }
    } else if (first == "float" || first == "double" || first == "real" || first == "decimal" || first == "numeric") {
        if (v.numeric() || v.kind == Kind::Decimal) {
            return Value::real(v.as_double());
        }
        if (v.kind == Kind::Duration) {
            return Value::real(static_cast<double>(v.i));
        }
        if (v.kind == Kind::String) {
            char* end = nullptr;
            const std::string text(v.s);
            const double d = std::strtod(text.c_str(), &end);
            if (end != nullptr && *end == '\0' && !text.empty()) {
                return Value::real(d);
            }
        }
    } else if (first == "varchar" || first == "string" || first == "text" || first == "char") {
        return to_string_value(v);
    } else if (first == "boolean" || first == "bool") {
        if (v.kind == Kind::Bool) {
            return v;
        }
        if (v.numeric()) {
            return Value::boolean(v.as_double() != 0.0);
        }
        if (v.kind == Kind::String) {
            const auto t = lower(std::string(v.s));
            if (t == "true" || t == "false") {
                return Value::boolean(t == "true");
            }
        }
    } else if (first == "binary" || first == "bytea" || first == "varbinary") {
        if (v.kind == Kind::String || v.kind == Kind::Binary) {
            Value out = v;
            out.kind = Kind::Binary;
            return out;
        }
    } else if (first == "date" || first == "timestamp") {
        if (v.temporal()) {
            return first == "date" ? Value::date(v.as_ns() / kNsPerDay) : Value::timestamp(v.as_ns());
        }
        if (v.kind == Kind::String) {
            std::int64_t ns = 0;
            bool date_only = false;
            if (parse_datetime(v.s, ns, date_only)) {
                return first == "date" ? Value::date(ns / kNsPerDay) : Value::timestamp(ns);
            }
        }
    } else {
        return ctx.fail("CAST to '" + type + "' is not supported");
    }
    return ctx.fail("cannot CAST " + format_value(v) + " AS " + type);
}

Value truth(const Value& v, Context& ctx) {
    if (v.is_null() || v.kind == Kind::Bool) {
        return v;
    }
    return ctx.fail(std::string("expected a boolean, got ") + kind_name(v.kind));
}

/// Lance's json_* functions (lance-datafusion's udf/json.rs) over JSONB values: a JSON null reads as
/// SQL NULL; a value the typed getters cannot convert fails the query, as there.
Value eval_json(const Node& n, int64_t row, Context& ctx) {
    namespace jb = nano_lance::jsonb;
    const auto& f = n.name;
    const Value doc = eval(*n.args[0], row, ctx);
    if (doc.is_null() || ctx.failed) {
        return Value::null();
    }
    if (doc.kind != Kind::Binary) {
        return ctx.fail(f + "() needs a JSON column (or json_get's result) as its first argument");
    }
    jb::Item root;
    if (!jb::Item::root(doc.s, root)) {
        return ctx.fail(f + "(): the value is not JSONB");
    }
    const Value arg = eval(*n.args[1], row, ctx);
    if (arg.is_null() || ctx.failed) {
        return Value::null();
    }
    if (arg.kind != Kind::String) {
        return ctx.fail(f + "() needs a string as its second argument");
    }
    std::string error;
    auto text_of = [&](const jb::Item& item, std::string& out) {
        out.clear();
        if (!item.text(out, error)) {
            ctx.fail(error);
            return false;
        }
        return true;
    };
    if (f == "json_get" || f.rfind("json_get_", 0) == 0) {
        // A key of an object; an index of an array; anything else is no value.
        jb::Item v;
        bool found = false;
        if (root.type() == jb::Item::Type::Object) {
            found = root.get(arg.s, v);
        } else if (root.type() == jb::Item::Type::Array) {
            std::size_t index = 0;
            const auto [p, ec] = std::from_chars(arg.s.data(), arg.s.data() + arg.s.size(), index);
            found = ec == std::errc() && p == arg.s.data() + arg.s.size() && root.at(index, v);
        }
        if (!found) {
            return Value::null();
        }
        if (f == "json_get") {
            return Value::string(v.document(), Kind::Binary);
        }
        if (v.type() == jb::Item::Type::Null) {
            return Value::null();
        }
        const bool is_bool = v.type() == jb::Item::Type::True || v.type() == jb::Item::Type::False;
        const bool truth = v.type() == jb::Item::Type::True;
        jb::Number num;
        const bool is_number = v.number(num);
        const bool is_string = v.type() == jb::Item::Type::String;
        if (f == "json_get_string") {
            if (is_bool) {
                return Value::string(truth ? "true" : "false");
            }
            if (is_string) {
                return Value::string(std::string(v.string()));
            }
            std::string out;
            if (is_number && text_of(v, out)) {
                return Value::string(out);
            }
            return ctx.fail("Failed to convert to string: InvalidCast");
        }
        if (f == "json_get_int") {
            if (is_bool) {
                return Value::integer(truth ? 1 : 0);
            }
            auto from_double = [&](double d) -> Value {
                const double r = std::round(d);  // half away from zero, as the crate rounds
                if (!std::isfinite(r) || r < -9223372036854775808.0 || r >= 9223372036854775808.0) {
                    return ctx.fail("Failed to convert to integer: InvalidCast");
                }
                return Value::integer(static_cast<std::int64_t>(r));
            };
            if (is_number) {
                if (num.kind == jb::Number::Kind::Int) {
                    return Value::integer(num.i);
                }
                if (num.kind == jb::Number::Kind::UInt) {
                    if (num.u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                        return ctx.fail("Failed to convert to integer: InvalidCast");
                    }
                    return Value::integer(static_cast<std::int64_t>(num.u));
                }
                return from_double(num.f);
            }
            if (is_string) {
                std::string_view s = v.string();
                if (!s.empty() && s.front() == '+') {
                    s.remove_prefix(1);
                }
                std::int64_t out = 0;
                const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
                if (ec == std::errc() && p == s.data() + s.size() && !s.empty()) {
                    return Value::integer(out);
                }
                const std::string text(v.string());
                char* end = nullptr;
                const double d = std::strtod(text.c_str(), &end);
                if (!text.empty() && end != nullptr && *end == '\0') {
                    return from_double(d);
                }
            }
            return ctx.fail("Failed to convert to integer: InvalidCast");
        }
        if (f == "json_get_float") {
            if (is_bool) {
                return Value::real(truth ? 1.0 : 0.0);
            }
            if (is_number) {
                return Value::real(num.as_double());
            }
            if (is_string) {
                const std::string text(v.string());
                char* end = nullptr;
                const double d = std::strtod(text.c_str(), &end);
                if (!text.empty() && end != nullptr && *end == '\0') {
                    return Value::real(d);
                }
            }
            return ctx.fail("Failed to convert to float: InvalidCast");
        }
        if (f == "json_get_bool") {
            if (is_bool) {
                return Value::boolean(truth);
            }
            if (is_string) {
                const auto s = lower(std::string(v.string()));
                if (s == "true" || s == "yes" || s == "false" || s == "no") {
                    return Value::boolean(s == "true" || s == "yes");
                }
            }
            return ctx.fail("Failed to convert to boolean: InvalidCast");
        }
        return ctx.fail("function '" + f + "' is not supported in nanolance filters");
    }
    // The rest take a JSONPath.
    std::vector<jb::Item> selected;
    if (!jb::select(root, arg.s, selected, error)) {
        return ctx.fail(error);
    }
    if (f == "json_exists") {
        return Value::boolean(!selected.empty());
    }
    if (f == "json_extract") {
        std::string out;
        if (selected.empty()) {
            return Value::null();
        }
        return text_of(selected.front(), out) ? Value::string(out) : Value::null();
    }
    if (f == "json_array_length") {
        if (selected.empty()) {
            return Value::null();
        }
        if (selected.front().type() != jb::Item::Type::Array) {
            return ctx.fail("Path '" + std::string(arg.s) + "' does not point to an array");
        }
        return Value::integer(static_cast<std::int64_t>(selected.front().size()));
    }
    if (f == "json_array_contains") {
        const Value wanted = eval(*n.args[2], row, ctx);
        if (wanted.is_null() || ctx.failed) {
            return Value::null();
        }
        const std::string needle(wanted.s);
        const std::string quoted = "\"" + needle + "\"";
        std::string elem;
        for (const auto& value : selected) {
            for (std::size_t k = 0; value.type() == jb::Item::Type::Array && k < value.size(); ++k) {
                jb::Item item;
                if (value.at(k, item) && text_of(item, elem) && (elem == needle || elem == quoted)) {
                    return Value::boolean(true);
                }
            }
        }
        return Value::boolean(false);
    }
    return ctx.fail("function '" + f + "' is not supported in nanolance filters");
}

Value eval(const Node& n, int64_t row, Context& ctx) {
    switch (n.op) {
        case Op::Literal: return n.literal;
        case Op::Column: return read_column(n.column, row, ctx);
        case Op::And: {
            const Value a = truth(eval(*n.args[0], row, ctx), ctx);
            if (a.kind == Kind::Bool && !a.b) {
                return a;
            }
            const Value b = truth(eval(*n.args[1], row, ctx), ctx);
            if (b.kind == Kind::Bool && !b.b) {
                return b;
            }
            return a.is_null() || b.is_null() ? Value::null() : Value::boolean(true);
        }
        case Op::Or: {
            const Value a = truth(eval(*n.args[0], row, ctx), ctx);
            if (a.kind == Kind::Bool && a.b) {
                return a;
            }
            const Value b = truth(eval(*n.args[1], row, ctx), ctx);
            if (b.kind == Kind::Bool && b.b) {
                return b;
            }
            return a.is_null() || b.is_null() ? Value::null() : Value::boolean(false);
        }
        case Op::Not: {
            const Value a = truth(eval(*n.args[0], row, ctx), ctx);
            return a.is_null() ? a : Value::boolean(!a.b);
        }
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
            const Value a = eval(*n.args[0], row, ctx);
            const Value b = eval(*n.args[1], row, ctx);
            if (a.is_null() || b.is_null()) {
                return Value::null();
            }
            const auto c = compare(a, b);
            if (!c) {
                return ctx.fail(std::string("cannot compare ") + kind_name(a.kind) + " with " + kind_name(b.kind));
            }
            switch (n.op) {
                case Op::Eq: return Value::boolean(*c == 0);
                case Op::Ne: return Value::boolean(*c != 0);
                case Op::Lt: return Value::boolean(*c < 0);
                case Op::Le: return Value::boolean(*c <= 0);
                case Op::Gt: return Value::boolean(*c > 0);
                default: return Value::boolean(*c >= 0);
            }
        }
        case Op::IsNull: return Value::boolean(eval(*n.args[0], row, ctx).is_null());
        case Op::IsNotNull: return Value::boolean(!eval(*n.args[0], row, ctx).is_null());
        case Op::IsTrue:
        case Op::IsFalse:
        case Op::IsNotTrue:
        case Op::IsNotFalse: {
            const Value a = truth(eval(*n.args[0], row, ctx), ctx);
            const bool is_true = a.kind == Kind::Bool && a.b;
            const bool is_false = a.kind == Kind::Bool && !a.b;
            switch (n.op) {
                case Op::IsTrue: return Value::boolean(is_true);
                case Op::IsFalse: return Value::boolean(is_false);
                case Op::IsNotTrue: return Value::boolean(!is_true);
                default: return Value::boolean(!is_false);
            }
        }
        case Op::In:
        case Op::NotIn: {
            const Value a = eval(*n.args[0], row, ctx);
            if (a.is_null()) {
                return a;
            }
            bool saw_null = false;
            for (std::size_t k = 1; k < n.args.size(); ++k) {
                const Value b = eval(*n.args[k], row, ctx);
                if (b.is_null()) {
                    saw_null = true;
                    continue;
                }
                const auto c = compare(a, b);
                if (!c) {
                    return ctx.fail(std::string("cannot compare ") + kind_name(a.kind) + " with " + kind_name(b.kind));
                }
                if (*c == 0) {
                    return Value::boolean(n.op == Op::In);
                }
            }
            return saw_null ? Value::null() : Value::boolean(n.op == Op::NotIn);
        }
        case Op::Between:
        case Op::NotBetween: {
            const Value a = eval(*n.args[0], row, ctx);
            const Value lo = eval(*n.args[1], row, ctx);
            const Value hi = eval(*n.args[2], row, ctx);
            if (a.is_null() || lo.is_null() || hi.is_null()) {
                return Value::null();
            }
            const auto c1 = compare(a, lo);
            const auto c2 = compare(a, hi);
            if (!c1 || !c2) {
                return ctx.fail("cannot compare in BETWEEN");
            }
            const bool in = *c1 >= 0 && *c2 <= 0;
            return Value::boolean(n.op == Op::Between ? in : !in);
        }
        case Op::Like:
        case Op::NotLike:
        case Op::ILike:
        case Op::NotILike: {
            const Value a = eval(*n.args[0], row, ctx);
            const Value p = eval(*n.args[1], row, ctx);
            if (a.is_null() || p.is_null()) {
                return Value::null();
            }
            if ((a.kind != Kind::String && a.kind != Kind::Binary) || p.kind != Kind::String) {
                return ctx.fail("LIKE needs a string");
            }
            const bool m = like(a.s, p.s, n.op == Op::ILike || n.op == Op::NotILike);
            return Value::boolean((n.op == Op::Like || n.op == Op::ILike) ? m : !m);
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
            return arith(n.op, eval(*n.args[0], row, ctx), eval(*n.args[1], row, ctx), ctx);
        case Op::Neg: {
            const Value a = eval(*n.args[0], row, ctx);
            if (a.is_null()) {
                return a;
            }
            if (a.kind == Kind::Int) {
                return Value::integer(-a.i);
            }
            if (a.kind == Kind::Double) {
                return Value::real(-a.d);
            }
            if (a.kind == Kind::UInt) {
                return Value::integer(-static_cast<std::int64_t>(a.u));
            }
            if (a.kind == Kind::Duration && ctx.projection) {
                return ctx.fail("Invalid user input: Error during planning: Negation only supports numeric, interval "
                                "and timestamp types");
            }
            return ctx.fail("cannot negate a " + std::string(kind_name(a.kind)));
        }
        case Op::Concat: {
            const Value a = eval(*n.args[0], row, ctx);
            const Value b = eval(*n.args[1], row, ctx);
            if (a.is_null() || b.is_null()) {
                return Value::null();
            }
            const Value x = to_string_value(a);
            const Value y = to_string_value(b);
            return Value::string(std::string(x.s) + std::string(y.s));
        }
        case Op::Cast:
            if (!n.source.empty() && !ctx.projection) {  // a column's arrow_cast to decimal: projections only
                return ctx.fail("invalid filter: arrow_cast to '" + n.source +
                                "' takes a number literal in nanolance filters");
            }
            return cast_value(eval(*n.args[0], row, ctx), n.name, ctx);
        case Op::Func: {
            const auto& f = n.name;
            if (n.family == Node::Family::Json) {
                return eval_json(n, row, ctx);
            }
            if (f == "_cast_list_f16") {  // the items are rounded to Float16 when they are appended
                return eval(*n.args[0], row, ctx);
            }
            if (n.family == Node::Family::Regexp) {
                const Value a = eval(*n.args[0], row, ctx);
                if (a.is_null()) {
                    return a;
                }
                if (a.kind != Kind::String) {
                    return ctx.fail(f + "() needs a string");
                }
                std::shared_ptr<const std::regex> re = n.regex;
                if (!re) {
                    const Value p = eval(*n.args[1], row, ctx);
                    if (p.is_null()) {
                        return p;
                    }
                    try {
                        re = std::make_shared<const std::regex>(std::string(p.s));
                    } catch (const std::regex_error& e) {
                        return ctx.fail("invalid regular expression: " + std::string(e.what()));
                    }
                }
                return Value::boolean(std::regex_search(a.s.begin(), a.s.end(), *re));
            }
            if (f == "coalesce") {
                for (const auto& a : n.args) {
                    Value v = eval(*a, row, ctx);
                    if (!v.is_null()) {
                        return v;
                    }
                }
                return Value::null();
            }
            if (f == "is_null") {
                return Value::boolean(eval(*n.args[0], row, ctx).is_null());
            }
            if (f == "is_valid") {
                return Value::boolean(!eval(*n.args[0], row, ctx).is_null());
            }
            if (f == "invert") {
                const Value a = truth(eval(*n.args[0], row, ctx), ctx);
                return a.is_null() ? a : Value::boolean(!a.b);
            }
            if (f == "and_" || f == "or_" || f == "equal") {
                Node tmp;
                tmp.op = f == "and_" ? Op::And : (f == "or_" ? Op::Or : Op::Eq);
                // Evaluate through the operator of the same meaning, borrowing the arguments.
                const Value a = eval(*n.args[0], row, ctx);
                const Value b = eval(*n.args[1], row, ctx);
                if (tmp.op == Op::Eq) {
                    if (a.is_null() || b.is_null()) {
                        return Value::null();
                    }
                    const auto c = compare(a, b);
                    return c ? Value::boolean(*c == 0) : ctx.fail("cannot compare in equal()");
                }
                const Value x = truth(a, ctx);
                const Value y = truth(b, ctx);
                if (tmp.op == Op::And) {
                    if ((x.kind == Kind::Bool && !x.b) || (y.kind == Kind::Bool && !y.b)) {
                        return Value::boolean(false);
                    }
                    return x.is_null() || y.is_null() ? Value::null() : Value::boolean(true);
                }
                if ((x.kind == Kind::Bool && x.b) || (y.kind == Kind::Bool && y.b)) {
                    return Value::boolean(true);
                }
                return x.is_null() || y.is_null() ? Value::null() : Value::boolean(false);
            }
            if (is_array_function(f)) {
                // DataFusion's array_has / array_has_any / array_has_all: a NULL list is NULL; a NULL
                // element matches nothing.
                const Value list = eval(*n.args[0], row, ctx);
                const Value b = eval(*n.args[1], row, ctx);
                if ((f == "array_has_all" || f == "list_has_all") && b.kind == Kind::List && b.items->empty()) {
                    return Value::boolean(true);  // as DataFusion folds it, a NULL list included
                }
                if (list.is_null() || b.is_null()) {
                    return Value::null();
                }
                if (list.kind != Kind::List) {
                    return ctx.fail(f + "() needs a list as its first argument");
                }
                const auto has = [&](const Value& v) {
                    if (v.is_null()) {
                        return false;
                    }
                    for (const auto& item : *list.items) {
                        if (!item.is_null()) {
                            const auto c = compare(item, v);
                            if (c && *c == 0) {
                                return true;
                            }
                        }
                    }
                    return false;
                };
                const bool any = f == "array_has_any" || f == "list_has_any";
                const bool all = f == "array_has_all" || f == "list_has_all";
                if (!any && !all) {
                    return Value::boolean(has(b));
                }
                if (b.kind != Kind::List) {
                    return ctx.fail(f + "() needs a list such as ['a', 'b'] as its second argument");
                }
                for (const auto& v : *b.items) {
                    if (has(v) == any) {
                        return Value::boolean(any);
                    }
                }
                return Value::boolean(all);
            }
            const Value a = eval(*n.args[0], row, ctx);
            if (a.is_null()) {
                return a;
            }
            if (f == "abs") {
                if (a.kind == Kind::Int) {
                    return Value::integer(a.i < 0 ? -a.i : a.i);
                }
                if (a.numeric()) {
                    return a.kind == Kind::UInt ? a : Value::real(std::fabs(a.d));
                }
                if (a.kind == Kind::Duration && ctx.projection) {
                    return ctx.fail(std::string("Invalid user input: Error during planning: Function 'abs' expects "
                                                "Numeric but received Duration(") +
                                    time_unit_abbrev(a.unit) + ")");
                }
                return ctx.fail("abs() needs a number");
            }
            if (a.kind != Kind::String) {
                return ctx.fail(f + "() needs a string");
            }
            if (f == "lower") {
                return Value::string(lower(std::string(a.s)));
            }
            if (f == "upper") {
                return Value::string(upper(std::string(a.s)));
            }
            if (f == "length" || f == "char_length" || f == "character_length") {
                return Value::integer(static_cast<std::int64_t>(utf8_length(a.s)));
            }
            const Value b = eval(*n.args[1], row, ctx);
            if (b.is_null()) {
                return b;
            }
            if (b.kind != Kind::String) {
                return ctx.fail(f + "() needs strings");
            }
            if (f == "starts_with") {
                return Value::boolean(a.s.substr(0, b.s.size()) == b.s);
            }
            if (f == "ends_with") {
                return Value::boolean(a.s.size() >= b.s.size() && a.s.substr(a.s.size() - b.s.size()) == b.s);
            }
            return Value::boolean(a.s.find(b.s) != std::string_view::npos);  // contains
        }
    }
    return ctx.fail("unsupported expression");
}

/// Append `v` to `out` (being built for `type`), converting as CAST would.
/// The IEEE binary16 bits of `f`, rounded to nearest, ties to even (subnormals and overflow included).
/// nanoarrow's ArrowFloatToHalfFloat truncates and flushes subnormals, which numpy does not.
static std::uint16_t float16_bits(float f) {
    std::uint32_t x = 0;
    std::memcpy(&x, &f, sizeof x);
    const std::uint32_t sign = (x >> 16) & 0x8000U;
    const std::uint32_t mag = x & 0x7fffffffU;
    if (mag >= 0x7f800000U) {  // infinity, or NaN with its top mantissa bit kept
        return static_cast<std::uint16_t>(sign | 0x7c00U | (mag > 0x7f800000U ? 0x200U : 0U));
    }
    if (mag >= 0x477ff000U) {  // from 65520 up, rounds to infinity
        return static_cast<std::uint16_t>(sign | 0x7c00U);
    }
    if (mag <= 0x33000000U) {  // up to half of the smallest subnormal rounds to zero
        return static_cast<std::uint16_t>(sign);
    }
    if (mag < 0x38800000U) {  // subnormal in binary16: units of 2^-24
        const std::int32_t exponent = static_cast<std::int32_t>(mag >> 23) - 127;  // -25 .. -15 here
        const std::uint32_t significand = (mag & 0x7fffffU) | 0x800000U;
        const auto shift = static_cast<std::uint32_t>(-exponent - 1);  // 2^-24 units: 14 .. 24 bits
        std::uint32_t q = significand >> shift;
        const std::uint32_t rem = significand & ((std::uint32_t{1} << shift) - 1);
        const std::uint32_t half = std::uint32_t{1} << (shift - 1);
        if (rem > half || (rem == half && (q & 1U) != 0)) {
            ++q;  // a carry to 0x400 is the smallest normal, as the encoding has it
        }
        return static_cast<std::uint16_t>(sign | q);
    }
    const std::uint32_t exponent = (mag >> 23) - 127 + 15;
    std::uint32_t q = (exponent << 10) | ((mag & 0x7fffffU) >> 13);
    const std::uint32_t rem = mag & 0x1fffU;
    if (rem > 0x1000U || (rem == 0x1000U && (q & 1U) != 0)) {
        ++q;  // a carry can reach infinity, as it should
    }
    return static_cast<std::uint16_t>(sign | q);
}

bool append_value(ArrowArray& out, const ArrowSchemaView& type, const Value& v, Context& ctx) {
    if (v.is_null()) {
        return ArrowArrayAppendNull(&out, 1) == NANOARROW_OK;
    }
    switch (type.type) {
        case NANOARROW_TYPE_BOOL: {
            const Value b = cast_value(v, "boolean", ctx);
            return !ctx.failed && ArrowArrayAppendInt(&out, b.b ? 1 : 0) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_DECIMAL128:
        case NANOARROW_TYPE_DECIMAL256: {
            Limbs256 limbs{};
            int32_t scale = 0;
            if (!exact_limbs(v, limbs, scale)) {
                ctx.fail("cannot store " + format_value(v) + " in a decimal column");
                return false;
            }
            const bool wide = type.type == NANOARROW_TYPE_DECIMAL256;
            Limbs256 rounded{};
            std::string error;
            if (!fit_decimal(limbs_magnitude(limbs), limbs_negative(limbs), scale, type.decimal_scale,
                             type.decimal_precision, wide ? "Decimal256" : "Decimal128", rounded, error)) {
                ctx.fail(error);
                return false;
            }
            ArrowDecimal dec{};
            ArrowDecimalInit(&dec, wide ? 256 : 128, type.decimal_precision, type.decimal_scale);
            for (std::size_t k = 0; k < 4; ++k) {
                dec.words[k] = rounded[k];
            }
            return ArrowArrayAppendDecimal(&out, &dec) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64: {
            const Value i = cast_value(v, "bigint", ctx);
            if (ctx.failed || ArrowArrayAppendInt(&out, i.i) != NANOARROW_OK) {
                if (!ctx.failed) {
                    ctx.fail("value " + format_value(v) + " does not fit the column");
                }
                return false;
            }
            return true;
        }
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64: {
            if (v.kind == Kind::UInt) {
                return ArrowArrayAppendUInt(&out, v.u) == NANOARROW_OK;
            }
            const Value i = cast_value(v, "bigint", ctx);
            if (ctx.failed || i.i < 0 || ArrowArrayAppendUInt(&out, static_cast<std::uint64_t>(i.i)) != NANOARROW_OK) {
                if (!ctx.failed) {
                    ctx.fail("value " + format_value(v) + " does not fit the column");
                }
                return false;
            }
            return true;
        }
        case NANOARROW_TYPE_HALF_FLOAT: {
            const Value d = cast_value(v, "double", ctx);
            if (ctx.failed) {
                return false;
            }
            // The element as ArrowArrayAppendDouble writes it, with the bits from float16_bits.
            if (ArrowBufferAppendUInt16(ArrowArrayBuffer(&out, 1), float16_bits(static_cast<float>(d.d))) !=
                NANOARROW_OK) {
                return false;
            }
            struct ArrowBitmap* validity = ArrowArrayValidityBitmap(&out);
            if (validity->buffer.data != nullptr && ArrowBitmapAppend(validity, 1, 1) != NANOARROW_OK) {
                return false;
            }
            ++out.length;
            return true;
        }
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE: {
            const Value d = cast_value(v, "double", ctx);
            return !ctx.failed && ArrowArrayAppendDouble(&out, d.d) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            const Value s = to_string_value(v);
            ArrowStringView sv{s.s.data(), static_cast<int64_t>(s.s.size())};
            return ArrowArrayAppendString(&out, sv) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_FIXED_SIZE_BINARY: {
            if (v.kind != Kind::String && v.kind != Kind::Binary) {
                ctx.fail("a binary column needs a string or binary value");
                return false;
            }
            ArrowBufferView bv;
            bv.data.data = v.s.data();
            bv.size_bytes = static_cast<int64_t>(v.s.size());
            return ArrowArrayAppendBytes(&out, bv) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_DATE32: {
            const Value d = cast_value(v, "date", ctx);
            return !ctx.failed && ArrowArrayAppendInt(&out, d.i) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_DATE64: {
            const Value d = cast_value(v, "date", ctx);
            return !ctx.failed && ArrowArrayAppendInt(&out, d.i * 86400000LL) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_TIMESTAMP: {
            const Value t = cast_value(v, "timestamp", ctx);
            return !ctx.failed && ArrowArrayAppendInt(&out, t.as_ns() / unit_to_ns(type.time_unit)) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_DURATION: {
            if (!v.duration()) {
                const Value i = cast_value(v, "bigint", ctx);
                return !ctx.failed && ArrowArrayAppendInt(&out, i.i) == NANOARROW_OK;
            }
            // Exact when the column's unit is finer; a coarser unit truncates toward zero (Arrow's cast).
            const std::int64_t from = unit_to_ns(v.unit);
            const std::int64_t to = unit_to_ns(type.time_unit);
            std::int64_t count = v.i;
            if (from != to) {
                if (from > to) {
                    const std::int64_t k = from / to;
                    if (count > std::numeric_limits<std::int64_t>::max() / k || count < std::numeric_limits<std::int64_t>::min() / k) {
                        ctx.fail("value " + format_value(v) + " does not fit the column");
                        return false;
                    }
                    count *= k;
                } else {
                    count /= to / from;
                }
            }
            return ArrowArrayAppendInt(&out, count) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_FIXED_SIZE_LIST:
        case NANOARROW_TYPE_LIST:
        case NANOARROW_TYPE_LARGE_LIST: {
            if (v.kind != Kind::List) {
                ctx.fail("a list column needs a list value such as [1, 2]");
                return false;
            }
            if (type.type == NANOARROW_TYPE_FIXED_SIZE_LIST &&
                static_cast<int64_t>(v.items->size()) != type.fixed_size) {
                ctx.fail("a list of " + std::to_string(v.items->size()) + " values for a fixed_size_list of " +
                         std::to_string(type.fixed_size));
                return false;
            }
            ArrowSchemaView child;
            ArrowError aerr;
            if (ArrowSchemaViewInit(&child, type.schema->children[0], &aerr) != NANOARROW_OK) {
                ctx.fail("cannot read the list's item type");
                return false;
            }
            for (const auto& item : *v.items) {
                if (!append_value(*out.children[0], child, item, ctx)) {
                    return false;
                }
            }
            return ArrowArrayFinishElement(&out) == NANOARROW_OK;
        }
        default:
            ctx.fail("expressions cannot produce values of this column's type");
            return false;
    }
}

}  // namespace

Expression::Expression() = default;
Expression::~Expression() = default;
Expression::Expression(Expression&&) noexcept = default;
Expression& Expression::operator=(Expression&&) noexcept = default;

bool Expression::parse(std::string_view sql, Expression& out, std::string& error) {
    error.clear();
    std::vector<Token> tokens;
    if (!tokenize(sql, tokens, error)) {
        error = "invalid filter: " + error;
        return false;
    }
    if (tokens.size() == 1U) {
        error = "invalid filter: the expression is empty";
        return false;
    }
    Parser parser(std::move(tokens), error);
    auto root = parser.parse();
    if (!root) {
        if (error.empty()) {
            error = "invalid filter";
        }
        return false;
    }
    out.root_ = std::move(root);
    out.binding_.reset();
    return true;
}

std::vector<std::string> Expression::columns() const {
    std::vector<std::string> out;
    if (root_) {
        collect_columns(*root_, out);
    }
    return out;
}

bool Expression::bind(const ArrowSchema& schema, std::string& error) {
    error.clear();
    if (!root_) {
        error = "empty expression";
        return false;
    }
    if (!bind_node(*root_, schema, false, error) || !coerce(*root_, error)) {
        return false;
    }
    auto binding = std::make_unique<Binding>();
    if (ArrowSchemaDeepCopy(&schema, &binding->schema) != NANOARROW_OK) {
        error = "failed to copy the schema";
        return false;
    }
    binding_ = std::move(binding);
    return true;
}

namespace {

struct ViewGuard {
    ArrowArrayView view;
    bool init = false;
    ~ViewGuard() {
        if (init) {
            ArrowArrayViewReset(&view);
        }
    }
};

bool view_batch(const ArrowSchema& schema, const ArrowArray& batch, ViewGuard& guard, std::string& error) {
    ArrowError aerr;
    if (ArrowArrayViewInitFromSchema(&guard.view, &schema, &aerr) != NANOARROW_OK) {
        error = std::string("cannot view the batch: ") + aerr.message;
        return false;
    }
    guard.init = true;
    if (ArrowArrayViewSetArray(&guard.view, &batch, &aerr) != NANOARROW_OK) {
        error = std::string("cannot view the batch: ") + aerr.message;
        return false;
    }
    return true;
}

}  // namespace

namespace {

// ── vectorized filters ──────────────────────────────────────────────────────────────────────────
//
// The common filters -- a column against constants (=, <, IN, BETWEEN, IS NULL), joined by AND, OR
// and NOT -- evaluated a column at a time rather than a row at a time, with exactly eval()'s
// semantics. A state per row: 0 FALSE, 1 TRUE, 2 NULL. Anything else is left to eval().

constexpr std::uint8_t kFalse = 0;
constexpr std::uint8_t kTrue = 1;
constexpr std::uint8_t kNull = 2;

/// A top-level column's view, or null.
const ArrowArrayView* fast_column(const Node& n, const ArrowArrayView& batch) {
    if (n.op != Op::Column || n.column.indices.size() != 1U || n.column.item_type != NANOARROW_TYPE_UNINITIALIZED) {
        return nullptr;
    }
    return batch.children[n.column.indices.front()];
}

/// The comparison outcome wanted: c is compare(value, literal).
bool wanted(Op op, int c) {
    switch (op) {
        case Op::Eq: return c == 0;
        case Op::Ne: return c != 0;
        case Op::Lt: return c < 0;
        case Op::Le: return c <= 0;
        case Op::Gt: return c > 0;
        default: return c >= 0;
    }
}

Op flip(Op op) {
    switch (op) {
        case Op::Lt: return Op::Gt;
        case Op::Le: return Op::Ge;
        case Op::Gt: return Op::Lt;
        case Op::Ge: return Op::Le;
        default: return op;
    }
}

template <typename T>
int three_way(T a, T b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

/// `ways[r]` = compare(column value r, literal) for the non-null rows of a numeric, temporal or
/// string column -- false when the pair is not one this handles.
bool compare_column(const ArrowArrayView& v, ArrowType type, ArrowTimeUnit unit, const Value& lit, int64_t n,
                    std::vector<std::int8_t>& ways) {
    ways.assign(static_cast<std::size_t>(n), 0);
    const auto width = v.layout.element_size_bits[1] / 8;
    const std::uint8_t* data = v.buffer_views[1].data.as_uint8;
    const auto signed_at = [&](int64_t r) -> std::int64_t {
        const auto i = v.offset + r;
        switch (width) {
            case 1: return reinterpret_cast<const std::int8_t*>(data)[i];
            case 2: return reinterpret_cast<const std::int16_t*>(data)[i];
            case 4: return reinterpret_cast<const std::int32_t*>(data)[i];
            default: return reinterpret_cast<const std::int64_t*>(data)[i];
        }
    };
    const auto unsigned_at = [&](int64_t r) -> std::uint64_t {
        const auto i = v.offset + r;
        switch (width) {
            case 1: return data[i];
            case 2: return reinterpret_cast<const std::uint16_t*>(data)[i];
            case 4: return reinterpret_cast<const std::uint32_t*>(data)[i];
            default: return reinterpret_cast<const std::uint64_t*>(data)[i];
        }
    };
    switch (type) {
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64:
            if (lit.kind == Kind::Int) {
                for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(three_way(signed_at(r), lit.i));
            } else if (lit.kind == Kind::UInt) {
                for (int64_t r = 0; r < n; ++r) {
                    const auto x = signed_at(r);
                    ways[r] = static_cast<std::int8_t>(x < 0 ? -1 : three_way(static_cast<std::uint64_t>(x), lit.u));
                }
            } else if (lit.kind == Kind::Double) {
                for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(float_way(static_cast<double>(signed_at(r)), lit.d));
            } else {
                return false;
            }
            return true;
        case NANOARROW_TYPE_DURATION:
            // Unit-aware: each row's count in its own unit against the literal's, exactly.
            if (lit.kind != Kind::Duration) {
                return false;
            }
            for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(compare_duration(signed_at(r), unit, lit.i, lit.unit));
            return true;
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            if (lit.kind == Kind::UInt) {
                for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(three_way(unsigned_at(r), lit.u));
            } else if (lit.kind == Kind::Int) {
                for (int64_t r = 0; r < n; ++r) {
                    ways[r] = static_cast<std::int8_t>(lit.i < 0 ? 1 : three_way(unsigned_at(r), static_cast<std::uint64_t>(lit.i)));
                }
            } else if (lit.kind == Kind::Double) {
                for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(float_way(static_cast<double>(unsigned_at(r)), lit.d));
            } else {
                return false;
            }
            return true;
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE: {
            if (!lit.numeric()) {
                return false;
            }
            const double y = lit.as_double();
            if (type == NANOARROW_TYPE_FLOAT) {
                const auto* f = reinterpret_cast<const float*>(data) + v.offset;
                for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(float_way(f[r], y));
            } else {
                const auto* d = reinterpret_cast<const double*>(data) + v.offset;
                for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(float_way(d[r], y));
            }
            return true;
        }
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_DATE64:
        case NANOARROW_TYPE_TIMESTAMP: {
            if (!lit.temporal()) {
                return false;
            }
            const auto y = lit.as_ns();
            const std::int64_t scale = type == NANOARROW_TYPE_DATE32   ? kNsPerDay
                                       : type == NANOARROW_TYPE_DATE64 ? 1000000LL
                                                                       : unit_to_ns(unit);
            for (int64_t r = 0; r < n; ++r) ways[r] = static_cast<std::int8_t>(three_way(signed_at(r) * scale, y));
            return true;
        }
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
            if (lit.kind != Kind::String && lit.kind != Kind::Binary) {
                return false;
            }
            for (int64_t r = 0; r < n; ++r) {
                const auto sv = ArrowArrayViewGetStringUnsafe(&v, r);
                const int c = std::string_view(sv.data, static_cast<std::size_t>(sv.size_bytes)).compare(lit.s);
                ways[r] = static_cast<std::int8_t>(c < 0 ? -1 : (c > 0 ? 1 : 0));
            }
            return true;
        default:
            return false;
    }
}

void set_nulls(const ArrowArrayView& v, int64_t n, std::vector<std::uint8_t>& state) {
    if (v.null_count == 0 || v.buffer_views[0].data.data == nullptr) {
        return;
    }
    for (int64_t r = 0; r < n; ++r) {
        if (ArrowArrayViewIsNull(&v, r)) {
            state[static_cast<std::size_t>(r)] = kNull;
        }
    }
}

bool fast_eval(const Node& n, const ArrowArrayView& batch, int64_t length, std::vector<std::uint8_t>& out) {
    const auto rows = static_cast<std::size_t>(length);
    switch (n.op) {
        case Op::And:
        case Op::Or: {
            std::vector<std::uint8_t> b;
            if (!fast_eval(*n.args[0], batch, length, out) || !fast_eval(*n.args[1], batch, length, b)) {
                return false;
            }
            const bool is_and = n.op == Op::And;
            for (std::size_t r = 0; r < rows; ++r) {
                const auto x = out[r];
                const auto y = b[r];
                if (is_and) {
                    out[r] = x == kFalse || y == kFalse ? kFalse : (x == kNull || y == kNull ? kNull : kTrue);
                } else {
                    out[r] = x == kTrue || y == kTrue ? kTrue : (x == kNull || y == kNull ? kNull : kFalse);
                }
            }
            return true;
        }
        case Op::Not:
            if (!fast_eval(*n.args[0], batch, length, out)) {
                return false;
            }
            for (auto& x : out) {
                x = x == kNull ? kNull : static_cast<std::uint8_t>(x == kTrue ? kFalse : kTrue);
            }
            return true;
        case Op::IsNull:
        case Op::IsNotNull: {
            const auto* v = fast_column(*n.args[0], batch);
            if (v == nullptr) {
                return false;
            }
            out.assign(rows, n.op == Op::IsNull ? kFalse : kTrue);
            for (int64_t r = 0; r < length && v->null_count != 0; ++r) {
                if (ArrowArrayViewIsNull(v, r)) {
                    out[static_cast<std::size_t>(r)] = n.op == Op::IsNull ? kTrue : kFalse;
                }
            }
            return true;
        }
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
            const bool left = n.args[0]->op == Op::Column && n.args[1]->op == Op::Literal;
            const bool right = n.args[1]->op == Op::Column && n.args[0]->op == Op::Literal;
            if (!left && !right) {
                return false;
            }
            const Node& col = left ? *n.args[0] : *n.args[1];
            const Value& lit = (left ? n.args[1] : n.args[0])->literal;
            const auto* v = fast_column(col, batch);
            if (v == nullptr) {
                return false;
            }
            const Op op = left ? n.op : flip(n.op);
            if (lit.is_null()) {
                out.assign(rows, kNull);
                return true;
            }
            std::vector<std::int8_t> ways;
            if (!compare_column(*v, col.column.type, col.column.unit, lit, length, ways)) {
                return false;
            }
            out.resize(rows);
            for (std::size_t r = 0; r < rows; ++r) {
                out[r] = wanted(op, ways[r]) ? kTrue : kFalse;
            }
            set_nulls(*v, length, out);
            return true;
        }
        case Op::In:
        case Op::NotIn:
        case Op::Between:
        case Op::NotBetween: {
            const auto* v = fast_column(*n.args[0], batch);
            if (v == nullptr || !std::all_of(n.args.begin() + 1, n.args.end(), [](const std::unique_ptr<Node>& a) {
                    return a->op == Op::Literal;
                })) {
                return false;
            }
            const auto& c = n.args[0]->column;
            std::vector<std::int8_t> ways;
            if (n.op == Op::Between || n.op == Op::NotBetween) {
                if (n.args.size() != 3U || n.args[1]->literal.is_null() || n.args[2]->literal.is_null()) {
                    return false;
                }
                std::vector<std::int8_t> upper;
                if (!compare_column(*v, c.type, c.unit, n.args[1]->literal, length, ways) ||
                    !compare_column(*v, c.type, c.unit, n.args[2]->literal, length, upper)) {
                    return false;
                }
                const bool negate = n.op == Op::NotBetween;
                out.resize(rows);
                for (std::size_t r = 0; r < rows; ++r) {
                    const bool in = ways[r] >= 0 && upper[r] <= 0;
                    out[r] = in != negate ? kTrue : kFalse;
                }
                set_nulls(*v, length, out);
                return true;
            }
            // x IN (a, b): TRUE if any equals; else NULL if any item is NULL; else FALSE.
            out.assign(rows, kFalse);
            bool null_item = false;
            for (std::size_t k = 1; k < n.args.size(); ++k) {
                const auto& lit = n.args[k]->literal;
                if (lit.is_null()) {
                    null_item = true;
                    continue;
                }
                if (!compare_column(*v, c.type, c.unit, lit, length, ways)) {
                    return false;
                }
                for (std::size_t r = 0; r < rows; ++r) {
                    if (ways[r] == 0) {
                        out[r] = kTrue;
                    }
                }
            }
            for (auto& x : out) {
                if (x == kFalse && null_item) {
                    x = kNull;
                }
                if (n.op == Op::NotIn && x != kNull) {
                    x = x == kTrue ? kFalse : kTrue;
                }
            }
            set_nulls(*v, length, out);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

bool Expression::filter(const ArrowArray& batch, std::vector<std::uint8_t>& keep, std::string& error) const {
    error.clear();
    if (!binding_) {
        error = "the expression is not bound to a schema";
        return false;
    }
    ViewGuard guard;
    if (!view_batch(binding_->schema, batch, guard, error)) {
        return false;
    }
    keep.assign(static_cast<std::size_t>(batch.length), 0U);
    std::vector<std::uint8_t> state;
    if (fast_eval(*root_, guard.view, batch.length, state)) {
        for (std::size_t r = 0; r < keep.size(); ++r) {
            keep[r] = state[r] == kTrue ? 1U : 0U;
        }
        return true;
    }
    Context ctx;
    ctx.batch = &guard.view;
    ctx.error = &error;
    for (int64_t r = 0; r < batch.length; ++r) {
        const Value v = eval(*root_, r, ctx);
        if (ctx.failed) {
            return false;
        }
        if (v.kind == Kind::Bool) {
            keep[static_cast<std::size_t>(r)] = v.b ? 1U : 0U;
        } else if (!v.is_null()) {
            error = std::string("a filter must be a boolean expression, not ") + kind_name(v.kind);
            return false;
        }
    }
    return true;
}

// ── projection types (EXPR_TYPES phase C1) ──────────────────────────────────────────────────────
//
// A projected numeric operator is typed as DataFusion types it, not as evaluate()'s dynamic Values:
// a literal beside a numeric column takes the column's type (cast when it fits, else refused); other
// literals keep their own type (Int64, UInt64, Float64); int with int widens (same signedness: the
// wider; mixed: the signed type of max(2 x unsigned width, signed width)); int with float32 is
// float32, int with float64 is double; results wrap on overflow; division by zero and MIN / -1 are
// errors. Only projections come here (result_type and evaluate); filters keep their own evaluation.

namespace {

enum class NumType : std::uint8_t { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

bool num_is_int(NumType t) { return t <= NumType::U64; }
bool num_is_signed(NumType t) { return t <= NumType::I64; }

int num_bits(NumType t) {
    switch (t) {
        case NumType::I8:
        case NumType::U8: return 8;
        case NumType::I16:
        case NumType::U16: return 16;
        case NumType::I32:
        case NumType::U32:
        case NumType::F32: return 32;
        default: return 64;
    }
}

std::optional<NumType> num_of_arrow(ArrowType t) {
    switch (t) {
        case NANOARROW_TYPE_INT8: return NumType::I8;
        case NANOARROW_TYPE_INT16: return NumType::I16;
        case NANOARROW_TYPE_INT32: return NumType::I32;
        case NANOARROW_TYPE_INT64: return NumType::I64;
        case NANOARROW_TYPE_UINT8: return NumType::U8;
        case NANOARROW_TYPE_UINT16: return NumType::U16;
        case NANOARROW_TYPE_UINT32: return NumType::U32;
        case NANOARROW_TYPE_UINT64: return NumType::U64;
        case NANOARROW_TYPE_FLOAT: return NumType::F32;
        case NANOARROW_TYPE_DOUBLE: return NumType::F64;
        default: return std::nullopt;
    }
}

ArrowType num_arrow(NumType t) {
    switch (t) {
        case NumType::I8: return NANOARROW_TYPE_INT8;
        case NumType::I16: return NANOARROW_TYPE_INT16;
        case NumType::I32: return NANOARROW_TYPE_INT32;
        case NumType::I64: return NANOARROW_TYPE_INT64;
        case NumType::U8: return NANOARROW_TYPE_UINT8;
        case NumType::U16: return NANOARROW_TYPE_UINT16;
        case NumType::U32: return NANOARROW_TYPE_UINT32;
        case NumType::U64: return NANOARROW_TYPE_UINT64;
        case NumType::F32: return NANOARROW_TYPE_FLOAT;
        default: return NANOARROW_TYPE_DOUBLE;
    }
}

/// The names DataFusion gives these types in its errors ('Int8', 'UInt64', 'Float32').
const char* num_name(NumType t) {
    switch (t) {
        case NumType::I8: return "Int8";
        case NumType::I16: return "Int16";
        case NumType::I32: return "Int32";
        case NumType::I64: return "Int64";
        case NumType::U8: return "UInt8";
        case NumType::U16: return "UInt16";
        case NumType::U32: return "UInt32";
        case NumType::U64: return "UInt64";
        case NumType::F32: return "Float32";
        default: return "Float64";
    }
}

/// The array type named in Arrow's abs() overflow error ('Int8Array').
const char* num_array_name(NumType t) {
    switch (t) {
        case NumType::I8: return "Int8Array";
        case NumType::I16: return "Int16Array";
        case NumType::I32: return "Int32Array";
        default: return "Int64Array";
    }
}

/// The largest value of a signed / unsigned integer type of `t`'s width.
std::uint64_t num_umax(NumType t) { return ~std::uint64_t{0} >> (64 - num_bits(t)); }
std::int64_t num_smax(NumType t) { return static_cast<std::int64_t>(num_umax(t) >> 1); }
std::int64_t num_smin(NumType t) { return -num_smax(t) - 1; }

/// The type of the common width of two numeric operands. A u64 with a signed int needs decimal128,
/// which is phase C2: `decimal` is set and the projection keeps its own typing.
NumType common_num(NumType a, NumType b, bool& decimal) {
    if (a == b) {
        return a;
    }
    if (!num_is_int(a) && !num_is_int(b)) {
        return NumType::F64;  // float32 with float64
    }
    if (!num_is_int(a) || !num_is_int(b)) {
        // int with float32 is float32; int with float64 is double
        const NumType f = num_is_int(a) ? b : a;
        return f == NumType::F32 ? NumType::F32 : NumType::F64;
    }
    if (num_is_signed(a) == num_is_signed(b)) {
        return num_bits(a) >= num_bits(b) ? a : b;
    }
    const NumType u = num_is_signed(a) ? b : a;
    const NumType s = num_is_signed(a) ? a : b;
    const int bits = std::max(2 * num_bits(u), num_bits(s));
    if (bits > 64) {
        decimal = true;
        return s;
    }
    switch (bits) {
        case 16: return NumType::I16;
        case 32: return NumType::I32;
        default: return NumType::I64;
    }
}

/// Whether an integer literal (Int or UInt) is in range for the integer type `t`.
bool int_literal_fits(const Value& v, NumType t) {
    const std::uint64_t umax = num_umax(t);
    if (v.kind == Kind::UInt) {
        return num_is_signed(t) ? v.u <= static_cast<std::uint64_t>(num_smax(t)) : v.u <= umax;
    }
    if (num_is_signed(t)) {
        return v.i >= num_smin(t) && v.i <= num_smax(t);
    }
    return v.i >= 0 && static_cast<std::uint64_t>(v.i) <= umax;
}

/// `raw` (two's complement bits) wrapped to the width of integer type `t`.
Value int_value(std::uint64_t raw, NumType t) {
    if (num_is_signed(t)) {
        switch (num_bits(t)) {
            case 8: return Value::integer(static_cast<std::int8_t>(raw));
            case 16: return Value::integer(static_cast<std::int16_t>(raw));
            case 32: return Value::integer(static_cast<std::int32_t>(raw));
            default: return Value::integer(static_cast<std::int64_t>(raw));
        }
    }
    return Value::uinteger(raw & num_umax(t));
}

/// `v` as a value of numeric type `t`: integers take t's signedness, float32 values are rounded once.
Value as_num(const Value& v, NumType t) {
    if (v.is_null()) {
        return v;
    }
    if (num_is_int(t)) {
        return int_value(v.kind == Kind::UInt ? v.u : static_cast<std::uint64_t>(v.i), t);
    }
    if (t == NumType::F32) {
        if (v.kind == Kind::UInt) {
            return Value::real(static_cast<float>(v.u));
        }
        if (v.kind == Kind::Int) {
            return Value::real(static_cast<float>(v.i));
        }
        return Value::real(round_to_float32(v.d));
    }
    if (v.kind == Kind::UInt) {
        return Value::real(static_cast<double>(v.u));
    }
    if (v.kind == Kind::Int) {
        return Value::real(static_cast<double>(v.i));
    }
    return Value::real(v.d);
}

/// A decimal128 type of the typed path: precision and scale.
struct DecType {
    int p = 0;
    int s = 0;
};


std::string i128_text(I128 v) {
    const bool neg = v < 0;
    const auto u = static_cast<U128>(v);
    const std::uint64_t sign = neg ? ~std::uint64_t{0} : 0;
    return mag_text(limbs_magnitude(Limbs256{static_cast<std::uint64_t>(u), static_cast<std::uint64_t>(u >> 64), sign, sign}),
                    0, neg);
}

Value decimal_value(I128 v, int scale) {
    const auto u = static_cast<U128>(v);
    const std::uint64_t sign = v < 0 ? ~std::uint64_t{0} : 0;
    return Value::decimal(Limbs256{static_cast<std::uint64_t>(u), static_cast<std::uint64_t>(u >> 64), sign, sign},
                          scale);
}

/// An integer or decimal Value as its unscaled 128-bit value and scale.
bool value_i128(const Value& v, I128& out, int& scale) {
    if (v.kind == Kind::Int) {
        out = v.i;
        scale = 0;
        return true;
    }
    if (v.kind == Kind::UInt) {
        out = static_cast<I128>(v.u);
        scale = 0;
        return true;
    }
    if (v.kind == Kind::Decimal) {
        out = static_cast<I128>((static_cast<U128>(v.dec[1]) << 64) | v.dec[0]);
        scale = v.scale;
        return true;
    }
    return false;
}

/// a sym b with DataFusion's message on i128 overflow (the operands as they are used).
bool checked_i128(char sym, I128 a, I128 b, I128& r, std::string& err) {
    const bool of = sym == '+' ? i128_add_overflow(a, b, r)
                    : sym == '-' ? i128_sub_overflow(a, b, r)
                                 : i128_mul_overflow(a, b, r);
    if (of) {
        err = "Arithmetic overflow: Overflow happened on: " + i128_text(a) + " " + std::string(1, sym) + " " +
              i128_text(b);
    }
    return !of;
}

/// v * 10^k with the same overflow check.
bool rescale_i128(I128 v, int k, I128& out, std::string& err) {
    I128 f = 1;
    for (int i = 0; i < k; ++i) {
        f *= 10;
    }
    return checked_i128('*', v, f, out, err);
}

double as_double_value(const Value& v) {
    if (v.kind == Kind::Decimal) {
        return decimal_as_double(v.dec, v.scale);
    }
    if (v.kind == Kind::Int) {
        return static_cast<double>(v.i);
    }
    if (v.kind == Kind::UInt) {
        return static_cast<double>(v.u);
    }
    return v.d;
}

/// The decimal type of `a op b` as DataFusion 12 gives it (measured on pylance; see the C2 notes).
/// Precision is capped at 38 and nothing else is checked: a result past its precision is returned as
/// it is, and only the 128-bit arithmetic can fail.
bool decimal_result_type(Op op, DecType a, DecType b, DecType& r, std::string& error) {
    switch (op) {
        case Op::Add:
        case Op::Sub:
            r.s = std::max(a.s, b.s);
            r.p = std::min(38, std::max(a.p - a.s, b.p - b.s) + r.s + 1);
            return true;
        case Op::Mul: {
            r.s = a.s + b.s;
            if (r.s > 38) {
                const std::string x = "Decimal128(" + std::to_string(a.p) + ", " + std::to_string(a.s) + ")";
                const std::string y = "Decimal128(" + std::to_string(b.p) + ", " + std::to_string(b.s) + ")";
                error = "Invalid user input: Error during planning: Cannot get result type for decimal operation " + x +
                        " * " + y + ": Invalid argument error: Output scale of " + x + " * " + y +
                        " would exceed max scale of 38";
                return false;
            }
            r.p = std::min(38, a.p + b.p + 1);
            return true;
        }
        case Op::Div: {
            r.s = std::min(38, a.s + 4);
            const int k = r.s + b.s - a.s;  // the numerator is scaled up by 10^k before the division
            if (k > 38) {
                const std::string x = "Decimal128(" + std::to_string(a.p) + ", " + std::to_string(a.s) + ")";
                const std::string y = "Decimal128(" + std::to_string(b.p) + ", " + std::to_string(b.s) + ")";
                error = "Invalid user input: Error during planning: Cannot get result type for decimal operation " + x +
                        " / " + y + ": Arithmetic overflow: Overflow happened on: 10 ^ " + std::to_string(k);
                return false;
            }
            r.p = std::min(38, a.p + b.s + 4);
            return true;
        }
        default:  // Mod
            r.s = std::max(a.s, b.s);
            r.p = std::min(38, std::min(a.p - a.s, b.p - b.s) + r.s);
            return true;
    }
}

/// `a op b` for decimals: both rescaled to the result scale, computed in 128 bits, truncated division.
Value arith_decimal(Op op, int rs, const Value& a, const Value& b, Context& ctx) {
    I128 x = 0;
    I128 y = 0;
    int sx = 0;
    int sy = 0;
    value_i128(a, x, sx);
    value_i128(b, y, sy);
    std::string err;
    I128 r = 0;
    if (op == Op::Mul) {
        if (!checked_i128('*', x, y, r, err)) {
            return ctx.fail(err);
        }
    } else if (op == Op::Div) {
        I128 num = 0;
        if (!rescale_i128(x, rs + sy - sx, num, err)) {
            return ctx.fail(err);
        }
        if (y == 0) {
            return ctx.fail("Divide by zero error");
        }
        if (y == -1) {
            if (!checked_i128('-', 0, num, r, err)) {
                return ctx.fail(err);
            }
        } else {
            r = num / y;
        }
    } else {
        I128 xs = 0;
        I128 ys = 0;
        if (!rescale_i128(x, rs - sx, xs, err) || !rescale_i128(y, rs - sy, ys, err)) {
            return ctx.fail(err);
        }
        if (op == Op::Mod) {
            if (ys == 0) {
                return ctx.fail("Divide by zero error");
            }
            r = ys == -1 ? 0 : xs % ys;
        } else if (!checked_i128(op == Op::Add ? '+' : '-', xs, ys, r, err)) {
            return ctx.fail(err);
        }
    }
    return decimal_value(r, rs);
}


struct TypedNode {
    enum class Shape : std::uint8_t { Constant, Column, Arith, Neg, Abs, Coalesce, ToDecimal, Cast };
    Shape shape = Shape::Constant;
    Op op = Op::Add;               // Arith
    NumType type = NumType::I64;   // integer or float (not used when dec)
    bool dec = false;              // a decimal128 of (dp, ds)
    int dp = 0;
    int ds = 0;
    bool dbl = false;              // Arith: decimal beside a float64, computed in double
    Value constant;                // Constant: already of its type
    const Node* column = nullptr;  // Column: the bound column reference
    std::vector<std::unique_ptr<TypedNode>> args;
};
using TypedPtr = std::unique_ptr<TypedNode>;

/// The decimal precision DataFusion gives an integer type (i8 3, i16 5, i32 10, i64 20).
int int_precision(NumType t) {
    switch (num_bits(t)) {
        case 8: return 3;
        case 16: return 5;
        case 32: return 10;
        default: return 20;
    }
}

/// A numeric column (not a list element) and its type.
std::optional<NumType> numeric_column(const Node& n) {
    if (n.op != Op::Column) {
        return std::nullopt;
    }
    for (const auto e : n.column.elements) {
        if (e != ColumnRef::kNoElement) {
            return std::nullopt;
        }
    }
    return num_of_arrow(n.column.type);
}

/// A decimal128 column (not a list element) or a folded decimal literal, and its type.
bool decimal_of(const Node& n, DecType& t) {
    if (n.op == Op::Column) {
        for (const auto e : n.column.elements) {
            if (e != ColumnRef::kNoElement) {
                return false;
            }
        }
    }
    DecimalType d;
    if (!decimal_type_of(n, d) || d.wide) {
        return false;
    }
    t = DecType{d.precision, d.scale};
    return true;
}

// Builders return true with `out` set. false with an empty `error` means the expression is not one
// of the typed shapes (the caller keeps its own typing); false with an error refuses it, as pylance does.

bool build_typed(const Node& n, TypedPtr& out, std::string& error, bool root = false);

bool build_literal(const Node& n, std::optional<NumType> beside, TypedPtr& out, std::string& error) {
    const Value& v = n.literal;
    if (v.kind != Kind::Int && v.kind != Kind::UInt && v.kind != Kind::Double) {
        return false;
    }
    NumType t = NumType::F64;
    if (v.kind == Kind::Int) {
        t = beside ? *beside : NumType::I64;
    } else if (v.kind == Kind::UInt) {
        t = beside ? *beside : NumType::U64;
    } else if (beside) {
        t = *beside;
    }
    const bool refused = num_is_int(t) && (v.kind == Kind::Double || !int_literal_fits(v, t));
    if (refused) {
        error = "Received literal " + datafusion_literal(v) + " and could not convert to literal of type '" +
                num_name(t) + "'";
        return false;
    }
    out = std::make_unique<TypedNode>();
    out->shape = TypedNode::Shape::Constant;
    out->type = t;
    out->constant = as_num(v, t);
    return true;
}

/// An integer literal beside a decimal takes the decimal's type (pylance: cast if it fits, else an error).
/// Float literals are not handled here (false, no error: the caller refuses them as before).
bool build_decimal_literal(const Node& n, DecType t, TypedPtr& out, std::string& error) {
    const Value& v = n.literal;
    if (v.kind != Kind::Int && v.kind != Kind::UInt) {
        return false;
    }
    const auto mag = v.kind == Kind::UInt ? static_cast<U128>(v.u)
                     : v.i < 0            ? 0 - static_cast<U128>(v.i)
                                          : static_cast<U128>(v.i);
    U128 bound = 1;
    for (int k = 0; k < t.p - t.s; ++k) {
        bound *= 10;
    }
    if (mag >= bound) {
        error = "Received literal " + datafusion_literal(v) + " and could not convert to literal of type 'Decimal128(" +
                std::to_string(t.p) + ", " + std::to_string(t.s) + ")'";
        return false;
    }
    I128 unscaled = v.kind == Kind::UInt ? static_cast<I128>(v.u) : static_cast<I128>(v.i);
    for (int k = 0; k < t.s; ++k) {
        unscaled *= 10;
    }
    out = std::make_unique<TypedNode>();
    out->shape = TypedNode::Shape::Constant;
    out->dec = true;
    out->dp = t.p;
    out->ds = t.s;
    out->constant = decimal_value(unscaled, t.s);
    return true;
}

/// A float as decimal(30, 15), the type coalesce gives it beside a decimal: its exact text at 15 digits.
bool float_decimal15(double d, Limbs256& limbs, std::string& error) {
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.15f", d);
    Magnitude m;
    bool negative = false;
    int scale = 0;
    if (!parse_decimal_text(buf, m, negative, scale)) {
        error = "cannot read a float as a decimal";
        return false;
    }
    return fit_decimal(m, negative, scale, 15, 30, "Decimal128", limbs, error);
}

/// A float literal as decimal(30, 15) (coalesce's rule beside a decimal).
bool float_literal_decimal(double d, TypedPtr& out, std::string& error) {
    Limbs256 limbs{};
    if (!float_decimal15(d, limbs, error)) {
        return false;
    }
    out = std::make_unique<TypedNode>();
    out->shape = TypedNode::Shape::Constant;
    out->dec = true;
    out->dp = 30;
    out->ds = 15;
    out->constant = Value::decimal(limbs, 15);
    return true;
}

/// An operand of a decimal operation: its type, or false when it is not one of the typed shapes
/// (a nested integer expression, an integer literal beside a non-column, a float32).
bool decimal_operand(const TypedNode& n, const TypedNode& other, DecType& out) {
    if (n.dec) {
        out = DecType{n.dp, n.ds};
        return true;
    }
    // An integer column, or an integer literal that did not take a decimal's type: decimal(p, 0).
    if ((n.shape != TypedNode::Shape::Column && n.shape != TypedNode::Shape::Constant &&
         n.shape != TypedNode::Shape::Cast) || !num_is_int(n.type)) {
        return false;
    }
    out = DecType{int_precision(n.type), 0};
    if (num_is_signed(n.type) && !other.dec && other.type == NumType::U64) {
        out.p = 20;  // a signed int beside a u64 is decimal(20, 0) too
    }
    return true;
}

/// `a op b` where one side is decimal (or both are integers of a u64 / signed pair, which DataFusion
/// computes as decimal too). A decimal beside a float64 column is double arithmetic.
bool build_decimal_arith(Op op, TypedPtr& a, TypedPtr& b, TypedPtr& out, std::string& error) {
    const TypedNode& other_a = a->dec ? *b : *a;
    if (!other_a.dec && other_a.type == NumType::F64 && other_a.shape == TypedNode::Shape::Column) {
        out = std::make_unique<TypedNode>();
        out->shape = TypedNode::Shape::Arith;
        out->op = op;
        out->type = NumType::F64;
        out->dbl = true;
        out->args.push_back(std::move(a));
        out->args.push_back(std::move(b));
        return true;
    }
    DecType da{};
    DecType db{};
    if (!decimal_operand(*a, *b, da) || !decimal_operand(*b, *a, db)) {
        return false;
    }
    DecType r{};
    if (!decimal_result_type(op, da, db, r, error)) {
        return false;
    }
    out = std::make_unique<TypedNode>();
    out->shape = TypedNode::Shape::Arith;
    out->op = op;
    out->dec = true;
    out->dp = r.p;
    out->ds = r.s;
    out->args.push_back(std::move(a));
    out->args.push_back(std::move(b));
    return true;
}

/// An operand of a numeric operator: a literal takes the type of a column beside it when that column is a
/// decimal (at any depth) or, for the root only, a numeric column (pylance narrows nowhere deeper).
/// The number type a cast names: arrow_cast's int8..uint64 and float32 / double, CAST's TINYINT, SMALLINT,
/// INT, BIGINT, FLOAT, DOUBLE. False for any other name (a decimal, a string: the caller keeps its own).
bool cast_num_target(const std::string& name, NumType& t) {
    static const std::pair<const char*, NumType> names[] = {
        {"int8", NumType::I8},       {"int16", NumType::I16},      {"int32", NumType::I32},
        {"int64", NumType::I64},     {"uint8", NumType::U8},       {"uint16", NumType::U16},
        {"uint32", NumType::U32},    {"uint64", NumType::U64},     {"tinyint", NumType::I8},
        {"smallint", NumType::I16},  {"int", NumType::I32},        {"bigint", NumType::I64},
        {"float", NumType::F32},     {"double", NumType::F64},
    };
    for (const auto& [text, type] : names) {
        if (name == text) {
            t = type;
            return true;
        }
    }
    return false;
}

bool build_operand(const Node& n, const Node& other, bool narrow, TypedPtr& out, std::string& error) {
    if (n.op == Op::Literal && n.literal.kind != Kind::Decimal) {
        DecType dt{};
        if (decimal_of(other, dt) && other.op == Op::Column) {
            if (n.literal.kind == Kind::Double) {
                error = "Received literal " + datafusion_literal(n.literal) +
                        " and could not convert to literal of type 'Decimal128(" + std::to_string(dt.p) + ", " +
                        std::to_string(dt.s) + ")'";
                return false;
            }
            return build_decimal_literal(n, dt, out, error);
        }
        return build_literal(n, narrow ? numeric_column(other) : std::nullopt, out, error);
    }
    return build_typed(n, out, error);
}

bool build_typed(const Node& n, TypedPtr& out, std::string& error, bool root) {
    if (const auto t = numeric_column(n)) {
        out = std::make_unique<TypedNode>();
        out->shape = TypedNode::Shape::Column;
        out->type = *t;
        out->column = &n;
        return true;
    }
    DecType dt{};
    if (n.op == Op::Column && decimal_of(n, dt)) {
        out = std::make_unique<TypedNode>();
        out->shape = TypedNode::Shape::Column;
        out->dec = true;
        out->dp = dt.p;
        out->ds = dt.s;
        out->column = &n;
        return true;
    }
    if (n.op == Op::Literal && n.literal.kind == Kind::Decimal && decimal_of(n, dt)) {
        out = std::make_unique<TypedNode>();
        out->shape = TypedNode::Shape::Constant;
        out->dec = true;
        out->dp = dt.p;
        out->ds = dt.s;
        out->constant = n.literal;
        return true;
    }
    switch (n.op) {
        case Op::Literal:
            return build_literal(n, std::nullopt, out, error);
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod: {
            TypedPtr a;
            TypedPtr b;
            if (!build_operand(*n.args[0], *n.args[1], root, a, error) ||
                !build_operand(*n.args[1], *n.args[0], root, b, error)) {
                return false;
            }
            if (a->dec || b->dec) {
                return build_decimal_arith(n.op, a, b, out, error);
            }
            bool decimal = false;
            const NumType t = common_num(a->type, b->type, decimal);
            if (decimal) {
                if (!root) {
                    return false;  // a nested u64 with a signed int keeps the caller's typing
                }
                return build_decimal_arith(n.op, a, b, out, error);
            }
            out = std::make_unique<TypedNode>();
            out->shape = TypedNode::Shape::Arith;
            out->op = n.op;
            out->type = t;
            out->args.push_back(std::move(a));
            out->args.push_back(std::move(b));
            return true;
        }
        case Op::Cast: {
            // arrow_cast / CAST to a number type (C3): the target is typed; the child is typed as usual.
            NumType target = NumType::I64;
            int dp = 0;
            int ds = 0;
            const bool decimal = std::sscanf(n.name.c_str(), "decimal %d %d", &dp, &ds) == 2;
            if (!decimal && !cast_num_target(n.name, target)) {
                return false;
            }
            TypedPtr a;
            const Node& arg = *n.args[0];
            const bool ok = arg.op == Op::Literal && arg.literal.kind != Kind::Decimal
                                ? build_literal(arg, std::nullopt, a, error)
                                : build_typed(arg, a, error);
            if (!ok) {
                return false;
            }
            out = std::make_unique<TypedNode>();
            out->shape = TypedNode::Shape::Cast;
            out->type = target;
            out->dec = decimal;
            out->dp = dp;
            out->ds = ds;
            out->args.push_back(std::move(a));
            return true;
        }
        case Op::Neg: {
            TypedPtr a;
            if (!build_typed(*n.args[0], a, error)) {
                return false;
            }
            if (!a->dec && num_is_int(a->type) && !num_is_signed(a->type)) {
                error = "Invalid user input: Error during planning: Negation only supports numeric, interval and "
                        "timestamp types";
                return false;
            }
            out = std::make_unique<TypedNode>();
            out->shape = TypedNode::Shape::Neg;
            out->type = a->type;
            out->dec = a->dec;
            out->dp = a->dp;
            out->ds = a->ds;
            out->args.push_back(std::move(a));
            return true;
        }
        case Op::Func: {
            if (n.name == "abs" && n.args.size() == 1) {
                TypedPtr a;
                if (!build_typed(*n.args[0], a, error)) {
                    return false;
                }
                out = std::make_unique<TypedNode>();
                out->shape = TypedNode::Shape::Abs;
                out->type = a->type;
                out->dec = a->dec;
                out->dp = a->dp;
                out->ds = a->ds;
                out->args.push_back(std::move(a));
                return true;
            }
            if (n.name == "coalesce" && !n.args.empty()) {
                // coalesce's literals are not narrowed to a column: each keeps its own type.
                out = std::make_unique<TypedNode>();
                out->shape = TypedNode::Shape::Coalesce;
                for (const auto& arg : n.args) {
                    TypedPtr item;
                    const bool ok = arg->op == Op::Literal ? build_literal(*arg, std::nullopt, item, error)
                                                           : build_typed(*arg, item, error);
                    if (!ok) {
                        return false;
                    }
                    out->args.push_back(std::move(item));
                }
                bool any_dec = false;
                for (const auto& item : out->args) {
                    any_dec = any_dec || item->dec;
                }
                if (any_dec) {
                    // decimal(p, s) with p - s the larger integer digits and s the larger scale; a float
                    // literal is decimal(30, 15) beside one, an integer its own digits (pylance 12).
                    int integer_digits = 0;
                    int scale = 0;
                    for (auto& item : out->args) {
                        if (!item->dec && item->shape == TypedNode::Shape::Constant && item->type == NumType::F64) {
                            TypedPtr d;
                            if (!float_literal_decimal(item->constant.d, d, error)) {
                                return false;
                            }
                            item = std::move(d);
                        } else if (!item->dec && item->shape == TypedNode::Shape::Column && item->type == NumType::F64) {
                            auto d = std::make_unique<TypedNode>();
                            d->shape = TypedNode::Shape::ToDecimal;
                            d->dec = true;
                            d->dp = 30;
                            d->ds = 15;
                            d->args.push_back(std::move(item));
                            item = std::move(d);
                        }
                        DecType t{};
                        if (item->dec) {
                            t = DecType{item->dp, item->ds};
                        } else if (item->shape != TypedNode::Shape::Arith && num_is_int(item->type)) {
                            t = DecType{int_precision(item->type), 0};
                        } else {
                            return false;  // a float64 column beside a decimal: the caller keeps its typing
                        }
                        integer_digits = std::max(integer_digits, t.p - t.s);
                        scale = std::max(scale, t.s);
                    }
                    out->dec = true;
                    out->dp = std::min(38, integer_digits + scale);
                    out->ds = scale;
                    return true;
                }
                bool decimal = false;
                NumType t = out->args[0]->type;
                for (std::size_t k = 1; k < out->args.size(); ++k) {
                    t = common_num(t, out->args[k]->type, decimal);
                }
                if (decimal) {
                    return false;
                }
                out->type = t;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

/// The typed tree of a projection, when its root is a numeric operator. See the builders above.
bool build_projection(const Node& root, TypedPtr& typed, std::string& error) {
    error.clear();
    const bool numeric_root = root.op == Op::Add || root.op == Op::Sub || root.op == Op::Mul ||
                              root.op == Op::Div || root.op == Op::Mod || root.op == Op::Neg ||
                              root.op == Op::Cast ||
                              (root.op == Op::Func && (root.name == "abs" || root.name == "coalesce" ||
                                                       root.name == "arrow_cast"));
    if (!numeric_root) {
        return false;
    }
    if (!build_typed(root, typed, error, true)) {
        typed.reset();
        return false;
    }
    return true;
}

/// CAST to an integer type, as Arrow's checked cast: floats and decimals truncate toward zero, and a
/// value outside the type is an error ("Cast error: Can't cast value N to type Int8").
Value cast_integer(const Value& v, NumType t, Context& ctx) {
    bool negative = false;
    bool fits = true;
    std::uint64_t mag = 0;
    if (v.kind == Kind::Int) {
        negative = v.i < 0;
        mag = negative ? 0 - static_cast<std::uint64_t>(v.i) : static_cast<std::uint64_t>(v.i);
    } else if (v.kind == Kind::UInt) {
        mag = v.u;
    } else if (v.kind == Kind::Double) {
        const double whole = std::trunc(v.d);
        fits = std::isfinite(whole) && std::fabs(whole) < 18446744073709551616.0;
        if (fits) {
            negative = whole < 0;
            mag = static_cast<std::uint64_t>(std::fabs(whole));
        }
    } else {
        Magnitude m = limbs_magnitude(v.dec);
        for (int k = 0; k < v.scale; ++k) {
            mag_div_small(m, 10);
        }
        for (std::size_t k = 2; k < m.w.size(); ++k) {
            fits = fits && m.w[k] == 0;
        }
        mag = static_cast<std::uint64_t>(m.w[0]) | (static_cast<std::uint64_t>(m.w[1]) << 32);
        negative = limbs_negative(v.dec) && mag != 0;
    }
    const int bits = num_bits(t);
    bool in_range = false;
    if (num_is_signed(t)) {
        const std::uint64_t limit = std::uint64_t{1} << (bits - 1);  // |min|; max is limit - 1
        in_range = fits && (negative ? mag <= limit : mag < limit);
    } else {
        const std::uint64_t limit = bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
        in_range = fits && !negative && mag <= limit;
    }
    if (!in_range) {
        const std::string text = v.kind == Kind::Double ? format_value(v) : (negative ? "-" : "") + std::to_string(mag);
        return ctx.fail("Cast error: Can't cast value " + text + " to type " + num_name(t));
    }
    return int_value(num_is_signed(t) && negative ? 0 - mag : mag, t);
}

/// CAST to decimal(p, s): rounds half away from zero to the scale, then checks the precision (pylance's
/// "... is too large to store in a Decimal128 of precision p" text).
Value cast_decimal(const Value& v, int dp, int ds, Context& ctx) {
    Magnitude m{};
    bool negative = false;
    int scale = 0;
    if (v.kind == Kind::Decimal) {
        m = limbs_magnitude(v.dec);
        negative = limbs_negative(v.dec);
        scale = v.scale;
    } else if (v.kind == Kind::Double) {
        if (!std::isfinite(v.d)) {
            return ctx.fail("cannot cast a non-finite float to a decimal");
        }
        char buf[400];
        std::snprintf(buf, sizeof buf, "%.15f", v.d);
        if (!parse_decimal_text(buf, m, negative, scale)) {
            return ctx.fail("cannot read a float as a decimal");
        }
    } else {
        const std::uint64_t mag = v.kind == Kind::UInt         ? v.u
                                  : v.i < 0                    ? 0 - static_cast<std::uint64_t>(v.i)
                                                               : static_cast<std::uint64_t>(v.i);
        negative = v.kind == Kind::Int && v.i < 0;
        m.w[0] = static_cast<std::uint32_t>(mag);
        m.w[1] = static_cast<std::uint32_t>(mag >> 32);
    }
    bool zero = true;
    for (const auto x : m.w) {
        zero = zero && x == 0;
    }
    Limbs256 out{};
    std::string err;
    if (!fit_decimal(m, negative && !zero, scale, ds, dp, "Decimal128", out, err)) {
        return ctx.fail(err);
    }
    return Value::decimal(out, ds);
}

/// `a op b` in type `t`: both operands already of type `t`. Errors are DataFusion's.
Value arith_typed(Op op, NumType t, const Value& a, const Value& b, Context& ctx) {
    if (num_is_int(t)) {
        if (!num_is_signed(t)) {
            const std::uint64_t x = a.u;
            const std::uint64_t y = b.u;
            switch (op) {
                case Op::Add: return int_value(x + y, t);
                case Op::Sub: return int_value(x - y, t);
                case Op::Mul: return int_value(x * y, t);
                case Op::Div:
                    if (y == 0) {
                        return ctx.fail("Divide by zero error");
                    }
                    return int_value(x / y, t);
                default:
                    if (y == 0) {
                        return ctx.fail("Divide by zero error");
                    }
                    return int_value(x % y, t);
            }
        }
        const std::int64_t x = a.i;
        const std::int64_t y = b.i;
        switch (op) {
            case Op::Add: return int_value(static_cast<std::uint64_t>(x) + static_cast<std::uint64_t>(y), t);
            case Op::Sub: return int_value(static_cast<std::uint64_t>(x) - static_cast<std::uint64_t>(y), t);
            case Op::Mul: return int_value(static_cast<std::uint64_t>(x) * static_cast<std::uint64_t>(y), t);
            case Op::Div:
                if (y == 0) {
                    return ctx.fail("Divide by zero error");
                }
                if (y == -1 && x == num_smin(t)) {
                    return ctx.fail("Arithmetic overflow: Overflow happened on: " + std::to_string(x) + " / -1");
                }
                return int_value(static_cast<std::uint64_t>(x / y), t);
            default:
                if (y == 0) {
                    return ctx.fail("Divide by zero error");
                }
                if (y == -1) {
                    return Value::integer(0);
                }
                return int_value(static_cast<std::uint64_t>(x % y), t);
        }
    }
    if (t == NumType::F32) {
        const auto x = static_cast<float>(a.d);
        const auto y = static_cast<float>(b.d);
        float r = 0.0F;
        switch (op) {
            case Op::Add: r = x + y; break;
            case Op::Sub: r = x - y; break;
            case Op::Mul: r = x * y; break;
            case Op::Div: r = x / y; break;
            default: r = std::fmod(x, y); break;
        }
        return Value::real(static_cast<double>(r));
    }
    switch (op) {
        case Op::Add: return Value::real(a.d + b.d);
        case Op::Sub: return Value::real(a.d - b.d);
        case Op::Mul: return Value::real(a.d * b.d);
        case Op::Div: return Value::real(a.d / b.d);
        default: return Value::real(std::fmod(a.d, b.d));
    }
}

Value eval_typed(const TypedNode& t, int64_t row, Context& ctx) {
    switch (t.shape) {
        case TypedNode::Shape::Constant:
            return t.constant;
        case TypedNode::Shape::Column:
            if (t.dec) {
                return eval(*t.column, row, ctx);
            }
            return as_num(eval(*t.column, row, ctx), t.type);
        case TypedNode::Shape::Arith: {
            if (t.dec || t.dbl) {
                const Value a = eval_typed(*t.args[0], row, ctx);
                const Value b = eval_typed(*t.args[1], row, ctx);
                if (a.is_null() || b.is_null()) {
                    return Value::null();
                }
                if (t.dbl) {
                    return arith_typed(t.op, NumType::F64, Value::real(as_double_value(a)),
                                       Value::real(as_double_value(b)), ctx);
                }
                return arith_decimal(t.op, t.ds, a, b, ctx);
            }
            const Value a = as_num(eval_typed(*t.args[0], row, ctx), t.type);
            const Value b = as_num(eval_typed(*t.args[1], row, ctx), t.type);
            if (a.is_null() || b.is_null()) {
                return Value::null();
            }
            return arith_typed(t.op, t.type, a, b, ctx);
        }
        case TypedNode::Shape::Neg: {
            if (t.dec) {
                const Value a = eval_typed(*t.args[0], row, ctx);
                if (a.is_null()) {
                    return a;
                }
                I128 x = 0;
                int scale = 0;
                value_i128(a, x, scale);
                return decimal_value(static_cast<I128>(0 - static_cast<U128>(x)), t.ds);
            }
            const Value a = as_num(eval_typed(*t.args[0], row, ctx), t.type);
            if (a.is_null()) {
                return a;
            }
            if (num_is_int(t.type)) {
                return int_value(std::uint64_t{0} - static_cast<std::uint64_t>(a.i), t.type);
            }
            return Value::real(-a.d);
        }
        case TypedNode::Shape::Abs: {
            if (t.dec) {
                const Value a = eval_typed(*t.args[0], row, ctx);
                if (a.is_null()) {
                    return a;
                }
                I128 x = 0;
                int scale = 0;
                value_i128(a, x, scale);
                return decimal_value(x < 0 ? static_cast<I128>(0 - static_cast<U128>(x)) : x, t.ds);
            }
            const Value a = as_num(eval_typed(*t.args[0], row, ctx), t.type);
            if (a.is_null()) {
                return a;
            }
            if (!num_is_int(t.type)) {
                return Value::real(std::fabs(a.d));
            }
            if (!num_is_signed(t.type)) {
                return a;
            }
            if (a.i == num_smin(t.type)) {
                return ctx.fail(std::string(num_array_name(t.type)) + " overflow on abs(" + std::to_string(a.i) + ")");
            }
            return Value::integer(a.i < 0 ? -a.i : a.i);
        }
        case TypedNode::Shape::Cast: {
            const Value v = eval_typed(*t.args[0], row, ctx);
            if (v.is_null()) {
                return v;
            }
            if (t.dec) {
                return cast_decimal(v, t.dp, t.ds, ctx);
            }
            if (num_is_int(t.type)) {
                return cast_integer(v, t.type, ctx);
            }
            return as_num(Value::real(as_double_value(v)), t.type);
        }
        case TypedNode::Shape::ToDecimal: {
            const Value v = as_num(eval_typed(*t.args[0], row, ctx), NumType::F64);
            if (v.is_null()) {
                return v;
            }
            Limbs256 limbs{};
            std::string err;
            if (!float_decimal15(v.d, limbs, err)) {
                return ctx.fail(err);
            }
            return Value::decimal(limbs, 15);
        }
        case TypedNode::Shape::Coalesce:
            for (const auto& arg : t.args) {
                if (t.dec) {
                    const Value v = eval_typed(*arg, row, ctx);
                    if (ctx.failed) {
                        return v;
                    }
                    if (v.is_null()) {
                        continue;
                    }
                    I128 x = 0;
                    int scale = 0;
                    value_i128(v, x, scale);
                    I128 r = 0;
                    std::string err;
                    if (!rescale_i128(x, t.ds - scale, r, err)) {
                        return ctx.fail(err);
                    }
                    return decimal_value(r, t.ds);
                }
                const Value v = as_num(eval_typed(*arg, row, ctx), t.type);
                if (ctx.failed || !v.is_null()) {
                    return v;
                }
            }
            return Value::null();
    }
    return Value::null();
}

/// A projected value. A decimal of the typed path is written as it is: pylance does not check the
/// precision of arithmetic results (only the 128-bit range), so neither do we.
bool append_projected(ArrowArray& out, const ArrowSchemaView& type, const Value& v, const TypedNode* typed,
                      Context& ctx) {
    if (typed != nullptr && typed->dec && v.kind == Kind::Decimal) {
        ArrowDecimal dec{};
        ArrowDecimalInit(&dec, 128, type.decimal_precision, type.decimal_scale);
        for (std::size_t k = 0; k < 4; ++k) {
            dec.words[k] = v.dec[k];
        }
        return ArrowArrayAppendDecimal(&out, &dec) == NANOARROW_OK;
    }
    return append_value(out, type, v, ctx);
}

}  // namespace

bool Expression::evaluate(const ArrowArray& batch, const ArrowSchema& type, ArrowArray& out, std::string& error) const {
    error.clear();
    if (!binding_) {
        error = "the expression is not bound to a schema";
        return false;
    }
    TypedPtr typed;
    if (!build_projection(*root_, typed, error) && !error.empty()) {
        return false;
    }
    ViewGuard guard;
    if (!view_batch(binding_->schema, batch, guard, error)) {
        return false;
    }
    ArrowSchemaView tv;
    ArrowError aerr;
    if (ArrowSchemaViewInit(&tv, &type, &aerr) != NANOARROW_OK) {
        error = std::string("bad output type: ") + aerr.message;
        return false;
    }
    if (ArrowArrayInitFromSchema(&out, &type, &aerr) != NANOARROW_OK || ArrowArrayStartAppending(&out) != NANOARROW_OK) {
        error = "cannot build the output array";
        if (out.release != nullptr) {
            out.release(&out);
        }
        return false;
    }
    Context ctx;
    ctx.batch = &guard.view;
    ctx.error = &error;
    ctx.projection = true;
    for (int64_t r = 0; r < batch.length; ++r) {
        const Value v = typed ? eval_typed(*typed, r, ctx) : eval(*root_, r, ctx);
        if (ctx.failed || !append_projected(out, tv, v, typed.get(), ctx)) {
            if (!ctx.failed) {
                error = "cannot append a value to the output";
            }
            out.release(&out);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&out, &aerr) != NANOARROW_OK) {
        error = std::string("cannot finish the output array: ") + aerr.message;
        out.release(&out);
        return false;
    }
    return true;
}

namespace {

/// A leaf type: an ArrowType and, for timestamps, its unit. `column` set: that column's exact type.
struct TypeGuess {
    ArrowType type = NANOARROW_TYPE_NA;
    ArrowTimeUnit unit = NANOARROW_TIME_UNIT_NANO;
    const ArrowSchema* column = nullptr;
};

const ArrowSchema* column_schema(const ArrowSchema& root, const ColumnRef& c) {
    const ArrowSchema* at = &root;
    for (std::size_t step = 0; step < c.indices.size(); ++step) {
        const bool element = !c.elements.empty() && c.elements[step] != ColumnRef::kNoElement;
        at = at->children[element ? 0 : c.indices[step]];  // a list element: the item schema
    }
    return at;
}

bool integer_type(ArrowType t) {
    return t == NANOARROW_TYPE_INT8 || t == NANOARROW_TYPE_INT16 || t == NANOARROW_TYPE_INT32 ||
           t == NANOARROW_TYPE_INT64 || t == NANOARROW_TYPE_UINT8 || t == NANOARROW_TYPE_UINT16 ||
           t == NANOARROW_TYPE_UINT32 || t == NANOARROW_TYPE_UINT64;
}

/// The type of a duration, timestamp or date sum or difference (temporal_arith's rules).
bool temporal_result(Op op, const TypeGuess& a, const TypeGuess& b, TypeGuess& g) {
    if (op != Op::Add && op != Op::Sub) {
        return false;
    }
    if (a.type == NANOARROW_TYPE_DURATION && b.type == NANOARROW_TYPE_DURATION) {
        g.type = NANOARROW_TYPE_DURATION;
        g.unit = a.unit;
        return true;
    }
    if (a.type == NANOARROW_TYPE_TIMESTAMP && b.type == NANOARROW_TYPE_DURATION) {
        g.type = NANOARROW_TYPE_TIMESTAMP;
        g.unit = a.unit;
        return true;
    }
    if (op == Op::Add && a.type == NANOARROW_TYPE_DURATION && b.type == NANOARROW_TYPE_TIMESTAMP) {
        g.type = NANOARROW_TYPE_TIMESTAMP;
        g.unit = b.unit;
        return true;
    }
    if (op == Op::Sub && a.type == NANOARROW_TYPE_TIMESTAMP && b.type == NANOARROW_TYPE_TIMESTAMP) {
        g.type = NANOARROW_TYPE_DURATION;
        g.unit = std::min(a.unit, b.unit);
        return true;
    }
    if (op == Op::Sub && a.type == NANOARROW_TYPE_DATE32 && b.type == NANOARROW_TYPE_DATE32) {
        g.type = NANOARROW_TYPE_INT64;
        return true;
    }
    return false;
}

TypeGuess guess(const Node& n, const ArrowSchema& root) {
    TypeGuess g;
    switch (n.op) {
        case Op::Literal:
            switch (n.literal.kind) {
                case Kind::Bool: g.type = NANOARROW_TYPE_BOOL; break;
                case Kind::Int: g.type = NANOARROW_TYPE_INT64; break;
                case Kind::UInt: g.type = NANOARROW_TYPE_UINT64; break;
                case Kind::Double: g.type = NANOARROW_TYPE_DOUBLE; break;
                case Kind::String: g.type = NANOARROW_TYPE_STRING; break;
                case Kind::Binary: g.type = NANOARROW_TYPE_BINARY; break;
                case Kind::Date: g.type = NANOARROW_TYPE_DATE32; break;
                case Kind::Timestamp: g.type = NANOARROW_TYPE_TIMESTAMP; break;
                case Kind::Null: g.type = NANOARROW_TYPE_NA; break;
                case Kind::List: g.type = NANOARROW_TYPE_LIST; break;
                case Kind::Decimal: g.type = NANOARROW_TYPE_DECIMAL128; break;  // precision and scale: see result_type
                case Kind::Duration:
                    g.type = NANOARROW_TYPE_DURATION;
                    g.unit = n.literal.unit;
                    break;
            }
            return g;
        case Op::Column:
            g.type = n.column.type;
            g.unit = n.column.unit;
            g.column = column_schema(root, n.column);
            return g;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod: {
            const auto a = guess(*n.args[0], root);
            const auto b = guess(*n.args[1], root);
            if (temporal_result(n.op, a, b, g)) {
                return g;
            }
            if (integer_type(a.type) && integer_type(b.type)) {
                if (a.column != nullptr && a.type == b.type) {
                    return a;  // int32 + int32 stays int32, as DataFusion keeps it
                }
                // A literal takes the column's type (Lance's planner casts it so): int32 + 1 is int32.
                if (a.column != nullptr && n.args[1]->op == Op::Literal) {
                    return a;
                }
                if (b.column != nullptr && n.args[0]->op == Op::Literal) {
                    return b;
                }
                g.type = NANOARROW_TYPE_INT64;
                return g;
            }
            if (integer_type(a.type) && b.type == NANOARROW_TYPE_NA) {
                return a;
            }
            if (a.type == NANOARROW_TYPE_FLOAT && (b.type == NANOARROW_TYPE_FLOAT || integer_type(b.type))) {
                return a;
            }
            g.type = NANOARROW_TYPE_DOUBLE;
            return g;
        }
        case Op::Neg: return guess(*n.args[0], root);
        case Op::Concat: g.type = NANOARROW_TYPE_STRING; return g;
        case Op::Cast: {
            const auto first = n.name.substr(0, n.name.find(' '));
            if (first == "int" || first == "integer" || first == "int32") {
                g.type = NANOARROW_TYPE_INT32;
            } else if (first == "bigint" || first == "int64") {
                g.type = NANOARROW_TYPE_INT64;
            } else if (first == "smallint" || first == "int16") {
                g.type = NANOARROW_TYPE_INT16;
            } else if (first == "tinyint" || first == "int8") {
                g.type = NANOARROW_TYPE_INT8;
            } else if (first == "uint8") {
                g.type = NANOARROW_TYPE_UINT8;
            } else if (first == "uint16") {
                g.type = NANOARROW_TYPE_UINT16;
            } else if (first == "uint32") {
                g.type = NANOARROW_TYPE_UINT32;
            } else if (first == "uint64") {
                g.type = NANOARROW_TYPE_UINT64;
            } else if (first == "float" || first == "real") {
                g.type = NANOARROW_TYPE_FLOAT;
            } else if (first == "double" || first == "decimal" || first == "numeric") {
                g.type = NANOARROW_TYPE_DOUBLE;
            } else if (first == "boolean" || first == "bool") {
                g.type = NANOARROW_TYPE_BOOL;
            } else if (first == "date") {
                g.type = NANOARROW_TYPE_DATE32;
            } else if (first == "timestamp" || first == "datetime") {
                g.type = NANOARROW_TYPE_TIMESTAMP;
                g.unit = NANOARROW_TIME_UNIT_MICRO;
            } else {
                g.type = NANOARROW_TYPE_STRING;
            }
            return g;
        }
        case Op::Func:
            if (n.name == "json_get") {
                g.type = NANOARROW_TYPE_LARGE_BINARY;
            } else if (n.name == "json_get_string" || n.name == "json_extract") {
                g.type = NANOARROW_TYPE_STRING;
            } else if (n.name == "json_get_int" || n.name == "json_array_length") {
                g.type = NANOARROW_TYPE_INT64;
            } else if (n.name == "json_get_float") {
                g.type = NANOARROW_TYPE_DOUBLE;
            } else if (n.name == "lower" || n.name == "upper") {
                g.type = NANOARROW_TYPE_STRING;
            } else if (n.name == "length" || n.name == "char_length" || n.name == "character_length") {
                g.type = NANOARROW_TYPE_INT32;
            } else if (n.name == "abs" || n.name == "coalesce") {
                return guess(*n.args[0], root);
            } else {
                g.type = NANOARROW_TYPE_BOOL;
            }
            return g;
        default:
            g.type = NANOARROW_TYPE_BOOL;
            return g;
    }
}

}  // namespace

namespace {

/// Whether the expression can yield a null, as DataFusion types it (and so Lance's add_columns marks
/// the new field): a literal only when it is NULL, a column when it is nullable, IS [NOT] NULL / TRUE /
/// FALSE never, an operator when one of its inputs can, a function always.
bool may_be_null(const Node& n, const ArrowSchema& root) {
    switch (n.op) {
        case Op::Literal:
            return n.literal.kind == Kind::Null;
        case Op::Column: {
            const ArrowSchema* at = &root;
            for (std::size_t step = 0; step < n.column.indices.size(); ++step) {
                if (!n.column.elements.empty() && n.column.elements[step] != ColumnRef::kNoElement) {
                    return true;  // an element past the end, or of a null list, is null
                }
                at = at->children[n.column.indices[step]];
                if ((at->flags & ARROW_FLAG_NULLABLE) != 0) {
                    return true;
                }
            }
            return n.column.indices.empty();
        }
        case Op::IsNull:
        case Op::IsNotNull:
        case Op::IsTrue:
        case Op::IsFalse:
        case Op::IsNotTrue:
        case Op::IsNotFalse:
            return false;
        case Op::Func:
            return true;
        default:
            return std::any_of(n.args.begin(), n.args.end(),
                               [&](const std::unique_ptr<Node>& a) { return a && may_be_null(*a, root); });
    }
}

}  // namespace

bool Expression::result_type(const std::string& name, ArrowSchema& out, std::string& error) const {
    error.clear();
    if (!binding_) {
        error = "the expression is not bound to a schema";
        return false;
    }
    if (root_->op == Op::Literal && root_->literal.kind == Kind::Decimal) {
        error = "a projection of a decimal literal is not supported in nanolance (use it in a filter)";
        return false;
    }
    TypedPtr typed;
    if (!build_projection(*root_, typed, error) && !error.empty()) {
        return false;
    }
    // _cast_list_f16(list): the same container (list or fixed_size_list), its items Float16.
    const bool list_cast = root_->op == Op::Func && root_->name == "_cast_list_f16";
    auto g = list_cast ? guess(*root_->args[0], binding_->schema) : guess(*root_, binding_->schema);
    if (typed) {
        g = TypeGuess{};
        g.type = typed->dec ? NANOARROW_TYPE_DECIMAL128 : num_arrow(typed->type);
    }
    int rc = NANOARROW_OK;
    if (g.column != nullptr) {
        rc = ArrowSchemaDeepCopy(g.column, &out);
        if (rc == NANOARROW_OK && list_cast) {
            rc = ArrowSchemaSetType(out.children[0], NANOARROW_TYPE_HALF_FLOAT);
        }
    } else {
        ArrowSchemaInit(&out);
        if (g.type == NANOARROW_TYPE_TIMESTAMP) {
            rc = ArrowSchemaSetTypeDateTime(&out, NANOARROW_TYPE_TIMESTAMP, g.unit, nullptr);
        } else if (g.type == NANOARROW_TYPE_DURATION) {
            rc = ArrowSchemaSetTypeDateTime(&out, NANOARROW_TYPE_DURATION, g.unit, nullptr);
        } else if (typed && typed->dec) {
            rc = ArrowSchemaSetTypeDecimal(&out, NANOARROW_TYPE_DECIMAL128, typed->dp, typed->ds);
        } else {
            rc = ArrowSchemaSetType(&out, g.type == NANOARROW_TYPE_NA ? NANOARROW_TYPE_NA : g.type);
        }
    }
    if (rc != NANOARROW_OK || ArrowSchemaSetName(&out, name.c_str()) != NANOARROW_OK) {
        error = "cannot build the expression's type";
        if (out.release != nullptr) {
            out.release(&out);
        }
        return false;
    }
    if (may_be_null(*root_, binding_->schema)) {
        out.flags |= ARROW_FLAG_NULLABLE;
    } else {
        out.flags &= ~ARROW_FLAG_NULLABLE;
    }
    return true;
}

std::string Expression::to_string() const { return root_ ? print(*root_) : std::string(); }

// ── conditions: the filter as an index sees it ──────────────────────────────────────────────────

namespace {

std::unique_ptr<Node> clone(const Node& n) {
    auto out = std::make_unique<Node>();
    out->op = n.op;
    out->literal = n.literal;
    out->column = n.column;
    out->name = n.name;
    out->source = n.source;
    for (const auto& a : n.args) {
        out->args.push_back(clone(*a));
    }
    return out;
}

bool constant(const Node& n) {
    if (n.op == Op::Column) {
        return false;
    }
    return std::all_of(n.args.begin(), n.args.end(), [](const std::unique_ptr<Node>& a) { return constant(*a); });
}

std::shared_ptr<const Node> shared_clone(const Node& n) { return std::shared_ptr<const Node>(clone(n).release()); }

std::shared_ptr<const Node> literal_node(const Value& v) {
    auto n = std::make_shared<Node>();
    n->op = Op::Literal;
    n->literal = v;
    return n;
}

const char* flipped(Op op) {
    switch (op) {
        case Op::Eq: return "=";
        case Op::Ne: return "!=";
        case Op::Lt: return ">";
        case Op::Le: return ">=";
        case Op::Gt: return "<";
        case Op::Ge: return "<=";
        default: return "?";
    }
}

Op op_of(const std::string& op) {
    if (op == "=") return Op::Eq;
    if (op == "!=") return Op::Ne;
    if (op == "<") return Op::Lt;
    if (op == "<=") return Op::Le;
    if (op == ">") return Op::Gt;
    return Op::Ge;
}

}  // namespace

Condition Expression::conditions() const {
    struct Walk {
        /// The column a test reads: `x`, or a JSON accessor of it at an identifier path.
        static bool column_of(const Node& n, Predicate& p) {
            if (n.op == Op::Column) {
                if (!n.column.elements.empty()) {  // `l[1]` is not a column of the dataset: no pushdown
                    return false;
                }
                p.column_ = n.column.path;
                return true;
            }
            static const char* const kAccessors[] = {"json_get_int", "json_get_float", "json_get_bool",
                                                     "json_get_string"};
            if (n.op != Op::Func || n.args.size() != 2U || n.args[0]->op != Op::Column ||
                n.args[1]->op != Op::Literal || n.args[1]->literal.kind != Kind::String ||
                std::none_of(std::begin(kAccessors), std::end(kAccessors),
                             [&](const char* f) { return n.name == f; })) {
                return false;
            }
            const std::string path(n.args[1]->literal.s);
            const auto alpha = [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; };
            const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
            const bool identifier = !path.empty() && alpha(path[0]) &&
                                    std::all_of(path.begin() + 1, path.end(),
                                                [&](char ch) { return alpha(ch) || digit(ch); });
            if (!identifier || !n.args[0]->column.elements.empty()) {
                return false;
            }
            p.column_ = n.args[0]->column.path;
            p.json_ = JsonAccess{path, n.name};
            return true;
        }

        static Condition at(const Node& n) {
            Condition c;
            auto leaf = [&](Predicate p) {
                c.kind = Condition::Kind::Predicate;
                c.predicate = std::make_shared<const Predicate>(std::move(p));
                return c;
            };
            switch (n.op) {
                case Op::And:
                case Op::Or:
                    c.kind = n.op == Op::And ? Condition::Kind::And : Condition::Kind::Or;
                    c.children.push_back(at(*n.args[0]));
                    c.children.push_back(at(*n.args[1]));
                    return c;
                case Op::Not:
                    c.kind = Condition::Kind::Not;
                    c.children.push_back(at(*n.args[0]));
                    return c;
                case Op::Eq:
                case Op::Ne:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge: {
                    Predicate p;
                    const bool left = constant(*n.args[1]) && column_of(*n.args[0], p);
                    const bool right = !left && constant(*n.args[0]) && column_of(*n.args[1], p);
                    if (!left && !right) {
                        return c;
                    }
                    p.test_ = Predicate::Test::Compare;
                    p.op_ = left ? op_text(n.op) : flipped(n.op);
                    p.constants_.push_back(shared_clone(left ? *n.args[1] : *n.args[0]));
                    return leaf(std::move(p));
                }
                case Op::In:
                case Op::Between: {
                    Predicate p;
                    if (!std::all_of(n.args.begin() + 1, n.args.end(),
                                     [](const std::unique_ptr<Node>& a) { return constant(*a); }) ||
                        !column_of(*n.args[0], p)) {
                        return c;
                    }
                    p.test_ = n.op == Op::In ? Predicate::Test::In : Predicate::Test::Between;
                    for (std::size_t k = 1; k < n.args.size(); ++k) {
                        p.constants_.push_back(shared_clone(*n.args[k]));
                    }
                    return leaf(std::move(p));
                }
                case Op::IsNull:
                case Op::IsNotNull: {
                    Predicate p;
                    if (!column_of(*n.args[0], p)) {
                        return c;
                    }
                    p.test_ = n.op == Op::IsNull ? Predicate::Test::IsNull : Predicate::Test::IsNotNull;
                    return leaf(std::move(p));
                }
                case Op::Func: {
                    if (n.name == "contains") {
                        if (n.args.size() != 2U || n.args[0]->op != Op::Column || n.args[1]->op != Op::Literal ||
                            n.args[1]->literal.kind != Kind::String || !n.args[0]->column.elements.empty()) {
                            return c;
                        }
                        Predicate p;
                        p.test_ = Predicate::Test::Contains;
                        p.column_ = n.args[0]->column.path;
                        p.constants_.push_back(literal_node(n.args[1]->literal));
                        return leaf(std::move(p));
                    }
                    if (!is_array_function(n.name) || n.args.size() != 2U || n.args[0]->op != Op::Column ||
                        n.args[1]->op != Op::Literal || !n.args[0]->column.elements.empty()) {
                        return c;
                    }
                    const bool any = n.name == "array_has_any" || n.name == "list_has_any";
                    const bool all = n.name == "array_has_all" || n.name == "list_has_all";
                    const auto& v = n.args[1]->literal;
                    Predicate p;
                    p.column_ = n.args[0]->column.path;
                    if (any || all) {
                        if (v.kind != Kind::List) {
                            return c;
                        }
                        p.test_ = any ? Predicate::Test::HasAny : Predicate::Test::HasAll;
                        for (const auto& item : *v.items) {
                            p.constants_.push_back(literal_node(item));
                        }
                    } else {
                        if (v.kind == Kind::List) {
                            return c;
                        }
                        p.test_ = Predicate::Test::Has;
                        p.constants_.push_back(literal_node(v));
                    }
                    return leaf(std::move(p));
                }
                default:
                    return c;
            }
        }
    };
    if (!root_) {
        return {};
    }
    return Walk::at(*root_);
}

Predicate Predicate::compare_with(const std::string& op, std::size_t k) const {
    Predicate p;
    p.test_ = Test::Compare;
    p.column_ = column_;
    p.op_ = op;
    p.constants_.push_back(constants_.at(k));
    return p;
}

namespace {

/// The predicate as a tree over a column named `key`.
std::unique_ptr<Node> predicate_node(Predicate::Test test, const std::string& op,
                                     const std::vector<std::shared_ptr<const Node>>& constants,
                                     const std::string& key) {
    auto column = std::make_unique<Node>();
    column->op = Op::Column;
    column->column.path = {key};
    auto n = std::make_unique<Node>();
    n->args.push_back(std::move(column));
    switch (test) {
        case Predicate::Test::Compare: n->op = op_of(op); break;
        case Predicate::Test::In:
        case Predicate::Test::HasAny:
        case Predicate::Test::HasAll:
        case Predicate::Test::Has: n->op = Op::In; break;  // on the elements: one the test names
        case Predicate::Test::Between: n->op = Op::Between; break;
        case Predicate::Test::IsNull: n->op = Op::IsNull; break;
        case Predicate::Test::IsNotNull: n->op = Op::IsNotNull; break;
        case Predicate::Test::Contains:
            n->op = Op::Func;
            n->name = "contains";
            break;
    }
    for (const auto& c : constants) {
        n->args.push_back(clone(*c));
    }
    return n;
}

void no_release_array(ArrowArray* array) { array->release = nullptr; }

}  // namespace

bool Predicate::filter(const ArrowSchema& type, const ArrowArray& values, std::vector<std::uint8_t>& pass,
                       std::string& error) const {
    error.clear();
    if ((test_ == Test::HasAny || test_ == Test::HasAll || test_ == Test::In) && constants_.empty()) {
        pass.assign(static_cast<std::size_t>(values.length), 0U);
        return true;
    }
    static const char* kKey = "__index_key";
    Expression e;
    e.root_ = predicate_node(test_, op_, constants_, kKey);
    ArrowSchema schema;
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 1) != NANOARROW_OK) {
        schema.release(&schema);
        error = "out of memory";
        return false;
    }
    schema.children[0]->release(schema.children[0]);
    if (ArrowSchemaDeepCopy(&type, schema.children[0]) != NANOARROW_OK ||
        ArrowSchemaSetName(schema.children[0], kKey) != NANOARROW_OK) {
        schema.release(&schema);
        error = "out of memory";
        return false;
    }
    const bool bound = e.bind(schema, error);
    schema.release(&schema);
    if (!bound) {
        return false;
    }
    // A struct batch around `values`, borrowing them.
    ArrowArray child = values;
    child.release = no_release_array;
    ArrowArray* children[1] = {&child};
    const void* buffers[1] = {nullptr};
    ArrowArray batch{};
    batch.length = values.length;
    batch.n_buffers = 1;
    batch.buffers = buffers;
    batch.n_children = 1;
    batch.children = children;
    batch.release = no_release_array;
    return e.filter(batch, pass, error);
}

bool Predicate::constant_as(std::size_t k, const ArrowSchema& type, ArrowArray& out, std::string& error) const {
    error.clear();
    if (k >= constants_.size()) {
        error = "no such constant";
        return false;
    }
    // The constant alone, bound to an empty schema, evaluated over one row of no columns.
    Expression e;
    e.root_ = clone(*constants_[k]);
    ArrowSchema schema;
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 0) != NANOARROW_OK) {
        schema.release(&schema);
        error = "out of memory";
        return false;
    }
    const bool bound = e.bind(schema, error);
    schema.release(&schema);
    if (!bound) {
        return false;
    }
    const void* buffers[1] = {nullptr};
    ArrowArray batch{};
    batch.length = 1;
    batch.n_buffers = 1;
    batch.buffers = buffers;
    batch.n_children = 0;
    batch.children = nullptr;
    batch.release = no_release_array;
    return e.evaluate(batch, type, out, error);
}

bool Predicate::routes_to_json_index(const char* value_format) const {
    if (!json_ || value_format == nullptr) {
        return false;
    }
    // The accessor's type is the index's value type: Int64, Float64, Boolean, Utf8 (Arrow's l, g, b, u).
    const std::string& fn = json_->function;
    const std::string_view want = fn == "json_get_int"     ? "l"
                                  : fn == "json_get_float" ? "g"
                                  : fn == "json_get_bool"  ? "b"
                                                           : "u";
    if (std::string_view(value_format) != want) {
        return false;
    }
    // Constants of the accessor's type: DataFusion casts an integer to a float accessor's type, and
    // nothing else is routed (`= 7.0` on an integer accessor is a CAST, which pylance scans).
    for (const auto& c : constants_) {
        if (c->op != Op::Literal) {
            return false;
        }
        const Kind k = c->literal.kind;
        const bool ok = fn == "json_get_int"     ? k == Kind::Int
                        : fn == "json_get_float" ? (k == Kind::Int || k == Kind::Double)
                        : fn == "json_get_bool"  ? k == Kind::Bool
                                                 : k == Kind::String;
        if (!ok) {
            return false;
        }
    }
    // A bool's equality, inequality and IN are rewritten to the bare accessor, which pylance scans.
    if (fn == "json_get_bool" &&
        (test_ == Test::In || (test_ == Test::Compare && (op_ == "=" || op_ == "!=")))) {
        return false;
    }
    return true;
}

std::string Predicate::to_string() const {
    std::string column;
    for (const auto& p : column_) {
        column += (column.empty() ? "" : ".") + p;
    }
    if (json_) {
        // Lance's form: the test on the value, then the path. A one-item IN is an equality there, and a
        // string or bool constant is written bare (true, not TRUE).
        const auto value = [](const Node& n) {
            if (n.op == Op::Literal && n.literal.kind == Kind::String) {
                return std::string(n.literal.s);
            }
            if (n.op == Op::Literal && n.literal.kind == Kind::Bool) {
                return std::string(n.literal.b ? "true" : "false");
            }
            return print(n);
        };
        const std::string at = "->" + json_->path + ")";
        const std::string head = "Json(" + column + " ";
        switch (test_) {
            case Test::Compare: return head + op_ + " " + value(*constants_.front()) + at;
            case Test::In: {
                if (constants_.size() == 1U) {
                    return head + "= " + value(*constants_.front()) + at;
                }
                std::string items;
                for (const auto& c : constants_) {
                    items += (items.empty() ? "" : ",") + value(*c);
                }
                return head + "IN [" + items + "]" + at;
            }
            case Test::Between:  // Lance prints the two bounds as an AND of two queries
                return head + ">= " + value(*constants_[0]) + " && " + column + " <= " + value(*constants_[1]) + at;
            case Test::IsNull: return head + "IS NULL" + at;
            case Test::IsNotNull: return head + "IS NOT NULL" + at;
            default: break;  // not a JSON accessor's test
        }
    }
    std::string list;
    for (const auto& c : constants_) {
        list += (list.empty() ? "" : ", ") + print(*c);
    }
    switch (test_) {
        case Test::Compare: return column + " " + op_ + " " + print(*constants_.front());
        case Test::Between:
            return column + " >= " + print(*constants_[0]) + " && " + column + " <= " + print(*constants_[1]);
        case Test::In: return column + " IN [" + list + "]";
        case Test::IsNull: return column + " IS NULL";
        case Test::IsNotNull: return column + " IS NOT NULL";
        case Test::HasAny: return "array_has_any(" + column + ", [" + list + "])";
        case Test::HasAll: return "array_has_all(" + column + ", [" + list + "])";
        case Test::Has: return "array_has(" + column + ", " + list + ")";
        case Test::Contains:  // Lance prints the needle as a Utf8 literal
            return "contains(" + column + ", Utf8(\"" + std::string(constants_.front()->literal.s) + "\"))";
    }
    return column;
}

}  // namespace nano_lance::expr
