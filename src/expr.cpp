// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// SQL expressions over Arrow batches: a small recursive-descent parser for the filter dialect Lance
// uses (see expr.hpp) and a row-at-a-time evaluator over nanoarrow array views. Correctness first:
// the evaluator walks the tree per row, which costs tens of nanoseconds a row -- small next to the
// decode that produced the batch.

#include "nanolance/expr.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace nano_lance::expr {

// ── values ──────────────────────────────────────────────────────────────────────────────────────

namespace detail {

enum class Kind { Null, Bool, Int, UInt, Double, String, Binary, Date, Timestamp, List };

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
    bool temporal() const { return kind == Kind::Date || kind == Kind::Timestamp; }
    double as_double() const {
        switch (kind) {
            case Kind::Int: return static_cast<double>(i);
            case Kind::UInt: return static_cast<double>(u);
            case Kind::Double: return d;
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
    std::vector<std::string> path;  // as written: "s.child" is {"s", "child"}
    // Bound: child indices from the batch down, the leaf's storage type, and its timestamp unit.
    std::vector<int64_t> indices;
    ArrowType type = NANOARROW_TYPE_UNINITIALIZED;
    ArrowTimeUnit unit = NANOARROW_TIME_UNIT_SECOND;
};

struct Node {
    Op op = Op::Literal;
    Value literal;
    ColumnRef column;
    std::string name;  // Func: lower-cased name; Cast: target type name
    std::vector<std::unique_ptr<Node>> args;
};

namespace {

// ── tokens ──────────────────────────────────────────────────────────────────────────────────────

enum class Tok { End, Ident, QuotedIdent, Number, String, Op, LParen, RParen, Comma, Dot, LBracket, RBracket };

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
            static const char* two[] = {"<=", ">=", "<>", "!=", "==", "||"};
            t.kind = Tok::Op;
            for (const char* op : two) {
                if (sql.substr(i, 2) == op) {
                    t.text = op;
                }
            }
            if (t.text.empty()) {
                if (std::strchr("=<>+-*/%", c) == nullptr) {
                    error = std::string("unexpected character '") + c + "' at position " + std::to_string(i);
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
                return inner;
            }
            if (inner->op == Op::Literal && inner->literal.kind == Kind::Double) {
                inner->literal.d = -inner->literal.d;
                return inner;
            }
            return make(Op::Neg, std::move(inner));
        }
        if (accept_op("+")) {
            return parse_unary();
        }
        return parse_primary();
    }

    static std::unique_ptr<Node> literal(Value v) {
        auto n = std::make_unique<Node>();
        n->op = Op::Literal;
        n->literal = std::move(v);
        return n;
    }

    std::unique_ptr<Node> parse_primary() {
        const Token t = peek();
        if (t.kind == Tok::Number) {
            next();
            if (t.text.find_first_of(".eE") != std::string::npos) {
                return literal(Value::real(std::strtod(t.text.c_str(), nullptr)));
            }
            std::uint64_t u = 0;
            const auto [p, ec] = std::from_chars(t.text.data(), t.text.data() + t.text.size(), u);
            if (ec != std::errc() || p != t.text.data() + t.text.size()) {
                return literal(Value::real(std::strtod(t.text.c_str(), nullptr)));
            }
            return literal(u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                               ? Value::integer(static_cast<std::int64_t>(u))
                               : Value::uinteger(u));
        }
        if (t.kind == Tok::String) {
            next();
            return literal(Value::string(t.text));
        }
        if (t.kind == Tok::LBracket) {
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
            if (peek().kind == Tok::LParen) {  // VARCHAR(10), DECIMAL(10, 2): the size is ignored
                while (peek().kind != Tok::RParen && peek().kind != Tok::End) {
                    next();
                }
                next();
            }
            if (next().kind != Tok::RParen) {
                return fail("expected ')' to close CAST");
            }
            auto n = make(Op::Cast, std::move(inner));
            n->name = type;
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
            return n;
        }
        return parse_column();
    }

    std::unique_ptr<Node> parse_column() {
        auto n = std::make_unique<Node>();
        n->op = Op::Column;
        n->column.path.push_back(next().text);
        while (peek().kind == Tok::Dot && (peek(1).kind == Tok::Ident || peek(1).kind == Tok::QuotedIdent)) {
            next();
            n->column.path.push_back(next().text);
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
            for (const auto& p : n.column.path) {
                out += (out.empty() ? "" : ".") + p;
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
        std::string full;
        for (const auto& p : n.column.path) {
            full += (full.empty() ? "" : ".") + p;
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
            return true;
        default:
            return false;
    }
}

bool bind_node(Node& n, const ArrowSchema& schema, bool under_null_test, std::string& error) {
    if (n.op == Op::Column) {
        auto& c = n.column;
        c.indices.clear();
        const ArrowSchema* at = &schema;
        // "a.b" may be a column named "a.b" as well as a struct path.
        std::string joined;
        for (const auto& p : c.path) {
            joined += (joined.empty() ? "" : ".") + p;
        }
        const int64_t whole = c.path.size() > 1 ? find_child(schema, joined) : -1;
        if (whole >= 0) {
            c.indices.push_back(whole);
            at = schema.children[whole];
        } else {
            for (const auto& p : c.path) {
                const int64_t k = find_child(*at, p);
                if (k < 0) {
                    error = "column '" + joined + "' not found";
                    return false;
                }
                c.indices.push_back(k);
                at = at->children[k];
            }
        }
        ArrowSchemaView sv;
        ArrowError aerr;
        if (ArrowSchemaViewInit(&sv, at, &aerr) != NANOARROW_OK) {
            error = std::string("cannot read the type of column '") + joined + "': " + aerr.message;
            return false;
        }
        c.type = sv.type;
        c.unit = sv.time_unit;
        if (!scalar_type(sv.type) && !under_null_test) {
            error = "column '" + joined + "' has a type filters cannot compare (only IS NULL / IS NOT NULL)";
            return false;
        }
        return true;
    }
    const bool null_test = n.op == Op::IsNull || n.op == Op::IsNotNull ||
                           (n.op == Op::Func && (n.name == "is_null" || n.name == "is_valid"));
    for (auto& a : n.args) {
        if (!bind_node(*a, schema, null_test && a.get() == n.args.front().get(), error)) {
            return false;
        }
    }
    if (n.op == Op::Func) {
        static const std::unordered_map<std::string, std::pair<int, int>> arity = {
            {"lower", {1, 1}},      {"upper", {1, 1}},       {"length", {1, 1}},   {"char_length", {1, 1}},
            {"character_length", {1, 1}}, {"abs", {1, 1}},   {"coalesce", {1, 64}}, {"starts_with", {2, 2}},
            {"ends_with", {2, 2}},  {"contains", {2, 2}},    {"is_null", {1, 1}},  {"is_valid", {1, 1}},
            {"invert", {1, 1}},     {"and_", {2, 2}},        {"or_", {2, 2}},      {"equal", {2, 2}},
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
    }
    return true;
}

bool temporal_type(ArrowType t) {
    return t == NANOARROW_TYPE_DATE32 || t == NANOARROW_TYPE_DATE64 || t == NANOARROW_TYPE_TIMESTAMP;
}

/// A string literal compared with a date or timestamp column becomes a date / timestamp literal,
/// as DataFusion coerces it.
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
    const Node* column = nullptr;
    for (const auto& a : n.args) {
        if (a->op == Op::Column && temporal_type(a->column.type)) {
            column = a.get();
        }
    }
    if (column == nullptr) {
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

    Value fail(const std::string& why) {
        if (!failed) {
            failed = true;
            *error = why;
        }
        return Value::null();
    }
};

Value read_column(const ColumnRef& c, int64_t row, Context& ctx) {
    const ArrowArrayView* view = ctx.batch;
    int64_t at = row;
    for (const auto index : c.indices) {
        if (view->storage_type == NANOARROW_TYPE_STRUCT && ArrowArrayViewIsNull(view, at)) {
            return Value::null();
        }
        at += view->offset;
        view = view->children[index];
    }
    if (ArrowArrayViewIsNull(view, at)) {
        return Value::null();
    }
    switch (c.type) {
        case NANOARROW_TYPE_BOOL: return Value::boolean(ArrowArrayViewGetIntUnsafe(view, at) != 0);
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64:
        case NANOARROW_TYPE_DURATION:
            return Value::integer(ArrowArrayViewGetIntUnsafe(view, at));
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
        case NANOARROW_TYPE_TIMESTAMP: return Value::timestamp(ArrowArrayViewGetIntUnsafe(view, at) * unit_to_ns(c.unit));
        default:
            // Bound only under a null test, where the value itself is never read: any non-null.
            return Value::boolean(true);
    }
}

/// -1, 0, 1, or nullopt when the two cannot be compared.
std::optional<int> compare(const Value& a, const Value& b) {
    if (a.numeric() && b.numeric()) {
        if (a.kind == Kind::Double || b.kind == Kind::Double) {
            const double x = a.as_double();
            const double y = b.as_double();
            return x < y ? -1 : (x > y ? 1 : 0);
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

Value arith(Op op, const Value& a, const Value& b, Context& ctx) {
    if (a.is_null() || b.is_null()) {
        return Value::null();
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
    const auto first = type.substr(0, type.find(' '));
    if (first == "int" || first == "integer" || first == "bigint" || first == "smallint" || first == "tinyint" ||
        first == "int8" || first == "int16" || first == "int32" || first == "int64") {
        if (v.kind == Kind::Int) {
            return v;
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
        if (v.numeric()) {
            return Value::real(v.as_double());
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
        case Op::Cast: return cast_value(eval(*n.args[0], row, ctx), n.name, ctx);
        case Op::Func: {
            const auto& f = n.name;
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
bool append_value(ArrowArray& out, const ArrowSchemaView& type, const Value& v, Context& ctx) {
    if (v.is_null()) {
        return ArrowArrayAppendNull(&out, 1) == NANOARROW_OK;
    }
    switch (type.type) {
        case NANOARROW_TYPE_BOOL: {
            const Value b = cast_value(v, "boolean", ctx);
            return !ctx.failed && ArrowArrayAppendInt(&out, b.b ? 1 : 0) == NANOARROW_OK;
        }
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64:
        case NANOARROW_TYPE_DURATION: {
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
        case NANOARROW_TYPE_HALF_FLOAT:
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

bool Expression::evaluate(const ArrowArray& batch, const ArrowSchema& type, ArrowArray& out, std::string& error) const {
    error.clear();
    if (!binding_) {
        error = "the expression is not bound to a schema";
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
    for (int64_t r = 0; r < batch.length; ++r) {
        const Value v = eval(*root_, r, ctx);
        if (ctx.failed || !append_value(out, tv, v, ctx)) {
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
    for (const auto index : c.indices) {
        at = at->children[index];
    }
    return at;
}

bool integer_type(ArrowType t) {
    return t == NANOARROW_TYPE_INT8 || t == NANOARROW_TYPE_INT16 || t == NANOARROW_TYPE_INT32 ||
           t == NANOARROW_TYPE_INT64 || t == NANOARROW_TYPE_UINT8 || t == NANOARROW_TYPE_UINT16 ||
           t == NANOARROW_TYPE_UINT32 || t == NANOARROW_TYPE_UINT64;
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
            if (integer_type(a.type) && integer_type(b.type)) {
                if (a.column != nullptr && a.type == b.type) {
                    return a;  // int32 + int32 stays int32, as DataFusion keeps it
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
            } else if (first == "float" || first == "real") {
                g.type = NANOARROW_TYPE_FLOAT;
            } else if (first == "double" || first == "decimal" || first == "numeric") {
                g.type = NANOARROW_TYPE_DOUBLE;
            } else if (first == "boolean" || first == "bool") {
                g.type = NANOARROW_TYPE_BOOL;
            } else if (first == "date") {
                g.type = NANOARROW_TYPE_DATE32;
            } else if (first == "timestamp") {
                g.type = NANOARROW_TYPE_TIMESTAMP;
                g.unit = NANOARROW_TIME_UNIT_MICRO;
            } else {
                g.type = NANOARROW_TYPE_STRING;
            }
            return g;
        }
        case Op::Func:
            if (n.name == "lower" || n.name == "upper") {
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

bool Expression::result_type(const std::string& name, ArrowSchema& out, std::string& error) const {
    error.clear();
    if (!binding_) {
        error = "the expression is not bound to a schema";
        return false;
    }
    const auto g = guess(*root_, binding_->schema);
    int rc = NANOARROW_OK;
    if (g.column != nullptr) {
        rc = ArrowSchemaDeepCopy(g.column, &out);
    } else {
        ArrowSchemaInit(&out);
        if (g.type == NANOARROW_TYPE_TIMESTAMP) {
            rc = ArrowSchemaSetTypeDateTime(&out, NANOARROW_TYPE_TIMESTAMP, g.unit, nullptr);
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
    out.flags |= ARROW_FLAG_NULLABLE;
    return true;
}

std::string Expression::to_string() const { return root_ ? print(*root_) : std::string(); }

}  // namespace nano_lance::expr
