// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// jieba-rs 0.10 (MIT, https://github.com/messense/jieba-rs) as Lance 12 runs it: dictionary loading
// (lib.rs load_dict), the DAG and route (dag, calc), cut_dag_hmm, the HMM (hmm.rs, with the built-in
// model jieba-macros 0.10 compiles in), cut_internal and cut_for_search. Comparisons and float arithmetic
// follow the Rust source step by step so the tokens are the same: Rust's max_by keeps a later element
// unless the one kept is greater, and tuples compare their second field on a tie.

#include "nanolance/fts_jieba.hpp"

#include "base/fts_json.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>

#if defined(NANOLANCE_WITH_JIEBA)
#include <zstd.h>
#endif

namespace nano_lance::fts {
namespace {

/// One UTF-8 character at `i` (the text is valid UTF-8, as Arrow strings are): its code point and length.
char32_t decode_utf8(std::string_view s, std::size_t i, std::size_t& len) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80U || i + 1 >= s.size()) {
        len = 1;
        return b;
    }
    const auto cont = [&](std::size_t k) { return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3FU); };
    if ((b & 0xE0U) == 0xC0U) {
        len = 2;
        return (static_cast<char32_t>(b & 0x1FU) << 6U) | cont(1);
    }
    if ((b & 0xF0U) == 0xE0U && i + 2 < s.size()) {
        len = 3;
        return (static_cast<char32_t>(b & 0x0FU) << 12U) | (cont(1) << 6U) | cont(2);
    }
    if (i + 3 < s.size()) {
        len = 4;
        return (static_cast<char32_t>(b & 0x07U) << 18U) | (cont(1) << 12U) | (cont(2) << 6U) | cont(3);
    }
    len = 1;
    return b;
}

std::size_t char_count(std::string_view s) {
    std::size_t n = 0;
    for (const char c : s) {
        n += static_cast<signed char>(c) >= -0x40 ? 1U : 0U;  // a leading byte, as make_token_incremental counts
    }
    return n;
}

bool is_ascii_alnum(char32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// lib.rs is_cjk.
bool is_cjk(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x2A6DF) || (c >= 0x2A700 && c <= 0x2B73F) || (c >= 0x2B740 && c <= 0x2B81F) ||
           (c >= 0x2B820 && c <= 0x2CEAF) || (c >= 0x2CEB0 && c <= 0x2EBEF) || (c >= 0x2F800 && c <= 0x2FA1F);
}

/// RE_HAN_DEFAULT: CJK, ASCII alphanumerics and +#&._%-.
bool is_han_default(char32_t c) {
    return is_cjk(c) || is_ascii_alnum(c) || c == '+' || c == '#' || c == '&' || c == '.' || c == '_' || c == '%' ||
           c == '-';
}

/// hmm.rs is_hmm_han.
bool is_hmm_han(char32_t c) { return c >= 0x4E00 && c <= 0x9FD5; }

/// Unicode White_Space (str::split_whitespace).
bool is_unicode_whitespace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/// SplitByCharacterClass: maximal runs where `classify` holds (matched) and the stretches between them.
template <class F, class G>
void split_by_class(std::string_view text, F&& classify, G&& on_run) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t len = 0;
        const bool matched = classify(decode_utf8(text, pos, len));
        std::size_t end = pos + len;
        while (end < text.size()) {
            const bool same = classify(decode_utf8(text, end, len)) == matched;
            if (!same) {
                break;
            }
            end += len;
        }
        on_run(text.substr(pos, end - pos), matched);
        pos = end;
    }
}

// ── the HMM ─────────────────────────────────────────────────────────────────────────────────────

constexpr double kMinFloat = -3.14e100;
enum State : int { kBegin = 0, kEnd = 1, kMiddle = 2, kSingle = 3 };  // jieba's discriminants (and Ord)
constexpr std::array<std::array<State, 2>, 4> kAllowedPrev = {{
    {kEnd, kSingle},     // into Begin
    {kBegin, kMiddle},   // into End
    {kMiddle, kBegin},   // into Middle
    {kSingle, kEnd},     // into Single
}};

struct HmmModel {
    std::array<double, 4> initial{};
    std::array<std::array<double, 4>, 4> trans{};
    std::array<std::unordered_map<char32_t, double>, 4> emit;
    double emit_prob(int state, char32_t c) const {
        const auto it = emit[static_cast<std::size_t>(state)].find(c);
        return it == emit[static_cast<std::size_t>(state)].end() ? kMinFloat : it->second;
    }
};

