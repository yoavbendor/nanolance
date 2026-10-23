// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lindera 3.0.7 (MIT, https://github.com/lindera/lindera) as Lance 12 runs it: the tokenizer config
// (tokenizer.rs, a YAML file), the segmenter (segmenter.rs: sentences, whitespace tokens, positions), the
// lattice and its forward Viterbi (lindera-dictionary viterbi.rs, normal and decompose modes), the
// dictionary files of a Lindera 3 dictionary directory (metadata.json, char_def.bin, unk.bin, matrix.mtx,
// dict.da / .vals / .wordsidx), user dictionaries (a CSV built as builder/user_dictionary.rs builds it, or
// a .bin), and the mapping character filter. The binary files are read as rkyv 0.8 archives and the
// daachorse 2.1 double array they embed, from their layouts in those crates' sources. Edges are added and
// compared in Lindera's order, so ties between equal-cost paths fall the same way.

#include "nanolance/fts_lindera.hpp"

#include "base/fts_json.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace nano_lance::fts {
namespace {

using json::Value;

// ---------------------------------------------------------------------------------------------------
// Small helpers.

char32_t decode_utf8(std::string_view s, std::size_t i, std::size_t& len) {
    const auto b = static_cast<unsigned char>(s[i]);
    const auto cont = [&](std::size_t k) { return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3FU); };
    if (b < 0x80U) {
        len = 1;
        return b;
    }
    if ((b & 0xE0U) == 0xC0U && i + 1 < s.size()) {
        len = 2;
        return (static_cast<char32_t>(b & 0x1FU) << 6U) | cont(1);
    }
    if ((b & 0xF0U) == 0xE0U && i + 2 < s.size()) {
        len = 3;
        return (static_cast<char32_t>(b & 0x0FU) << 12U) | (cont(1) << 6U) | cont(2);
    }
    if ((b & 0xF8U) == 0xF0U && i + 3 < s.size()) {
        len = 4;
        return (static_cast<char32_t>(b & 0x07U) << 18U) | (cont(1) << 12U) | (cont(2) << 6U) | cont(3);
    }
    len = 1;
    return b;
}

std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8U)); }
std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::int32_t sat_add(std::int32_t a, std::int32_t b) {
    const std::int64_t s = static_cast<std::int64_t>(a) + b;
    return s > std::numeric_limits<std::int32_t>::max()   ? std::numeric_limits<std::int32_t>::max()
           : s < std::numeric_limits<std::int32_t>::min() ? std::numeric_limits<std::int32_t>::min()
                                                          : static_cast<std::int32_t>(s);
}

/// Lindera's util::read_file, with its error text.
bool read_file(const std::filesystem::path& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "LinderaError(kind=Io, source=Failed to open file: " + path.string() + ")";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string file_stamp(const std::filesystem::path& p) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    const auto time = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
    return p.string() + ":" + std::to_string(ec ? 0ULL : static_cast<unsigned long long>(size)) + ":" +
           std::to_string(static_cast<long long>(time));
}

// ---------------------------------------------------------------------------------------------------
// The config: the block-style YAML a Lindera tokenizer config uses (mappings, sequences, plain and quoted
// scalars, flow collections on one line), read into the JSON value serde_yaml_ng's conversion gives.

class Yaml {
public:
    bool parse(std::string_view text, Value& out, std::string& error) {
        std::size_t at = 0;
        while (at <= text.size()) {
            std::size_t end = text.find('\n', at);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            std::string_view line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            std::size_t indent = 0;
            while (indent < line.size() && line[indent] == ' ') {
                ++indent;
            }
            if (indent < line.size() && line[indent] == '\t') {
                error = "tabs are not allowed for indentation";
                return false;
            }
            std::string content(strip_comment(line.substr(indent)));
            while (!content.empty() && (content.back() == ' ' || content.back() == '\t')) {
                content.pop_back();
            }
            if (content.empty() || (lines_.empty() && (content == "---" || content.rfind("%", 0) == 0))) {
                continue;
            }
            if (content == "..." ) {
                break;
            }
            lines_.push_back({indent, content});
        }
        if (lines_.empty()) {
            out = Value();
            return true;
        }
        std::size_t i = 0;
        if (!node(i, lines_[0].indent, out, error)) {
            return false;
        }
        if (i != lines_.size()) {
            error = "did not find expected key at line " + std::to_string(i + 1);
            return false;
        }
        return true;
    }

private:
    struct Line {
        std::size_t indent;
        std::string text;
    };

    static std::string_view strip_comment(std::string_view s) {
        char quote = 0;
        for (std::size_t k = 0; k < s.size(); ++k) {
            const char c = s[k];
            if (quote != 0) {
                if (c == '\\' && quote == '"') {
                    ++k;
                } else if (c == quote) {
                    quote = 0;
                }
            } else if ((c == '"' || c == '\'') && (k == 0 || s[k - 1] == ' ' || s[k - 1] == ':' || s[k - 1] == '-' ||
                                                    s[k - 1] == '[' || s[k - 1] == '{' || s[k - 1] == ',')) {
                quote = c;
            } else if (c == '#' && (k == 0 || s[k - 1] == ' ' || s[k - 1] == '\t')) {
                return s.substr(0, k);
            }
        }
        return s;
    }

    static bool is_seq_item(const std::string& t) { return t == "-" || t.rfind("- ", 0) == 0; }

    /// The offset of a mapping key's ':' (a colon followed by a space or the end, outside quotes), or npos.
    static std::size_t key_colon(std::string_view t) {
        std::size_t k = 0;
        if (!t.empty() && (t[0] == '"' || t[0] == '\'')) {
            const char q = t[0];
            for (k = 1; k < t.size(); ++k) {
                if (q == '"' && t[k] == '\\') {
                    ++k;
                } else if (t[k] == q) {
                    if (q == '\'' && k + 1 < t.size() && t[k + 1] == '\'') {
                        ++k;
                        continue;
                    }
                    break;
                }
            }
            ++k;
            while (k < t.size() && t[k] == ' ') {
                ++k;
            }
            return k < t.size() && t[k] == ':' && (k + 1 == t.size() || t[k + 1] == ' ') ? k : std::string_view::npos;
        }
        if (!t.empty() && (t[0] == '[' || t[0] == '{')) {
            return std::string_view::npos;
        }
        for (k = 0; k < t.size(); ++k) {
            if (t[k] == ':' && (k + 1 == t.size() || t[k + 1] == ' ')) {
                return k;
            }
        }
        return std::string_view::npos;
    }

    bool node(std::size_t& i, std::size_t indent, Value& out, std::string& error) {
        const Line& line = lines_[i];
        if (is_seq_item(line.text)) {
            return sequence(i, line.indent, out, error);
        }
        if (key_colon(line.text) != std::string_view::npos) {
            return mapping(i, line.indent, out, error);
        }
        (void)indent;
        if (!scalar(line.text, out, error)) {
            return false;
        }
        ++i;
        if (i < lines_.size() && lines_[i].indent > line.indent && !is_seq_item(lines_[i].text) &&
            key_colon(lines_[i].text) == std::string_view::npos) {
            error = "multi-line scalars are not supported";
            return false;
        }
        return true;
    }

