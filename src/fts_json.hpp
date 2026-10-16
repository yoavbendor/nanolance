// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// A small JSON reader and writer (internal): what an INVERTED index's params and full-text queries
// need.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nano_lance::fts::json {

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;                           // Array
    std::vector<std::pair<std::string, Value>> fields;  // Object, in order

    const Value* get(std::string_view key) const {
        for (const auto& [k, v] : fields) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    /// The whole text as one value.
    bool parse(Value& out) {
        if (!value(out, 0)) {
            return false;
        }
        space();
        return i_ == text_.size();
    }

private:
    void space() {
        while (i_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[i_])) != 0) {
            ++i_;
        }
    }
    bool literal(std::string_view word) {
        if (text_.substr(i_, word.size()) == word) {
            i_ += word.size();
            return true;
        }
        return false;
    }
    static bool hex4(std::string_view s, std::uint32_t& out) {
        out = 0;
        if (s.size() < 4) {
            return false;
        }
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = s[k];
            const int d = c >= '0' && c <= '9' ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (d < 0) {
                return false;
            }
            out = out * 16U + static_cast<std::uint32_t>(d);
        }
        return true;
    }
    static void utf8(std::uint32_t cp, std::string& out) {
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
    bool string(std::string& out) {
        space();
        if (i_ >= text_.size() || text_[i_] != '"') {
            return false;
        }
        ++i_;
        out.clear();
        while (i_ < text_.size() && text_[i_] != '"') {
            char c = text_[i_++];
            if (c == '\\') {
                if (i_ >= text_.size()) {
                    return false;
                }
                c = text_[i_++];
                switch (c) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        std::uint32_t cp = 0;
                        if (!hex4(text_.substr(i_), cp)) {
                            return false;
                        }
                        i_ += 4;
                        std::uint32_t lo = 0;
                        if (cp >= 0xD800U && cp < 0xDC00U && text_.substr(i_, 2) == "\\u" &&
                            hex4(text_.substr(i_ + 2), lo) && lo >= 0xDC00U && lo < 0xE000U) {
                            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
                            i_ += 6;
                        }
                        utf8(cp, out);
                        continue;
                    }
                    default: break;  // \" \\ \/
                }
            }
            out.push_back(c);
        }
        if (i_ >= text_.size()) {
            return false;
        }
        ++i_;
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > 64) {
            return false;
        }
        space();
        if (i_ >= text_.size()) {
            return false;
        }
        const char c = text_[i_];
        if (c == '"') {
            v.kind = Value::String;
            return string(v.s);
        }
        if (literal("null")) {
            v.kind = Value::Null;
            return true;
        }
        if (literal("true")) {
            v.kind = Value::Bool;
            v.b = true;
            return true;
        }
        if (literal("false")) {
            v.kind = Value::Bool;
            v.b = false;
            return true;
        }
        if (c == '[' || c == '{') {
            const bool object = c == '{';
            const char close = object ? '}' : ']';
            v.kind = object ? Value::Object : Value::Array;
            ++i_;
            space();
            if (i_ < text_.size() && text_[i_] == close) {
                ++i_;
                return true;
            }
            while (true) {
                Value item;
                std::string key;
                if (object) {
                    if (!string(key)) {
                        return false;
                    }
                    space();
                    if (i_ >= text_.size() || text_[i_] != ':') {
                        return false;
                    }
                    ++i_;
                }
                if (!value(item, depth + 1)) {
                    return false;
                }
                if (object) {
                    v.fields.emplace_back(std::move(key), std::move(item));
                } else {
                    v.items.push_back(std::move(item));
                }
                space();
                if (i_ < text_.size() && text_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < text_.size() && text_[i_] == close) {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        const std::size_t start = i_;
        while (i_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[i_])) != 0 || text_[i_] == '-' ||
                                     text_[i_] == '+' || text_[i_] == '.' || text_[i_] == 'e' || text_[i_] == 'E')) {
            ++i_;
        }
        if (i_ == start) {
            return false;
        }
        v.kind = Value::Number;
        const std::string number(text_.substr(start, i_ - start));
        char* end = nullptr;
        v.n = std::strtod(number.c_str(), &end);
        return end == number.c_str() + number.size();
    }

    std::string_view text_;
    std::size_t i_ = 0;
};

inline bool parse(std::string_view text, Value& out) { return Reader(text).parse(out); }

inline std::string quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20U) {
            static const char* hex = "0123456789abcdef";
            out += "\\u00";
            out.push_back(hex[(static_cast<unsigned char>(c) >> 4U) & 0xFU]);
            out.push_back(hex[static_cast<unsigned char>(c) & 0xFU]);
        } else {
            out.push_back(c);
        }
    }
    return out + "\"";
}

}  // namespace nano_lance::fts::json