#if defined(NANOLANCE_WITH_JIEBA)
#include "index/fts/fts_jieba_hmm_data.inc"

/// The built-in model, from tools/fts_jieba_hmm.py's packing of jieba-macros 0.10's hmm.model.
const HmmModel* builtin_hmm() {
    static const HmmModel* model = [] {
        auto m = std::make_unique<HmmModel>();
        std::vector<std::uint8_t> raw(kJiebaHmmRawSize);
        if (ZSTD_decompress(raw.data(), raw.size(), kJiebaHmmZstd, kJiebaHmmZstdSize) != kJiebaHmmRawSize) {
            return static_cast<const HmmModel*>(nullptr);
        }
        std::size_t at = 0;
        const auto u32 = [&] {
            std::uint32_t v = 0;
            std::memcpy(&v, raw.data() + at, 4);
            at += 4;
            return v;
        };
        const auto f64 = [&] {
            double v = 0;
            std::memcpy(&v, raw.data() + at, 8);
            at += 8;
            return v;
        };
        if (u32() != 1U) {
            return static_cast<const HmmModel*>(nullptr);
        }
        for (auto& p : m->initial) {
            p = f64();
        }
        for (auto& row : m->trans) {
            for (auto& p : row) {
                p = f64();
            }
        }
        for (auto& table : m->emit) {
            const auto n = u32();
            table.reserve(n);
            for (std::uint32_t k = 0; k < n; ++k) {
                const char32_t cp = u32();
                table[cp] = f64();
            }
        }
        return static_cast<const HmmModel*>(m.release());
    }();
    return model;
}
#else
const HmmModel* builtin_hmm() { return nullptr; }
#endif

/// hmm.rs viterbi + cut_internal over a run of two or more HMM Han characters.
void hmm_cut_han(std::string_view sentence, const HmmModel& hmm, std::vector<std::string_view>& words) {
    std::vector<std::pair<std::size_t, char32_t>> chars;
    for (std::size_t i = 0, len = 0; i < sentence.size(); i += len) {
        chars.emplace_back(i, decode_utf8(sentence, i, len));
    }
    const std::size_t n = chars.size();
    constexpr std::array<State, 4> kStates = {kBegin, kMiddle, kEnd, kSingle};
    std::vector<std::array<double, 4>> v(n);
    std::vector<std::array<int, 4>> prev(n, {-1, -1, -1, -1});
    for (const State y : kStates) {
        v[0][y] = hmm.initial[y] + hmm.emit_prob(y, chars[0].second);
    }
    for (std::size_t t = 1; t < n; ++t) {
        for (const State y : kStates) {
            const double em = hmm.emit_prob(y, chars[t].second);
            // max_by over (prob, y0): a later candidate replaces the kept one unless the kept one is greater.
            double best_p = 0.0;
            State best_s = kBegin;
            bool first = true;
            for (const State y0 : kAllowedPrev[y]) {
                const double p = v[t - 1][y0] + hmm.trans[y0][y] + em;
                const bool kept_greater = !first && (best_p > p || (best_p == p && best_s > y0));
                if (first || !kept_greater) {
                    best_p = p;
                    best_s = y0;
                    first = false;
                }
            }
            v[t][y] = best_p;
            prev[t][y] = best_s;
        }
    }
    double final_p = v[n - 1][kEnd];
    State state = kEnd;
    {
        const double p = v[n - 1][kSingle];
        const bool kept_greater = final_p > p || (final_p == p && kEnd > kSingle);
        if (!kept_greater) {
            final_p = p;
            state = kSingle;
        }
    }
    std::vector<State> path(n, kBegin);
    std::size_t t = n - 1;
    path[t] = state;
    State curr = state;
    while (t > 0) {
        const auto p = static_cast<State>(prev[t][curr]);
        path[t - 1] = p;
        curr = p;
        --t;
    }
    std::size_t begin = 0;
    std::size_t next = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = chars[i].first;
        const std::size_t end = i + 1 < n ? chars[i + 1].first : sentence.size();
        switch (path[i]) {
            case kBegin: begin = at; break;
            case kEnd:
                words.push_back(sentence.substr(begin, end - begin));
                next = end;
                break;
            case kSingle:
                words.push_back(sentence.substr(at, end - at));
                next = end;
                break;
            case kMiddle: break;
        }
    }
    if (next < sentence.size()) {
        words.push_back(sentence.substr(next));
    }
}