    bool sequence(std::size_t& i, std::size_t indent, Value& out, std::string& error) {
        out = Value();
        out.kind = Value::Array;
        while (i < lines_.size() && lines_[i].indent == indent && is_seq_item(lines_[i].text)) {
            Line& line = lines_[i];
            Value item;
            if (line.text == "-") {
                ++i;
                if (i < lines_.size() && lines_[i].indent > indent) {
                    if (!node(i, lines_[i].indent, item, error)) {
                        return false;
                    }
                }
            } else {
                // "- key: value": the item is a block whose first line starts after the dash.
                std::size_t skip = 1;
                while (skip < line.text.size() && line.text[skip] == ' ') {
                    ++skip;
                }
                line.indent += skip;
                line.text.erase(0, skip);
                if (!node(i, line.indent, item, error)) {
                    return false;
                }
            }
            out.items.push_back(std::move(item));
        }
        if (i < lines_.size() && lines_[i].indent > indent) {
            error = "bad indentation of a sequence entry at line " + std::to_string(i + 1);
            return false;
        }
        return true;
    }

    bool mapping(std::size_t& i, std::size_t indent, Value& out, std::string& error) {
        out = Value();
        out.kind = Value::Object;
        while (i < lines_.size() && lines_[i].indent == indent && !is_seq_item(lines_[i].text)) {
            const std::string text = lines_[i].text;
            const std::size_t colon = key_colon(text);
            if (colon == std::string_view::npos) {
                error = "could not find expected ':' at line " + std::to_string(i + 1);
                return false;
            }
            std::string key_text = text.substr(0, colon);
            while (!key_text.empty() && key_text.back() == ' ') {
                key_text.pop_back();
            }
            Value key;
            if (!scalar(key_text, key, error)) {
                return false;
            }
            std::string rest = colon + 1 < text.size() ? text.substr(colon + 1) : std::string();
            while (!rest.empty() && rest.front() == ' ') {
                rest.erase(0, 1);
            }
            ++i;
            Value value;
            if (rest.empty()) {
                if (i < lines_.size() && (lines_[i].indent > indent ||
                                          (lines_[i].indent == indent && is_seq_item(lines_[i].text)))) {
                    if (!node(i, lines_[i].indent, value, error)) {
                        return false;
                    }
                }
            } else if (!scalar(rest, value, error)) {
                return false;
            }
            std::string name = key.kind == Value::String ? key.s : key_text;
            for (auto& m : out.fields) {
                if (m.first == name) {
                    error = "duplicate entry with key \"" + name + "\"";
                    return false;
                }
            }
            out.fields.push_back({std::move(name), std::move(value)});
        }
        if (i < lines_.size() && lines_[i].indent > indent) {
            error = "mapping values are not allowed in this context at line " + std::to_string(i + 1);
            return false;
        }
        return true;
    }

    /// One scalar or flow collection on a line.
    static bool scalar(std::string_view t, Value& out, std::string& error) {
        std::size_t k = 0;
        if (!flow(t, k, out, false, error)) {
            return false;
        }
        while (k < t.size() && t[k] == ' ') {
            ++k;
        }
        if (k != t.size()) {
            error = "unexpected characters after a value: " + std::string(t.substr(k));
            return false;
        }
        return true;
    }

    static bool flow(std::string_view t, std::size_t& k, Value& out, bool in_flow, std::string& error) {
        while (k < t.size() && t[k] == ' ') {
            ++k;
        }
        out = Value();
        if (k < t.size() && (t[k] == '[' || t[k] == '{')) {
            const bool seq = t[k] == '[';
            const char close = seq ? ']' : '}';
            out.kind = seq ? Value::Array : Value::Object;
            ++k;
            for (;;) {
                while (k < t.size() && t[k] == ' ') {
                    ++k;
                }
                if (k < t.size() && t[k] == close) {
                    ++k;
                    return true;
                }
                Value item;
                if (!flow(t, k, item, true, error)) {
                    return false;
                }
                while (k < t.size() && t[k] == ' ') {
                    ++k;
                }
                if (!seq) {
                    if (k >= t.size() || t[k] != ':') {
                        error = "did not find expected ':' in a flow mapping";
                        return false;
                    }
                    ++k;
                    Value value;
                    if (!flow(t, k, value, true, error)) {
                        return false;
                    }
                    out.fields.push_back({item.kind == Value::String ? item.s : std::string(), std::move(value)});
                } else {
                    out.items.push_back(std::move(item));
                }
                while (k < t.size() && t[k] == ' ') {
                    ++k;
                }
                if (k < t.size() && t[k] == ',') {
                    ++k;
                    continue;
                }
                if (k < t.size() && t[k] == close) {
                    ++k;
                    return true;
                }
                error = "did not find expected ',' or '" + std::string(1, close) + "'";
                return false;
            }
        }
        if (k < t.size() && t[k] == '"') {
            out.kind = Value::String;
            ++k;
            while (k < t.size() && t[k] != '"') {
                char c = t[k++];
                if (c == '\\' && k < t.size()) {
                    c = t[k++];
                    switch (c) {
                        case 'n': out.s.push_back('\n'); break;
                        case 't': out.s.push_back('\t'); break;
                        case 'r': out.s.push_back('\r'); break;
                        case '0': out.s.push_back('\0'); break;
                        case '"': case '\\': case '/': case ' ': out.s.push_back(c); break;
                        case 'u': case 'U': case 'x': {
                            const std::size_t n = c == 'x' ? 2 : c == 'u' ? 4 : 8;
                            if (k + n > t.size()) {
                                error = "bad escape in a double-quoted scalar";
                                return false;
                            }
                            const std::string hex(t.substr(k, n));
                            char* end = nullptr;
                            const unsigned long cp = std::strtoul(hex.c_str(), &end, 16);
                            if (end != hex.c_str() + n) {
                                error = "bad escape in a double-quoted scalar";
                                return false;
                            }
                            k += n;
                            append_utf8(static_cast<std::uint32_t>(cp), out.s);
                            break;
                        }
                        default:
                            error = "unknown escape character in a double-quoted scalar";
                            return false;
                    }
                } else {
                    out.s.push_back(c);
                }
            }
            if (k >= t.size()) {
                error = "found unexpected end of stream while scanning a quoted scalar";
                return false;
            }
            ++k;
            return true;
        }
        if (k < t.size() && t[k] == '\'') {
            out.kind = Value::String;
            ++k;
            for (;;) {
                if (k >= t.size()) {
                    error = "found unexpected end of stream while scanning a quoted scalar";
                    return false;
                }
                if (t[k] == '\'') {
                    if (k + 1 < t.size() && t[k + 1] == '\'') {
                        out.s.push_back('\'');
                        k += 2;
                        continue;
                    }
                    ++k;
                    return true;
                }
                out.s.push_back(t[k++]);
            }
        }
        if (k < t.size() && (t[k] == '&' || t[k] == '*' || t[k] == '!' || t[k] == '|' || t[k] == '>')) {
            error = "anchors, aliases, tags and block scalars are not supported";
            return false;
        }
        // A plain scalar: up to the end, or (in a flow collection) a ',', ']', '}' or ": ".
        const std::size_t from = k;
        while (k < t.size()) {
            if (in_flow && (t[k] == ',' || t[k] == ']' || t[k] == '}' ||
                            (t[k] == ':' && (k + 1 == t.size() || t[k + 1] == ' ' || t[k + 1] == ',')))) {
                break;
            }
            ++k;
        }
        std::string plain(t.substr(from, k - from));
        while (!plain.empty() && plain.back() == ' ') {
            plain.pop_back();
        }
        resolve_plain(plain, out);
        return true;
    }

