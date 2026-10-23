// Snowball's runtime, after frostem's snowball_env.rs (BSD-3-Clause): a UTF-8 string with byte
// cursors, as the generated stemmers (fts_english_stem.inc) expect it.
struct Among {
    const char* s;
    int substring_i;
    int result;
};

struct SnowballEnv {
    std::string current;
    int cursor = 0;
    int limit = 0;
    int limit_backward = 0;
    int bra = 0;
    int ket = 0;

    explicit SnowballEnv(std::string_view value) : current(value), limit(static_cast<int>(value.size())) {}

    int byte(int i) const { return static_cast<unsigned char>(current[static_cast<std::size_t>(i)]); }
    bool is_char_boundary(int i) const {
        return i <= 0 || i >= static_cast<int>(current.size()) || (byte(i) & 0xC0) != 0x80;
    }
    /// The code point starting at byte `i`.
    std::uint32_t char_at(int i) const {
        const auto* p = reinterpret_cast<const unsigned char*>(current.data()) + i;
        if (p[0] < 0x80) return p[0];
        if (p[0] < 0xE0) return ((p[0] & 0x1FU) << 6) | (p[1] & 0x3FU);
        if (p[0] < 0xF0) return ((p[0] & 0x0FU) << 12) | ((p[1] & 0x3FU) << 6) | (p[2] & 0x3FU);
        return ((p[0] & 0x07U) << 18) | ((p[1] & 0x3FU) << 12) | ((p[2] & 0x3FU) << 6) | (p[3] & 0x3FU);
    }
    int replace_s(int b, int k, std::string_view s) {
        const int adjustment = static_cast<int>(s.size()) - (k - b);
        current.replace(static_cast<std::size_t>(b), static_cast<std::size_t>(k - b), s);
        limit += adjustment;
        if (cursor >= k) {
            cursor += adjustment;
        } else if (cursor > b) {
            cursor = b;
        }
        return adjustment;
    }
    bool eq_s(std::string_view s) {
        if (cursor >= limit) return false;
        if (current.compare(static_cast<std::size_t>(cursor), s.size(), s) == 0 &&
            static_cast<std::size_t>(cursor) + s.size() <= current.size()) {
            cursor += static_cast<int>(s.size());
            while (!is_char_boundary(cursor)) ++cursor;
            return true;
        }
        return false;
    }
    bool eq_s_b(std::string_view s) {
        const int n = static_cast<int>(s.size());
        if (cursor - limit_backward < n) return false;
        if (!is_char_boundary(cursor - n) || current.compare(static_cast<std::size_t>(cursor - n), s.size(), s) != 0) {
            return false;
        }
        cursor -= n;
        return true;
    }
    void slice_from(std::string_view s) {
        const int b = bra;
        replace_s(bra, ket, s);
        ket = b + static_cast<int>(s.size());
    }
    void slice_del() { slice_from(""); }
    void next_char() {
        ++cursor;
        while (!is_char_boundary(cursor)) ++cursor;
    }
    void previous_char() {
        --cursor;
        while (!is_char_boundary(cursor)) --cursor;
    }
    bool hop(int delta) {
        int res = cursor;
        while (delta > 0) {
            --delta;
            if (res >= limit) return false;
            ++res;
            while (res < limit && !is_char_boundary(res)) ++res;
        }
        cursor = res;
        return true;
    }
    bool hop_back(int delta) {
        int res = cursor;
        while (delta > 0) {
            --delta;
            if (res <= limit_backward) return false;
            --res;
            while (res > limit_backward && !is_char_boundary(res)) --res;
        }
        cursor = res;
        return true;
    }
    static bool in_set(const unsigned char* chars, std::uint32_t ch, std::uint32_t min, std::uint32_t max) {
        if (ch > max || ch < min) return false;
        ch -= min;
        return (chars[ch >> 3U] & (1U << (ch & 7U))) != 0;
    }
    bool in_grouping(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        if (cursor >= limit) return false;
        if (!in_set(chars, char_at(cursor), min, max)) return false;
        next_char();
        return true;
    }
    bool go_in_grouping(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        while (cursor < limit) {
            if (!in_set(chars, char_at(cursor), min, max)) return true;
            next_char();
        }
        return false;
    }
    bool in_grouping_b(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        if (cursor <= limit_backward) return false;
        const int c = cursor;
        previous_char();
        if (!in_set(chars, char_at(cursor), min, max)) {
            cursor = c;
            return false;
        }
        return true;
    }
    bool go_in_grouping_b(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        while (cursor > limit_backward) {
            const int c = cursor;
            previous_char();
            if (!in_set(chars, char_at(cursor), min, max)) {
                cursor = c;
                return true;
            }
        }
        return false;
    }
    bool out_grouping(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        if (cursor >= limit) return false;
        if (!in_set(chars, char_at(cursor), min, max)) {
            next_char();
            return true;
        }
        return false;
    }
    bool go_out_grouping(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        while (cursor < limit) {
            if (in_set(chars, char_at(cursor), min, max)) return true;
            next_char();
        }
        return false;
    }
    bool out_grouping_b(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        if (cursor <= limit_backward) return false;
        const int c = cursor;
        previous_char();
        if (!in_set(chars, char_at(cursor), min, max)) return true;
        cursor = c;
        return false;
    }
    bool go_out_grouping_b(const unsigned char* chars, std::uint32_t min, std::uint32_t max) {
        while (cursor > limit_backward) {
            const int c = cursor;
            previous_char();
            if (in_set(chars, char_at(cursor), min, max)) {
                cursor = c;
                return true;
            }
        }
        return false;
    }
    int find_among(const Among* amongs, std::size_t n) {
        int i = 0;
        int j = static_cast<int>(n);
        const int c = cursor;
        const int l = limit;
        int common_i = 0;
        int common_j = 0;
        bool first_key_inspected = false;
        for (;;) {
            const int k = i + ((j - i) >> 1);
            int diff = 0;
            int common = std::min(common_i, common_j);
            const Among& w = amongs[k];
            const int wl = static_cast<int>(std::strlen(w.s));
            for (int lvar = common; lvar < wl; ++lvar) {
                if (c + common == l) {
                    diff = -1;
                    break;
                }
                diff = byte(c + common) - static_cast<unsigned char>(w.s[lvar]);
                if (diff != 0) break;
                ++common;
            }
            if (diff < 0) {
                j = k;
                common_j = common;
            } else {
                i = k;
                common_i = common;
            }
            if (j - i <= 1) {
                if (i > 0 || j == i || first_key_inspected) break;
                first_key_inspected = true;
            }
        }
        for (;;) {
            const Among& w = amongs[i];
            const int wl = static_cast<int>(std::strlen(w.s));
            if (common_i >= wl) {
                cursor = c + wl;
                return w.result;
            }
            i = w.substring_i;
            if (i < 0) return 0;
        }
    }
    int find_among_b(const Among* amongs, std::size_t n) {
        int i = 0;
        int j = static_cast<int>(n);
        const int c = cursor;
        const int lb = limit_backward;
        int common_i = 0;
        int common_j = 0;
        bool first_key_inspected = false;
        for (;;) {
            const int k = i + ((j - i) >> 1);
            int diff = 0;
            int common = common_i < common_j ? common_i : common_j;
            const Among& w = amongs[k];
            const int wl = static_cast<int>(std::strlen(w.s));
            for (int lvar = wl - common - 1; lvar >= 0; --lvar) {
                if (c - common == lb) {
                    diff = -1;
                    break;
                }
                diff = byte(c - common - 1) - static_cast<unsigned char>(w.s[lvar]);
                if (diff != 0) break;
                ++common;
            }
            if (diff < 0) {
                j = k;
                common_j = common;
            } else {
                i = k;
                common_i = common;
            }
            if (j - i <= 1) {
                if (i > 0 || j == i || first_key_inspected) break;
                first_key_inspected = true;
            }
        }
        for (;;) {
            const Among& w = amongs[i];
            const int wl = static_cast<int>(std::strlen(w.s));
            if (common_i >= wl) {
                cursor = c - wl;
                return w.result;
            }
            i = w.substring_i;
            if (i < 0) return 0;
        }
    }
};