/// RE_SKIP `([a-zA-Z0-9]+(?:.\d+)?%?)`, leftmost-first: the match starting at or after `from`, as
/// [start, end); false when there is none. `.` is any character but a newline; `\d` is a Unicode digit,
/// and the text here holds no digits but ASCII ones (it is a run of RE_HAN_DEFAULT characters).
bool find_skip(std::string_view s, std::size_t from, std::size_t& start, std::size_t& end) {
    std::size_t i = from;
    while (i < s.size() && !is_ascii_alnum(static_cast<unsigned char>(s[i]))) {
        std::size_t len = 0;
        decode_utf8(s, i, len);
        i += len;
    }
    if (i >= s.size()) {
        return false;
    }
    start = i;
    while (i < s.size() && is_ascii_alnum(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    if (i < s.size() && s[i] != '\n') {
        std::size_t len = 0;
        decode_utf8(s, i, len);
        std::size_t j = i + len;
        if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                ++j;
            }
            i = j;
        }
    }
    if (i < s.size() && s[i] == '%') {
        ++i;
    }
    end = i;
    return true;
}

/// hmm.rs cut_with_allocated_memory: HMM Han runs through Viterbi, the rest split by RE_SKIP.
void hmm_cut(std::string_view word, const HmmModel& hmm, std::vector<std::string_view>& words) {
    split_by_class(word, is_hmm_han, [&](std::string_view block, bool han) {
        if (han) {
            if (char_count(block) > 1U) {
                hmm_cut_han(block, hmm, words);
            } else {
                words.push_back(block);
            }
            return;
        }
        std::size_t last = 0;
        std::size_t start = 0;
        std::size_t end = 0;
        while (find_skip(block, last, start, end)) {
            if (start > last) {
                words.push_back(block.substr(last, start - last));
            }
            if (end > start) {
                words.push_back(block.substr(start, end - start));
            }
            last = end;
        }
        if (last < block.size()) {
            words.push_back(block.substr(last));
        }
    });
}

/// usize::from_str: digits, an optional leading '+'; false on anything else or an overflow.
bool parse_usize(std::string_view s, std::uint64_t& out) {
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10U) {
            return false;
        }
        v = v * 10U + d;
    }
    out = v;
    return true;
}

bool read_file(const std::filesystem::path& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "IO error: " + path.string() + ": No such file or directory (os error 2)";
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
    const auto time = std::filesystem::last_write_time(p, ec);
    return p.string() + "|" + std::to_string(ec ? 0U : size) + "|" +
           std::to_string(ec ? 0 : static_cast<long long>(time.time_since_epoch().count()));
}

}  // namespace

const Jieba::Record* Jieba::find(std::string_view word) const {
    const auto it = ids_.find(std::string(word));
    return it == ids_.end() ? nullptr : &records_[it->second];
}

bool Jieba::load_dict(std::string_view text, std::string& error) {
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl + 1 - pos);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        ++line_no;
        // split_whitespace: the first field is the word, the second its frequency.
        std::vector<std::string_view> fields;
        std::size_t i = 0;
        while (i < line.size() && fields.size() < 2U) {
            std::size_t len = 0;
            while (i < line.size() && is_unicode_whitespace(decode_utf8(line, i, len))) {
                i += len;
            }
            if (i >= line.size()) {
                break;
            }
            const std::size_t start = i;
            while (i < line.size() && !is_unicode_whitespace(decode_utf8(line, i, len))) {
                i += len;
            }
            fields.push_back(line.substr(start, i - start));
        }
        if (fields.empty()) {
            continue;
        }
        std::uint64_t freq = 0;
        if (fields.size() > 1U && !parse_usize(fields[1], freq)) {
            error = "line " + std::to_string(line_no) + " `" + std::string(line) + "` frequency " +
                    std::string(fields[1]) + " is not a valid integer: invalid digit found in string";
            return false;
        }
        const auto log_freq = std::log(static_cast<double>(freq));
        const std::string word(fields[0]);
        const auto it = ids_.find(word);
        if (it != ids_.end()) {
            records_[it->second] = Record{freq, log_freq};
        } else {
            ids_.emplace(word, static_cast<std::uint32_t>(records_.size()));
            records_.push_back(Record{freq, log_freq});
            max_word_chars_ = std::max(max_word_chars_, char_count(word));
        }
    }
    total_ = 0;
    for (const auto& r : records_) {
        total_ += r.freq;
    }
    return true;
}