    /// The YAML 1.2 core schema serde_yaml_ng resolves plain scalars with.
    static void resolve_plain(const std::string& p, Value& out) {
        if (p.empty() || p == "~" || p == "null" || p == "Null" || p == "NULL") {
            out.kind = Value::Null;
            return;
        }
        if (p == "true" || p == "True" || p == "TRUE" || p == "false" || p == "False" || p == "FALSE") {
            out.kind = Value::Bool;
            out.b = p[0] == 't' || p[0] == 'T';
            return;
        }
        const char* s = p.c_str();
        char* end = nullptr;
        std::size_t digits = (p[0] == '-' || p[0] == '+') ? 1 : 0;
        if (digits < p.size() && std::isdigit(static_cast<unsigned char>(p[digits])) != 0) {
            const bool hex = p.compare(digits, 2, "0x") == 0;
            const bool oct = p.compare(digits, 2, "0o") == 0;
            if (hex || oct) {
                const long long v = std::strtoll(s + digits + 2, &end, hex ? 16 : 8);
                if (end == s + p.size() && p.size() > digits + 2) {
                    out.kind = Value::Number;
                    out.n = static_cast<double>(p[0] == '-' ? -v : v);
                    return;
                }
            } else {
                const double v = std::strtod(s, &end);
                if (end == s + p.size()) {
                    out.kind = Value::Number;
                    out.n = v;
                    return;
                }
            }
        }
        out.kind = Value::String;
        out.s = p;
    }

    static void append_utf8(std::uint32_t cp, std::string& out) {
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

    std::vector<Line> lines_;
};

// ---------------------------------------------------------------------------------------------------
// rkyv 0.8 archives (little-endian, 32-bit relative pointers): the root object ends the buffer; a Vec is
// a 4-byte offset from the field itself and a 4-byte length; a String is inline (up to 8 bytes, padded
// with 0xFF) or out of line (a length with 0b10 in its top bits and an offset).

class Archive {
public:
    explicit Archive(std::string_view bytes) : b_(bytes) {}

    bool ok() const { return ok_; }
    std::size_t root(std::size_t size) {
        if (b_.size() < size) {
            ok_ = false;
            return 0;
        }
        return b_.size() - size;
    }
    std::uint32_t u32(std::size_t at) {
        if (at + 4 > b_.size()) {
            ok_ = false;
            return 0;
        }
        return le32(data(at));
    }
    std::uint16_t u16(std::size_t at) {
        if (at + 2 > b_.size()) {
            ok_ = false;
            return 0;
        }
        return le16(data(at));
    }
    std::uint8_t u8(std::size_t at) {
        if (at >= b_.size()) {
            ok_ = false;
            return 0;
        }
        return static_cast<std::uint8_t>(b_[at]);
    }
    /// A Vec field at `at` with elements of `elem` bytes: its first element's offset and its length.
    std::pair<std::size_t, std::size_t> vec(std::size_t at, std::size_t elem) {
        const auto rel = static_cast<std::int32_t>(u32(at));
        const std::size_t len = u32(at + 4);
        const std::int64_t start = static_cast<std::int64_t>(at) + rel;
        if (!ok_ || start < 0 || static_cast<std::uint64_t>(start) + static_cast<std::uint64_t>(len) * elem > b_.size()) {
            ok_ = false;
            return {0, 0};
        }
        return {static_cast<std::size_t>(start), len};
    }
    std::string_view bytes(std::size_t at) {
        const auto [start, len] = vec(at, 1);
        return ok_ ? b_.substr(start, len) : std::string_view();
    }
    std::string string(std::size_t at) {
        if (at + 8 > b_.size()) {
            ok_ = false;
            return {};
        }
        const auto* p = data(at);
        if ((p[0] & 0xC0U) != 0x80U) {
            std::size_t n = 0;
            while (n < 8 && p[n] != 0xFFU) {
                ++n;
            }
            return std::string(reinterpret_cast<const char*>(p), n);
        }
        const std::uint32_t v = le32(p);
        const std::size_t len = (v & 0x3FU) | ((v >> 8U) << 6U);
        const std::int64_t start = static_cast<std::int64_t>(at) + static_cast<std::int32_t>(le32(p + 4));
        if (start < 0 || static_cast<std::uint64_t>(start) + len > b_.size()) {
            ok_ = false;
            return {};
        }
        return std::string(b_.substr(static_cast<std::size_t>(start), len));
    }

private:
    const unsigned char* data(std::size_t at) const { return reinterpret_cast<const unsigned char*>(b_.data()) + at; }
    std::string_view b_;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------------------------------
// The dictionary.

struct WordEntry {
    std::uint32_t id = std::numeric_limits<std::uint32_t>::max();
    std::int16_t cost = 0;
    std::uint16_t left_id = 0;
    std::uint16_t right_id = 0;
};

constexpr std::size_t kWordEntryLen = 10;  // WordEntry::SERIALIZED_LEN

WordEntry word_entry(const unsigned char* p) {
    WordEntry e;
    e.id = le32(p);
    e.cost = static_cast<std::int16_t>(le16(p + 4));
    e.left_id = le16(p + 6);
    e.right_id = le16(p + 8);
    return e;
}

/// A daachorse 2.1 bytewise DoubleArrayAhoCorasick<u32>, serialized: states (base, fail, output_pos << 8 |
/// check) and outputs (value, length, parent). Only its trie is walked: the patterns that start a text.
class DoubleArray {
public:
    bool load(std::string_view bytes) {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
        if (bytes.size() < 4) {
            return false;
        }
        const std::size_t states = le32(p);
        if (4 + states * 12 + 4 > bytes.size()) {
            return false;
        }
        states_ = std::string_view(bytes.data() + 4, states * 12);
        const std::size_t at = 4 + states * 12;
        const std::size_t outputs = le32(p + at);
        if (at + 4 + outputs * 12 > bytes.size()) {
            return false;
        }
        outputs_ = std::string_view(bytes.data() + at + 4, outputs * 12);
        return states > 0;
    }

    /// Calls `f(length, value)` for each pattern that is a prefix of `text`, shortest first (the order
    /// find_overlapping_iter reports the matches that start at one position in).
    template <typename F>
    void prefixes(std::string_view text, F&& f) const {
        std::uint32_t state = 0;
        const std::size_t n_states = states_.size() / 12;
        for (std::size_t depth = 0; depth < text.size(); ++depth) {
            const auto* s = state_at(state);
            const std::uint32_t base = le32(s);
            if (base == 0) {
                return;
            }
            const auto c = static_cast<unsigned char>(text[depth]);
            const std::uint32_t child = base ^ c;
            if (child >= n_states || (le32(state_at(child) + 8) & 0xFFU) != c) {
                return;
            }
            state = child;
            const std::uint32_t opos = le32(state_at(state) + 8) >> 8U;
            if (opos != 0 && opos <= outputs_.size() / 12) {
                const auto* o = reinterpret_cast<const unsigned char*>(outputs_.data()) + (opos - 1) * 12;
                if (le32(o + 4) == depth + 1) {
                    f(depth + 1, le32(o));
                }
            }
        }
    }

private:
    const unsigned char* state_at(std::uint32_t i) const {
        return reinterpret_cast<const unsigned char*>(states_.data()) + static_cast<std::size_t>(i) * 12;
    }
    std::string_view states_;
    std::string_view outputs_;
};

/// A prefix dictionary: the system dictionary, or a user dictionary (a .bin's archive, or one built from a
/// CSV, looked up by surface).
struct PrefixDictionary {
    std::string da_bytes;
    DoubleArray da;
    std::unordered_map<std::string, std::uint32_t> by_surface;  // a CSV user dictionary
    std::size_t max_surface = 0;
    std::string vals;
    bool is_system = true;

    std::pair<std::uint32_t, std::uint32_t> decode_val(std::uint32_t v) const {
        return is_system ? std::make_pair(v >> 8U, v & 0xFFU) : std::make_pair(v >> 5U, v & 0x1FU);
    }

    /// Calls `f(end, entry)` for the entries of each word that starts `text`, as set_text collects them.
    template <typename F>
    void matches(std::string_view text, F&& f) const {
        const auto emit = [&](std::size_t len, std::uint32_t value) {
            const auto [offset, count] = decode_val(value);
            const std::size_t bytes = static_cast<std::size_t>(offset) * kWordEntryLen;
            if (bytes >= vals.size()) {
                return;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t at = bytes + kWordEntryLen * i;
                if (at + kWordEntryLen <= vals.size()) {
                    f(len, word_entry(reinterpret_cast<const unsigned char*>(vals.data()) + at));
                }
            }
        };
        if (by_surface.empty()) {
            da.prefixes(text, emit);
            return;
        }
        for (std::size_t len = 1; len <= std::min(max_surface, text.size()); ++len) {
            if (const auto it = by_surface.find(std::string(text.substr(0, len))); it != by_surface.end()) {
                emit(len, it->second);
            }
        }
    }
};

struct CategoryData {
    bool invoke = false;
    bool group = false;
};

struct Metadata {
    std::int16_t default_word_cost = 0;
    std::uint16_t default_left_context_id = 0;
    std::uint16_t default_right_context_id = 0;
    bool flexible_csv = false;
    std::size_t dictionary_fields = 0;
    std::size_t user_dictionary_fields = 0;
};

struct Dictionary {
    Metadata metadata;
    std::vector<CategoryData> categories;
    std::vector<std::string> category_names;
    std::vector<std::uint32_t> boundaries;
    std::vector<std::vector<std::uint32_t>> category_values;
    std::vector<std::vector<std::uint32_t>> unknown_refs;
    std::vector<WordEntry> unknown_costs;
    std::vector<std::int16_t> costs;
    std::uint32_t forward_size = 0;
    std::uint32_t backward_size = 0;
    PrefixDictionary prefix;

    const std::vector<std::uint32_t>& lookup_categories(char32_t c) const {
        const auto target = static_cast<std::uint32_t>(c);
        const auto it = std::upper_bound(boundaries.begin(), boundaries.end(), target);
        return category_values[static_cast<std::size_t>(it - boundaries.begin()) - 1];
    }

    std::int32_t cost(std::uint32_t forward_id, std::uint32_t backward_id) const {
        const std::size_t at = forward_id + static_cast<std::size_t>(backward_id) * forward_size;
        return at < costs.size() ? costs[at] : 0;
    }
};

bool load_metadata(const std::filesystem::path& dir, Metadata& m, std::string& error) {
    std::string text;
    if (!read_file(dir / "metadata.json", text, error)) {
        return false;
    }
    Value v;
    const auto num = [&](const char* key, double& out) {
        const Value* f = v.get(key);
        if (f == nullptr || f->kind != Value::Number) {
            return false;
        }
        out = f->n;
        return true;
    };
    const auto fields = [&](const char* key, std::size_t& out) {
        const Value* s = v.get(key);
        const Value* f = s != nullptr ? s->get("fields") : nullptr;
        if (f == nullptr || f->kind != Value::Array) {
            return false;
        }
        out = f->items.size();
        return true;
    };
    double cost = 0;
    double left = 0;
    double right = 0;
    const Value* flexible = nullptr;
    if (!json::parse(text, v) || v.kind != Value::Object || !num("default_word_cost", cost) ||
        !num("default_left_context_id", left) || !num("default_right_context_id", right) ||
        (flexible = v.get("flexible_csv")) == nullptr || flexible->kind != Value::Bool ||
        !fields("dictionary_schema", m.dictionary_fields) || !fields("user_dictionary_schema", m.user_dictionary_fields)) {
        error = "LinderaError(kind=Deserialize, source=Failed to deserialize metadata: " + (dir / "metadata.json").string() + ")";
        return false;
    }
    m.default_word_cost = static_cast<std::int16_t>(cost);
    m.default_left_context_id = static_cast<std::uint16_t>(left);
    m.default_right_context_id = static_cast<std::uint16_t>(right);
    m.flexible_csv = flexible->b;
    return true;
}

bool load_character_definition(const std::filesystem::path& dir, Dictionary& d, std::string& error) {
    std::string bytes;
    if (!read_file(dir / "char_def.bin", bytes, error)) {
        return false;
    }
    Archive a(bytes);
    // ArchivedCharacterDefinition: category_definitions, category_names, mapping { boundaries, values }.
    const std::size_t root = a.root(32);
    const auto [defs, n_defs] = a.vec(root, 8);
    for (std::size_t i = 0; i < n_defs && a.ok(); ++i) {
        d.categories.push_back({a.u8(defs + i * 8) != 0, a.u8(defs + i * 8 + 1) != 0});
    }
    const auto [names, n_names] = a.vec(root + 8, 8);
    for (std::size_t i = 0; i < n_names && a.ok(); ++i) {
        d.category_names.push_back(a.string(names + i * 8));
    }
    const auto [bounds, n_bounds] = a.vec(root + 16, 4);
    for (std::size_t i = 0; i < n_bounds && a.ok(); ++i) {
        d.boundaries.push_back(a.u32(bounds + i * 4));
    }
    const auto [values, n_values] = a.vec(root + 24, 8);
    for (std::size_t i = 0; i < n_values && a.ok(); ++i) {
        const auto [ids, n_ids] = a.vec(values + i * 8, 4);
        std::vector<std::uint32_t> row;
        for (std::size_t k = 0; k < n_ids && a.ok(); ++k) {
            row.push_back(a.u32(ids + k * 4));
        }
        d.category_values.push_back(std::move(row));
    }
    bool valid = a.ok() && n_bounds == n_values && !d.boundaries.empty() && d.boundaries[0] == 0 &&
                 std::is_sorted(d.boundaries.begin(), d.boundaries.end());
    for (const auto& row : d.category_values) {
        for (const auto id : row) {
            valid = valid && id < d.categories.size();
        }
    }
    if (!valid) {
        error = "LinderaError(kind=Deserialize, source=" + (dir / "char_def.bin").string() + " is not a character definition)";
        return false;
    }
    return true;
}

bool load_unknown_dictionary(const std::filesystem::path& dir, Dictionary& d, std::string& error) {
    std::string bytes;
    if (!read_file(dir / "unk.bin", bytes, error)) {
        return false;
    }
    Archive a(bytes);
    // ArchivedUnknownDictionary: category_references, costs (ArchivedWordEntry: id, is_system, lex_type,
    // word_cost, left_id, right_id; 16 bytes), words_idx_data, words_data.
    const std::size_t root = a.root(32);
    const auto [refs, n_refs] = a.vec(root, 8);
    for (std::size_t i = 0; i < n_refs && a.ok(); ++i) {
        const auto [ids, n_ids] = a.vec(refs + i * 8, 4);
        std::vector<std::uint32_t> row;
        for (std::size_t k = 0; k < n_ids && a.ok(); ++k) {
            row.push_back(a.u32(ids + k * 4));
        }
        d.unknown_refs.push_back(std::move(row));
    }
    const auto [costs, n_costs] = a.vec(root + 8, 16);
    for (std::size_t i = 0; i < n_costs && a.ok(); ++i) {
        WordEntry e;
        e.id = a.u32(costs + i * 16);
        e.cost = static_cast<std::int16_t>(a.u16(costs + i * 16 + 8));
        e.left_id = a.u16(costs + i * 16 + 10);
        e.right_id = a.u16(costs + i * 16 + 12);
        d.unknown_costs.push_back(e);
    }
    bool valid = a.ok() && d.unknown_refs.size() >= d.categories.size();
    for (const auto& row : d.unknown_refs) {
        for (const auto id : row) {
            valid = valid && id < d.unknown_costs.size();
        }
    }
    if (!valid) {
        error = "LinderaError(kind=Deserialize, source=" + (dir / "unk.bin").string() + " is not an unknown-word dictionary)";
        return false;
    }
    return true;
}

bool load_matrix(const std::filesystem::path& dir, Dictionary& d, std::string& error) {
    std::string bytes;
    if (!read_file(dir / "matrix.mtx", bytes, error)) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < 4) {
        error = "LinderaError(kind=Deserialize, source=Connection cost matrix data too short: " +
                std::to_string(bytes.size()) + " bytes)";
        return false;
    }
    const auto first = static_cast<std::int16_t>(le16(p));
    if (first == -1) {
        if (bytes.size() < 6) {
            error = "LinderaError(kind=Deserialize, source=Connection cost matrix header too short)";
            return false;
        }
        d.forward_size = static_cast<std::uint32_t>(static_cast<std::int16_t>(le16(p + 2)));
        d.backward_size = static_cast<std::uint32_t>(static_cast<std::int16_t>(le16(p + 4)));
        d.costs.resize(bytes.size() / 2 - 3);
        for (std::size_t i = 0; i < d.costs.size(); ++i) {
            d.costs[i] = static_cast<std::int16_t>(le16(p + 6 + i * 2));
        }
        return true;
    }
    // The old layout, transposed as Lindera transposes it.
    d.forward_size = static_cast<std::uint32_t>(first);
    d.backward_size = static_cast<std::uint32_t>(static_cast<std::int16_t>(le16(p + 2)));
    const std::size_t size = bytes.size() / 2 - 2;
    d.costs.assign(size, 0);
    for (std::uint32_t f = 0; f < d.forward_size; ++f) {
        for (std::uint32_t b = 0; b < d.backward_size; ++b) {
            const std::size_t old_id = b + static_cast<std::size_t>(f) * d.backward_size;
            const std::size_t new_id = f + static_cast<std::size_t>(b) * d.forward_size;
            if (old_id < size && new_id < size) {
                d.costs[new_id] = static_cast<std::int16_t>(le16(p + 4 + old_id * 2));
            }
        }
    }
    return true;
}

/// Dictionary::load_from_path, cached by its files.
std::shared_ptr<const Dictionary> load_dictionary(const std::filesystem::path& dir, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        error = "LinderaError(kind=Io, source=Dictionary path does not exist: " + dir.string() + ")";
        return nullptr;
    }
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "LinderaError(kind=Io, source=Dictionary path is not a directory: " + dir.string() + ")";
        return nullptr;
    }
    std::string key;
    for (const char* f : {"metadata.json", "char_def.bin", "matrix.mtx", "dict.da", "dict.vals", "unk.bin"}) {
        key += file_stamp(std::filesystem::absolute(dir, ec) / f) + "\n";
    }
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Dictionary>> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }
    }
    auto d = std::make_shared<Dictionary>();
    if (!load_metadata(dir, d->metadata, error) || !load_character_definition(dir, *d, error) ||
        !load_matrix(dir, *d, error) || !read_file(dir / "dict.da", d->prefix.da_bytes, error) ||
        !read_file(dir / "dict.vals", d->prefix.vals, error)) {
        return nullptr;
    }
    std::string unused;
    if (!read_file(dir / "dict.wordsidx", unused, error) || !read_file(dir / "dict.words", unused, error)) {
        return nullptr;
    }
    if (!d->prefix.da.load(d->prefix.da_bytes)) {
        error = "LinderaError(kind=Deserialize, source=" + (dir / "dict.da").string() + " is not a double array)";
        return nullptr;
    }
    if (!load_unknown_dictionary(dir, *d, error)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 4U) {
        cache.erase(cache.begin());
    }
    cache[key] = d;
    return d;
}