void Jieba::cut_block(std::string_view sentence, std::vector<std::string_view>& words) const {
    // The DAG (every dictionary word starting at each character) and the route (lib.rs calc).
    std::vector<std::size_t> starts;
    for (std::size_t i = 0, len = 0; i < sentence.size(); i += len) {
        decode_utf8(sentence, i, len);
        starts.push_back(i);
    }
    const std::size_t n = sentence.size();
    std::vector<std::pair<double, std::size_t>> route(n + 1, {0.0, 0});
    const double logtotal = std::log(static_cast<double>(total_));
    const double log1 = 0.0 - logtotal;
    std::size_t prev_byte_start = n;
    for (std::size_t k = starts.size(); k-- > 0;) {
        const std::size_t byte_start = starts[k];
        bool any = false;
        std::pair<double, std::size_t> best{0.0, 0};
        // Prefixes in increasing length, as cedar's common_prefix_iter yields them.
        std::size_t end_k = k;
        for (std::size_t chars = 1; chars <= max_word_chars_ && end_k < starts.size(); ++chars) {
            ++end_k;
            const std::size_t byte_end = end_k < starts.size() ? starts[end_k] : n;
            const Record* r = find(sentence.substr(byte_start, byte_end - byte_start));
            if (r == nullptr) {
                continue;
            }
            const std::pair<double, std::size_t> cand{r->log_freq - logtotal + route[byte_end].first, byte_end};
            const bool kept_greater =
                any && (best.first > cand.first || (best.first == cand.first && best.second > cand.second));
            if (!any || !kept_greater) {
                best = cand;
                any = true;
            }
        }
        route[byte_start] = any ? best : std::make_pair(log1 + route[prev_byte_start].first, prev_byte_start);
        prev_byte_start = byte_start;
    }
    // cut_dag_hmm: runs of single characters are joined and, unless the run is a dictionary word, cut by
    // the HMM (a dictionary word is split into its characters).
    const HmmModel* hmm = builtin_hmm();
    const auto flush = [&](std::string_view word) {
        if (char_count(word) == 1U) {
            words.push_back(word);
        } else if (find(word) == nullptr) {
            hmm_cut(word, *hmm, words);
        } else {
            for (std::size_t i = 0, len = 0; i < word.size(); i += len) {
                decode_utf8(word, i, len);
                words.push_back(word.substr(i, len));
            }
        }
    };
    std::size_t x = 0;
    std::optional<std::size_t> left;
    while (x < n) {
        const std::size_t y = route[x].second;
        if (char_count(sentence.substr(x, y - x)) == 1U) {
            if (!left) {
                left = x;
            }
        } else {
            if (left) {
                flush(sentence.substr(*left, x - *left));
                left.reset();
            }
            words.push_back(sentence.substr(x, y - x));
        }
        x = y;
    }
    if (left) {
        flush(sentence.substr(*left));
    }
}

void Jieba::tokenize_search(std::string_view text, std::vector<JiebaToken>& out) const {
    // cut_internal (hmm = true): RE_HAN_DEFAULT runs are cut; every other character is a token alone
    // ("\r\n" together).
    std::vector<JiebaToken> tokens;
    std::uint32_t offset = 0;
    std::vector<std::string_view> words;
    const auto push = [&](std::string_view word) {
        JiebaToken t;
        t.byte_start = static_cast<std::size_t>(word.data() - text.data());
        t.byte_end = t.byte_start + word.size();
        t.char_start = offset;
        offset += static_cast<std::uint32_t>(char_count(word));
        t.char_end = offset;
        tokens.push_back(t);
    };
    split_by_class(text, is_han_default, [&](std::string_view block, bool matched) {
        if (matched) {
            words.clear();
            cut_block(block, words);
            for (const auto w : words) {
                push(w);
            }
            return;
        }
        for (std::size_t i = 0, len = 0; i < block.size(); i += len) {
            decode_utf8(block, i, len);
            if (block[i] == '\r' && i + 1 < block.size() && block[i + 1] == '\n') {
                len = 2;
            }
            push(block.substr(i, len));
        }
    });
    // cut_for_search: before each word of more than two characters, its 2-grams in the dictionary; of
    // more than three, its 3-grams.
    for (const auto& token : tokens) {
        const std::string_view word = text.substr(token.byte_start, token.byte_end - token.byte_start);
        std::vector<std::size_t> idx;
        for (std::size_t i = 0, len = 0; i < word.size(); i += len) {
            decode_utf8(word, i, len);
            idx.push_back(i);
        }
        const std::size_t count = idx.size();
        const auto gram = [&](std::size_t i, std::size_t width) {
            const std::size_t from = idx[i];
            const std::size_t to = i + width < count ? idx[i + width] : word.size();
            if (find(word.substr(from, to - from)) != nullptr) {
                JiebaToken g;
                g.byte_start = token.byte_start + from;
                g.byte_end = token.byte_start + to;
                g.char_start = token.char_start + static_cast<std::uint32_t>(i);
                g.char_end = g.char_start + static_cast<std::uint32_t>(width);
                out.push_back(g);
            }
        };
        if (count > 2U) {
            for (std::size_t i = 0; i + 1 < count; ++i) {
                gram(i, 2);
            }
        }
        if (count > 3U) {
            for (std::size_t i = 0; i + 2 < count; ++i) {
                gram(i, 3);
            }
        }
        out.push_back(token);
    }
}