/// The Rust csv crate's reader as Lindera configures it: no headers, ',' and '"', empty lines skipped;
/// records of different lengths are an error unless the dictionary allows flexible CSV.
bool read_csv(std::string_view text, bool flexible, std::vector<std::vector<std::string>>& rows, std::string& error) {
    std::size_t i = 0;
    std::size_t first_len = 0;
    while (i < text.size()) {
        std::vector<std::string> row;
        std::string field;
        bool quoted = false;
        bool any = false;
        for (;;) {
            if (i >= text.size()) {
                break;
            }
            const char c = text[i];
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < text.size() && text[i + 1] == '"') {
                        field.push_back('"');
                        i += 2;
                        continue;
                    }
                    quoted = false;
                    ++i;
                    continue;
                }
                field.push_back(c);
                ++i;
                continue;
            }
            if (c == '"' && field.empty()) {
                quoted = true;
                any = true;
                ++i;
                continue;
            }
            if (c == ',') {
                row.push_back(std::move(field));
                field.clear();
                any = true;
                ++i;
                continue;
            }
            if (c == '\n' || c == '\r') {
                ++i;
                if (c == '\r' && i < text.size() && text[i] == '\n') {
                    ++i;
                }
                break;
            }
            field.push_back(c);
            any = true;
            ++i;
        }
        if (!any && field.empty() && row.empty()) {
            continue;  // an empty line
        }
        row.push_back(std::move(field));
        if (rows.empty()) {
            first_len = row.size();
        } else if (!flexible && row.size() != first_len) {
            error = "found record with " + std::to_string(row.size()) + " fields, but the previous record has " +
                    std::to_string(first_len) + " fields";
            return false;
        }
        rows.push_back(std::move(row));
    }
    return true;
}

template <typename T>
bool parse_int(const std::string& s, T& out) {
    if (s.empty()) {
        return false;
    }
    std::size_t k = s[0] == '+' || s[0] == '-' ? 1 : 0;
    if (k == s.size()) {
        return false;
    }
    long long v = 0;
    for (; k < s.size(); ++k) {
        if (s[k] < '0' || s[k] > '9') {
            return false;
        }
        v = v * 10 + (s[k] - '0');
        if (v > 100000) {
            return false;
        }
    }
    if (s[0] == '-') {
        v = -v;
    }
    if (v < std::numeric_limits<T>::min() || v > std::numeric_limits<T>::max()) {
        return false;
    }
    out = static_cast<T>(v);
    return true;
}

/// UserDictionaryBuilder::build: rows sorted by surface, entries grouped by surface in byte order.
std::shared_ptr<const PrefixDictionary> load_user_csv(const std::filesystem::path& path, const Metadata& m,
                                                      std::string& error) {
    std::string text;
    if (!read_file(path, text, error)) {
        error = "LinderaError(kind=Io, source=Failed to open user dictionary CSV file: " + path.string() + ")";
        return nullptr;
    }
    std::vector<std::vector<std::string>> rows;
    std::string why;
    if (!read_csv(text, m.flexible_csv, rows, why)) {
        error = "LinderaError(kind=Content, source=Failed to parse CSV record in file: " + path.string() + ": " + why + ")";
        return nullptr;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    std::map<std::string, std::vector<WordEntry>> by_surface;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        WordEntry e;
        e.id = static_cast<std::uint32_t>(r);
        if (row.size() == m.user_dictionary_fields) {
            e.cost = m.default_word_cost;
            e.left_id = m.default_left_context_id;
            e.right_id = m.default_right_context_id;
        } else if (row.size() < 4 || !parse_int(row[3], e.cost) || !parse_int(row[1], e.left_id) ||
                   !parse_int(row[2], e.right_id)) {
            error = "LinderaError(kind=Parse, source=failed to parse word cost or context ids at row " +
                    std::to_string(r + 1) + " (surface: '" + row[0] + "'))";
            return nullptr;
        }
        if (row.size() != m.user_dictionary_fields && row.size() < m.dictionary_fields) {
            error = "LinderaError(kind=Content, source=user dictionary should be a CSV with " +
                    std::to_string(m.user_dictionary_fields) + " or " + std::to_string(m.dictionary_fields) + "+ fields)";
            return nullptr;
        }
        by_surface[row[0]].push_back(e);
    }
    auto dict = std::make_shared<PrefixDictionary>();
    dict->is_system = false;
    std::uint32_t id = 0;
    for (const auto& [surface, entries] : by_surface) {
        if (surface.empty()) {
            error = "LinderaError(kind=Build, source=Failed to build DoubleArray for user dictionary)";
            return nullptr;
        }
        dict->by_surface[surface] = (id << 5U) | static_cast<std::uint32_t>(entries.size());
        dict->max_surface = std::max(dict->max_surface, surface.size());
        id += static_cast<std::uint32_t>(entries.size());
        for (const auto& e : entries) {
            const unsigned char raw[kWordEntryLen] = {
                static_cast<unsigned char>(e.id), static_cast<unsigned char>(e.id >> 8U),
                static_cast<unsigned char>(e.id >> 16U), static_cast<unsigned char>(e.id >> 24U),
                static_cast<unsigned char>(static_cast<std::uint16_t>(e.cost)),
                static_cast<unsigned char>(static_cast<std::uint16_t>(e.cost) >> 8U),
                static_cast<unsigned char>(e.left_id), static_cast<unsigned char>(e.left_id >> 8U),
                static_cast<unsigned char>(e.right_id), static_cast<unsigned char>(e.right_id >> 8U)};
            dict->vals.append(reinterpret_cast<const char*>(raw), kWordEntryLen);
        }
    }
    return dict;
}

/// UserDictionary::load: the rkyv archive of { dict: PrefixDictionary { da, vals_data, words_idx_data,
/// words_data, is_system } }.
std::shared_ptr<const PrefixDictionary> load_user_bin(const std::filesystem::path& path, std::string& error) {
    std::string bytes;
    if (!read_file(path, bytes, error)) {
        return nullptr;
    }
    Archive a(bytes);
    const std::size_t root = a.root(36);
    auto dict = std::make_shared<PrefixDictionary>();
    dict->da_bytes = std::string(a.bytes(root));
    dict->vals = std::string(a.bytes(root + 8));
    a.bytes(root + 16);
    a.bytes(root + 24);
    dict->is_system = a.u8(root + 32) != 0;
    if (!a.ok() || !dict->da.load(dict->da_bytes)) {
        error = "LinderaError(kind=Deserialize, source=" + path.string() + " is not a Lindera 3 user dictionary)";
        return nullptr;
    }
    return dict;
}

// ---------------------------------------------------------------------------------------------------
// The lattice (viterbi.rs).

struct Penalty {
    std::size_t kanji_threshold = 2;
    std::int32_t kanji_penalty = 3000;
    std::size_t other_threshold = 7;
    std::int32_t other_penalty = 1700;
};

struct Edge {
    WordEntry entry;
    std::int32_t path_cost = std::numeric_limits<std::int32_t>::max();
    std::uint16_t left_index = std::numeric_limits<std::uint16_t>::max();
    std::uint32_t start = 0;
    std::uint32_t stop = 0;
    bool kanji_only = false;
};

bool is_kanji(char32_t c) { return (c >= 0x4E00 && c <= 0x9FAF) || (c >= 0x3400 && c <= 0x4DBF); }

struct CharData {
    std::uint32_t byte_offset = 0;
    bool kanji = false;
    const std::vector<std::uint32_t>* categories = nullptr;
    std::uint32_t kanji_run = 0;
};

class Lattice {
public:
    Lattice(const Dictionary& d, const PrefixDictionary* user, const std::optional<Penalty>& decompose)
        : d_(d), user_(user), decompose_(decompose) {}

    /// Lattice::set_text and tokens_offset for one sentence: each token's start and word.
    void run(std::string_view text, std::vector<std::pair<std::size_t, WordEntry>>& offsets) {
        const std::size_t len = text.size();
        ends_at_.assign(len + 1, {});
        chars_.clear();
        for (std::size_t i = 0; i < len;) {
            std::size_t n = 0;
            const char32_t c = decode_utf8(text, i, n);
            chars_.push_back({static_cast<std::uint32_t>(i), is_kanji(c), &d_.lookup_categories(c), 0});
            i += n;
        }
        static const std::vector<std::uint32_t> kNone;
        chars_.push_back({static_cast<std::uint32_t>(len), false, &kNone, 0});
        for (std::size_t i = chars_.size() - 1; i-- > 0;) {
            chars_[i].kanji_run = chars_[i].kanji ? (chars_[i + 1].byte_offset - chars_[i].byte_offset) + chars_[i + 1].kanji_run : 0;
        }
        Edge bos;
        bos.path_cost = 0;
        bos.entry = WordEntry{};
        ends_at_[0].push_back(bos);

        std::optional<std::size_t> unknown_end;
        std::vector<std::pair<std::size_t, WordEntry>> found_here;
        for (std::size_t ci = 0; ci + 1 < chars_.size(); ++ci) {
            const std::size_t start = chars_[ci].byte_offset;
            if (ends_at_[start].empty()) {
                continue;
            }
            // matches_head: a linked list per start, newest first; the user dictionary was scanned last.
            found_here.clear();
            d_.prefix.matches(text.substr(start), [&](std::size_t n, const WordEntry& e) { found_here.emplace_back(n, e); });
            if (user_ != nullptr) {
                user_->matches(text.substr(start), [&](std::size_t n, const WordEntry& e) { found_here.emplace_back(n, e); });
            }
            const bool found = !found_here.empty();
            for (auto it = found_here.rbegin(); it != found_here.rend(); ++it) {
                Edge e;
                e.entry = it->second;
                e.start = static_cast<std::uint32_t>(start);
                e.stop = static_cast<std::uint32_t>(start + it->first);
                e.kanji_only = chars_[ci].kanji_run >= it->first;
                add_edge(e);
            }
            if (decompose_.has_value() || !unknown_end.has_value() || *unknown_end <= start) {
                const auto& cats = *chars_[ci].categories;
                for (std::size_t ord = 0; ord < cats.size(); ++ord) {
                    unknown_end = unknown_word(cats[ord], ord, unknown_end, start, ci, found);
                }
            }
        }
        if (!ends_at_[len].empty()) {
            std::int32_t best = std::numeric_limits<std::int32_t>::max();
            std::optional<std::uint16_t> best_left;
            for (std::size_t i = 0; i < ends_at_[len].size(); ++i) {
                const std::int32_t c = sat_add(ends_at_[len][i].path_cost, d_.cost(ends_at_[len][i].entry.right_id, 0));
                if (c < best) {
                    best = c;
                    best_left = static_cast<std::uint16_t>(i);
                }
            }
            if (best_left.has_value()) {
                Edge eos;
                eos.start = eos.stop = static_cast<std::uint32_t>(len);
                eos.entry = WordEntry{};
                eos.left_index = *best_left;
                eos.path_cost = best;
                ends_at_[len].push_back(eos);
            }
        }
        // tokens_offset.
        offsets.clear();
        std::size_t last = ends_at_.size() - 1;
        while (last > 0 && ends_at_[last].empty()) {
            --last;
        }
        if (ends_at_[last].empty()) {
            return;
        }
        const Edge* edge = &ends_at_[last].back();
        if (edge->left_index == std::numeric_limits<std::uint16_t>::max()) {
            return;
        }
        while (edge->left_index != std::numeric_limits<std::uint16_t>::max()) {
            offsets.emplace_back(edge->start, edge->entry);
            const auto& from = ends_at_[edge->start];
            if (edge->left_index >= from.size()) {
                break;
            }
            edge = &from[edge->left_index];
        }
        std::reverse(offsets.begin(), offsets.end());
        if (!offsets.empty()) {
            offsets.pop_back();  // EOS
        }
    }

private:
    std::optional<std::size_t> unknown_word(std::uint32_t category, std::size_t ord, std::optional<std::size_t> unknown_end,
                                            std::size_t start, std::size_t ci, bool found) {
        std::size_t n_chars = 0;
        const CategoryData& data = d_.categories[category];
        if (data.invoke || !found) {
            n_chars = 1;
            if (data.group) {
                for (std::size_t next = ci + 1; next + 1 < chars_.size(); ++next) {
                    const auto& cats = *chars_[next].categories;
                    if (ord < cats.size() && cats[ord] == category) {
                        ++n_chars;
                    } else {
                        break;
                    }
                }
            }
        }
        if (n_chars == 0) {
            return unknown_end;
        }
        const std::size_t byte_len = chars_[ci + n_chars].byte_offset - start;
        const bool kanji_only = chars_[ci].kanji_run >= byte_len;
        if (category < d_.unknown_refs.size()) {
            for (const auto id : d_.unknown_refs[category]) {
                Edge e;
                e.entry = d_.unknown_costs[id];
                e.start = static_cast<std::uint32_t>(start);
                e.stop = static_cast<std::uint32_t>(start + byte_len);
                e.kanji_only = kanji_only;
                add_edge(e);
            }
        }
        return start + byte_len;
    }