std::shared_ptr<const Jieba> Jieba::load(const std::filesystem::path& dir, std::string& error) {
    if (!jieba_available()) {
        error = "the 'jieba' tokenizer is not built into this nanolance";
        return nullptr;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        error = "Invalid user input: Invalid directory path: " + dir.string();
        return nullptr;
    }
    // config.json: {"main": <path>, "users": [<path>, ...]}, both relative to the model directory.
    std::filesystem::path main = dir / "dict.txt";
    std::vector<std::filesystem::path> users;
    const auto config_path = dir / "config.json";
    if (std::filesystem::exists(config_path, ec)) {
        std::string text;
        if (!read_file(config_path, text, error)) {
            return nullptr;
        }
        json::Value config;
        if (!json::parse(text, config) || config.kind != json::Value::Object) {
            error = "Invalid user input: " + config_path.string() + " is not a jieba model configuration";
            return nullptr;
        }
        if (const auto* m = config.get("main"); m != nullptr && m->kind == json::Value::String) {
            main = dir / m->s;
        }
        if (const auto* u = config.get("users"); u != nullptr && u->kind == json::Value::Array) {
            for (const auto& item : u->items) {
                if (item.kind == json::Value::String) {
                    users.push_back(dir / item.s);
                }
            }
        }
    }
    // Cached by the files' paths, sizes and times: a model is loaded once while it does not change.
    std::string key = file_stamp(main);
    for (const auto& u : users) {
        key += "\n" + file_stamp(u);
    }
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const Jieba>> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }
    }
    auto jieba = std::make_shared<Jieba>();
    std::string text;
    if (!read_file(main, text, error)) {
        return nullptr;
    }
    std::string why;
    if (!jieba->load_dict(text, why)) {
        error = "Invalid user input: Failed to load Jieba dictionary from " + main.string() + ": " + why;
        return nullptr;
    }
    for (const auto& u : users) {
        if (!read_file(u, text, error)) {
            return nullptr;
        }
        if (!jieba->load_dict(text, why)) {
            error = "Invalid user input: Failed to load Jieba user dictionary from " + u.string() + ": " + why;
            return nullptr;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 8U) {
        cache.erase(cache.begin());
    }
    cache[key] = jieba;
    return jieba;
}

std::filesystem::path language_model_home() {
    if (const char* home = std::getenv("LANCE_LANGUAGE_MODEL_HOME"); home != nullptr) {
        return std::filesystem::path(home);
    }
    // dirs::data_local_dir()
    std::filesystem::path base;
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA"); p != nullptr && *p != '\0') {
        base = p;
    }
#elif defined(__APPLE__)
    if (const char* p = std::getenv("HOME"); p != nullptr && *p != '\0') {
        base = std::filesystem::path(p) / "Library" / "Application Support";
    }
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p != nullptr && *p == '/') {
        base = p;
    } else if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') {
        base = std::filesystem::path(h) / ".local" / "share";
    }
#endif
    return base.empty() ? base : base / "lance" / "language_models";
}

bool jieba_available() { return builtin_hmm() != nullptr; }

}  // namespace nano_lance::fts