    std::int32_t penalty(const Edge& left) const {
        const Penalty& p = *decompose_;
        const std::size_t n_chars = (left.stop - left.start) / 3;  // Edge::num_chars
        if (n_chars <= p.kanji_threshold) {
            return 0;
        }
        if (left.kanji_only) {
            return static_cast<std::int32_t>(n_chars - p.kanji_threshold) * p.kanji_penalty;
        }
        if (n_chars > p.other_threshold) {
            return static_cast<std::int32_t>(n_chars - p.other_threshold) * p.other_penalty;
        }
        return 0;
    }

    void add_edge(Edge e) {
        const auto& left = ends_at_[e.start];
        if (left.empty()) {
            return;
        }
        std::int32_t best = std::numeric_limits<std::int32_t>::max();
        std::optional<std::uint16_t> best_left;
        for (std::size_t i = 0; i < left.size(); ++i) {
            std::int32_t c = sat_add(left[i].path_cost, d_.cost(left[i].entry.right_id, e.entry.left_id));
            if (decompose_.has_value()) {
                c = sat_add(c, penalty(left[i]));
            }
            if (c < best) {
                best = c;
                best_left = static_cast<std::uint16_t>(i);
            }
        }
        if (best_left.has_value()) {
            e.path_cost = sat_add(best, e.entry.cost);
            e.left_index = *best_left;
            ends_at_[e.stop].push_back(e);
        }
    }

    const Dictionary& d_;
    const PrefixDictionary* user_;
    std::optional<Penalty> decompose_;
    std::vector<std::vector<Edge>> ends_at_;
    std::vector<CharData> chars_;
};

/// A dictionary or user dictionary location: a path, or a file:// URI (percent-decoded).
bool resolve_uri(const std::string& uri, std::filesystem::path& out, bool user, std::string& error) {
    if (uri.find("://") == std::string::npos) {
        out = uri;
        return true;
    }
    const std::size_t colon = uri.find(':');
    const std::string scheme = uri.substr(0, colon);
    if (scheme != "file") {
        error = user ? "LinderaError(kind=Dictionary, source=Invalid dictionary scheme: " + scheme + ")"
                     : "LinderaError(kind=Dictionary, source=Invalid dictionary scheme: LinderaError(kind=Dictionary, "
                       "source=Invalid dictionary scheme: " + scheme + "))";
        return false;
    }
    std::string path = uri.substr(colon + 3);
    const std::size_t slash = path.find('/');
    path = slash == std::string::npos ? std::string() : path.substr(slash);  // file://host/path: the path
    std::string decoded;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) != 0 &&
            std::isxdigit(static_cast<unsigned char>(path[i + 2])) != 0) {
            decoded.push_back(static_cast<char>(std::strtol(path.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(path[i]);
        }
    }
    out = decoded;
    return true;
}

}  // namespace

struct Lindera::Impl {
    std::shared_ptr<const Dictionary> dictionary;
    std::shared_ptr<const PrefixDictionary> user;
    std::optional<Penalty> decompose;
    bool keep_whitespace = false;
    std::uint32_t space_category = 0;
    struct Mapping {
        std::map<std::string, std::string, std::less<>> to;
        std::size_t longest = 0;
    };
    std::vector<Mapping> mappings;  // mapping character filters, in order
};

namespace {

/// Tokenizer::from_config (with Segmenter::from_config): false and Lindera's error otherwise; `unsupported`
/// is set when the config is valid Lindera but asks for a filter nanolance does not implement.
bool build(const Value& config, Lindera::Impl& impl, std::string& error, bool& unsupported) {
    const Value* segmenter = config.get("segmenter");
    const Value* dict = segmenter != nullptr ? segmenter->get("dictionary") : nullptr;
    if (dict == nullptr || dict->kind != Value::String) {
        error = "LinderaError(kind=Parse, source=dictionary field is missing)";
        return false;
    }
    std::filesystem::path dict_path;
    if (!resolve_uri(dict->s, dict_path, false, error)) {
        return false;
    }
    impl.dictionary = load_dictionary(dict_path, error);
    if (impl.dictionary == nullptr) {
        return false;
    }
    if (const Value* u = segmenter->get("user_dictionary"); u != nullptr && u->kind == Value::String) {
        std::filesystem::path user_path;
        if (!resolve_uri(u->s, user_path, true, error)) {
            return false;
        }
        const std::string ext = user_path.has_extension() ? user_path.extension().string().substr(1) : std::string();
        if (ext.empty()) {
            error = "LinderaError(kind=Args, source=Invalid user dictionary source file)";
            return false;
        }
        if (ext == "csv") {
            impl.user = load_user_csv(user_path, impl.dictionary->metadata, error);
        } else if (ext == "bin") {
            impl.user = load_user_bin(user_path, error);
        } else {
            error = "LinderaError(kind=Args, source=Invalid user dictionary source file extension)";
            return false;
        }
        if (impl.user == nullptr) {
            return false;
        }
    }
    if (const Value* mode = segmenter->get("mode"); mode != nullptr) {
        if (mode->kind == Value::String) {
            if (mode->s == "decompose") {
                impl.decompose = Penalty{};
            } else if (mode->s != "normal") {
                error = "LinderaError(kind=Parse, source=mode field is invalid string: LinderaError(kind=Mode, "
                        "source=Invalid mode: " + mode->s + "))";
                return false;
            }
        } else {
            // serde's externally tagged Mode: {"decompose": {the four penalty fields}}.
            const Value* p = mode->kind == Value::Object && mode->fields.size() == 1 ? mode->get("decompose") : nullptr;
            const auto field = [&](const char* key, double& out) {
                const Value* f = p != nullptr ? p->get(key) : nullptr;
                if (f == nullptr || f->kind != Value::Number || f->n < 0 || f->n != static_cast<double>(static_cast<long long>(f->n))) {
                    return false;
                }
                out = f->n;
                return true;
            };
            double kt = 0;
            double kp = 0;
            double ot = 0;
            double op = 0;
            if (!field("kanji_penalty_length_threshold", kt) || !field("kanji_penalty_length_penalty", kp) ||
                !field("other_penalty_length_threshold", ot) || !field("other_penalty_length_penalty", op)) {
                error = "LinderaError(kind=Parse, source=mode field is invalid object)";
                return false;
            }
            impl.decompose = Penalty{static_cast<std::size_t>(kt), static_cast<std::int32_t>(kp),
                                     static_cast<std::size_t>(ot), static_cast<std::int32_t>(op)};
        }
    }
    if (const Value* k = segmenter->get("keep_whitespace"); k != nullptr && k->kind == Value::Bool) {
        impl.keep_whitespace = k->b;
    }
    if (!impl.keep_whitespace) {
        const auto& names = impl.dictionary->category_names;
        const auto it = std::find(names.begin(), names.end(), "SPACE");
        if (it == names.end()) {
            error = "LinderaError(kind=Parse, source=SPACE category is not defined in the dictionary (char.def))";
            return false;
        }
        impl.space_category = static_cast<std::uint32_t>(it - names.begin());
    }
    if (const Value* filters = config.get("character_filters"); filters != nullptr && filters->kind == Value::Array) {
        for (const auto& f : filters->items) {
            const Value* kind = f.get("kind");
            if (kind == nullptr || kind->kind != Value::String) {
                continue;
            }
            if (kind->s == "mapping") {
                const Value* args = f.get("args");
                const Value* mapping = args != nullptr ? args->get("mapping") : nullptr;
                if (mapping == nullptr || mapping->kind != Value::Object) {
                    error = "LinderaError(kind=Parse, source=mapping must be an object.)";
                    return false;
                }
                Lindera::Impl::Mapping m;
                for (const auto& [key, value] : mapping->fields) {
                    if (value.kind == Value::String) {
                        if (key.empty()) {
                            error = "LinderaError(kind=Build, source=an empty mapping key)";
                            return false;
                        }
                        m.to[key] = value.s;
                        m.longest = std::max(m.longest, key.size());
                    }
                }
                impl.mappings.push_back(std::move(m));
            } else if (kind->s == "japanese_iteration_mark" || kind->s == "regex" || kind->s == "unicode_normalize") {
                error = "the lindera character filter '" + kind->s + "' is not supported (mapping is)";
                unsupported = true;
                return false;
            } else {
                error = "LinderaError(kind=Deserialize, source=unsupported character filter: " + kind->s + ")";
                return false;
            }
        }
    }
    if (const Value* filters = config.get("token_filters"); filters != nullptr && filters->kind == Value::Array) {
        for (const auto& f : filters->items) {
            const Value* kind = f.get("kind");
            if (kind != nullptr && kind->kind == Value::String) {
                error = "the lindera token filter '" + kind->s + "' is not supported";
                unsupported = true;
                return false;
            }
        }
    }
    return true;
}

bool parse_config(const std::filesystem::path& path, Value& config, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "LinderaError { kind: Io, source: Failed to open tokenizer config file: " + path.string() +
                " Caused by: No such file or directory (os error 2) }";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string why;
    if (!Yaml().parse(ss.str(), config, why)) {
        error = "LinderaError { kind: Deserialize, source: Failed to parse YAML config file: " + path.string() + ": " + why + " }";
        return false;
    }
    if (config.kind != Value::Object) {
        error = "LinderaError { kind: Deserialize, source: Config file must contain a YAML mapping: " + path.string() + " }";
        return false;
    }
    return true;
}

}  // namespace

std::shared_ptr<const Lindera> Lindera::load(const std::filesystem::path& dir, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "Invalid user input: Invalid directory path: " + dir.string();
        return nullptr;
    }
    // LinderaBuilder::build: the model's config.yml, else TokenizerBuilder::new() (LINDERA_CONFIG_PATH, else
    // an empty config, which has no dictionary).
    const auto config_path = dir / "config.yml";
    const bool own = std::filesystem::exists(config_path, ec);
    Value config;
    std::string why;
    bool ok = true;
    if (own) {
        ok = parse_config(config_path, config, why);
    } else if (const char* env = std::getenv("LINDERA_CONFIG_PATH"); env != nullptr) {
        ok = parse_config(env, config, why);
    } else {
        config.kind = Value::Object;
    }
    auto impl = std::make_shared<Impl>();
    bool unsupported = false;
    if (ok && !build(config, *impl, why, unsupported)) {
        if (unsupported) {
            error = why;
            return nullptr;
        }
        why = "LinderaError { kind: Parse, source: failed to build tokenizer: " + why + " }";
        ok = false;
    }
    if (!ok) {
        error = own ? "IO error: Failed to load tokenizer config at " + config_path.string() + ": " + why
                    : "IO error: Failed to initialize default tokenizer: " + why;
        return nullptr;
    }
    auto lindera = std::make_shared<Lindera>();
    lindera->impl_ = impl;
    return lindera;
}

void Lindera::tokenize(std::string_view input, std::vector<LinderaToken>& out) const {
    const Impl& impl = *impl_;
    std::string text(input);
    for (const auto& mapping : impl.mappings) {
        // MappingCharacterFilter::apply: the longest key at each position, else one character.
        std::string filtered;
        filtered.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t best = 0;
            const std::string* replacement = nullptr;
            for (std::size_t n = std::min(mapping.longest, text.size() - i); n > 0 && replacement == nullptr; --n) {
                if (const auto it = mapping.to.find(std::string_view(text).substr(i, n)); it != mapping.to.end()) {
                    best = n;
                    replacement = &it->second;
                }
            }
            if (replacement != nullptr) {
                filtered += *replacement;
                i += best;
                continue;
            }
            std::size_t n = 0;
            decode_utf8(text, i, n);
            filtered.append(text, i, n);
            i += n;
        }
        text = std::move(filtered);
    }
    // Segmenter::segment_with_lattice: sentences end after '\n', '\t', '。' or '、'.
    const Dictionary& d = *impl.dictionary;
    Lattice lattice(d, impl.user.get(), impl.decompose);
    std::vector<std::pair<std::size_t, WordEntry>> offsets;
    std::uint32_t position = 0;
    std::size_t sentence_start = 0;
    const std::size_t len = text.size();
    while (sentence_start < len) {
        std::size_t sentence_end = sentence_start;
        while (sentence_end < len) {
            const char ch = text[sentence_end];
            ++sentence_end;
            if (ch == '\n' || ch == '\t') {
                break;
            }
            if (sentence_end >= 3 && (text.compare(sentence_end - 3, 3, "\xE3\x80\x82") == 0 ||
                                      text.compare(sentence_end - 3, 3, "\xE3\x80\x81") == 0)) {
                break;
            }
        }
        const std::string_view sentence = std::string_view(text).substr(sentence_start, sentence_end - sentence_start);
        lattice.run(sentence, offsets);
        for (std::size_t k = 0; k < offsets.size(); ++k) {
            const std::size_t from = offsets[k].first;
            const std::size_t to = k + 1 == offsets.size() ? sentence.size() : offsets[k + 1].first;
            if (to < from) {
                continue;
            }
            const std::string_view surface = sentence.substr(from, to - from);
            if (!impl.keep_whitespace) {
                bool space = true;
                for (std::size_t i = 0; i < surface.size() && space;) {
                    std::size_t n = 0;
                    const char32_t c = decode_utf8(surface, i, n);
                    const auto& cats = d.lookup_categories(c);
                    space = std::find(cats.begin(), cats.end(), impl.space_category) != cats.end();
                    i += n;
                }
                if (space) {
                    continue;
                }
            }
            out.push_back({std::string(surface), position++});
        }
        sentence_start = sentence_end;
    }
}

}  // namespace nano_lance::fts
