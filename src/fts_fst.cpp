// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "fts_fst.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace nano_lance::fts {
namespace {

constexpr std::uint64_t kVersion = 3;
constexpr std::size_t kEmptyAddress = 0;
constexpr std::size_t kNoneAddress = 1;
constexpr std::size_t kTransIndexThreshold = 32;

// fst's src/raw/common_inputs.rs: each byte's rank among the most common key bytes. The 63 most
// common fit in a state byte (stored as rank + 1).
// clang-format off
constexpr std::uint8_t kCommonInputs[256] = {
    84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    116, 80, 117, 118, 79, 39, 30, 81, 75, 74, 82, 57, 66, 16, 12, 2,
    19, 20, 21, 27, 32, 29, 35, 36, 37, 34, 24, 73, 119, 23, 120, 40,
    83, 44, 48, 42, 43, 49, 46, 62, 61, 47, 69, 68, 58, 56, 55, 59,
    51, 72, 54, 45, 52, 64, 65, 63, 71, 67, 70, 77, 121, 78, 122, 31,
    123, 4, 25, 9, 17, 1, 26, 22, 13, 7, 50, 38, 14, 15, 10, 3,
    8, 60, 6, 5, 0, 18, 33, 11, 41, 28, 53, 124, 125, 76, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
};
// clang-format on

struct CommonInputsInv {
    std::uint8_t v[256];
    constexpr CommonInputsInv() : v() {
        for (int i = 0; i < 256; ++i) v[kCommonInputs[i]] = static_cast<std::uint8_t>(i);
    }
};
constexpr CommonInputsInv kCommonInputsInv;

std::uint8_t common_idx(std::uint8_t input) {
    const unsigned val = (kCommonInputs[input] + 1u) % 256u;
    return val > 0x3F ? 0 : static_cast<std::uint8_t>(val);
}

// The common input a state byte's low 6 bits name, or -1.
int common_input(std::uint8_t idx) { return idx == 0 ? -1 : kCommonInputsInv.v[idx - 1]; }

std::size_t pack_size(std::uint64_t n) {
    std::size_t size = 1;
    while (size < 8 && (n >> (8 * size)) != 0) ++size;
    return size;
}

void pack_uint_in(std::vector<std::uint8_t>& out, std::uint64_t n, std::size_t nbytes) {
    for (std::size_t i = 0; i < nbytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(n));
        n >>= 8;
    }
}

std::uint64_t unpack_uint(const std::uint8_t* p, std::size_t nbytes) {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < nbytes; ++i) n |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return n;
}

std::uint64_t read_u64(const std::uint8_t* p) { return unpack_uint(p, 8); }

std::uint64_t delta_of(std::size_t node_addr, std::size_t trans_addr) {
    return trans_addr == kEmptyAddress ? kEmptyAddress : node_addr - trans_addr;
}

struct Crc32cTables {
    std::uint32_t t[8][256];
    Crc32cTables() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            t[0][i] = c;
        }
        for (int s = 1; s < 8; ++s)
            for (int i = 0; i < 256; ++i) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};

std::uint32_t masked(std::uint32_t sum) { return ((sum >> 15) | (sum << 17)) + 0xA282EAD8u; }

}  // namespace

std::uint32_t crc32c(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
    static const Crc32cTables tables;
    const auto& t = tables.t;
    crc = ~crc;
    while (size >= 8) {
        std::uint32_t lo;
        std::uint32_t hi;
        std::memcpy(&lo, data, 4);
        std::memcpy(&hi, data + 4, 4);
        lo ^= crc;
        crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
        data += 8;
        size -= 8;
    }
    while (size--) crc = t[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---- reader ----------------------------------------------------------------------------------

bool FstMap::open(const std::uint8_t* data, std::size_t size, std::string& error) {
    if (size < 36) {
        error = "fst: " + std::to_string(size) + " bytes is too short";
        return false;
    }
    const std::uint64_t version = read_u64(data);
    if (version == 0 || version > kVersion) {
        error = "fst: unsupported version " + std::to_string(version);
        return false;
    }
    std::size_t end = size;
    if (version >= 3) {
        end = size - 4;
        std::uint32_t stored;
        std::memcpy(&stored, data + end, 4);
        if (masked(crc32c(0, data, end)) != stored) {
            error = "fst: checksum mismatch";
            return false;
        }
    }
    const std::uint64_t root = read_u64(data + end - 8);
    const std::uint64_t len = read_u64(data + end - 16);
    const std::size_t empty_total = version <= 2 ? 32 : 36;
    const std::size_t addr_offset = version <= 2 ? 17 : 21;
    const bool ok = root == kEmptyAddress ? size == empty_total : root + addr_offset == size;
    if (!ok) {
        error = "fst: bad root address";
        return false;
    }
    data_ = data;
    size_ = end - 16;
    version_ = version;
    root_ = static_cast<std::size_t>(root);
    len_ = len;
    return true;
}

bool FstMap::node(std::size_t addr, Node& n) const {
    n = Node{};
    if (addr == kEmptyAddress) {
        n.is_final = true;
        return true;
    }
    if (addr < 16 || addr >= size_) return false;
    const std::uint8_t v = data_[addr];
    n.start = addr;
    n.state = v;
    std::size_t need = 0;  // bytes before the state byte
    switch (v >> 6) {
        case 3:  // one transition to the node just before, output zero
            n.ntrans = 1;
            n.input_len = (v & 0x3F) == 0 ? 1 : 0;
            need = n.input_len;
            break;
        case 2: {  // one transition
            n.ntrans = 1;
            n.input_len = (v & 0x3F) == 0 ? 1 : 0;
            if (addr < n.input_len + 1 + 16) return false;
            const std::uint8_t sizes = data_[addr - n.input_len - 1];
            n.tsize = sizes >> 4;
            n.osize = sizes & 0x0F;
            need = n.input_len + 1 + n.tsize + n.osize;
            break;
        }
        default: {
            n.is_final = (v & 0x40) != 0;
            n.ntrans = v & 0x3F;
            if (n.ntrans == 0) {
                n.ntrans_len = 1;
                if (addr < 16 + 2) return false;
                n.ntrans = data_[addr - 1];
                if (n.ntrans == 1) n.ntrans = 256;
            }
            if (addr < n.ntrans_len + 1 + 16) return false;
            const std::uint8_t sizes = data_[addr - n.ntrans_len - 1];
            n.tsize = sizes >> 4;
            n.osize = sizes & 0x0F;
            n.index_size = version_ >= 2 && n.ntrans > kTransIndexThreshold ? 256 : 0;
            need = n.ntrans_len + 1 + n.index_size + n.ntrans * (1 + n.tsize) + n.ntrans * n.osize +
                   (n.is_final ? n.osize : 0);
            break;
        }
    }
    if (n.tsize > 8 || n.osize > 8 || need > addr - 16) return false;
    n.end = addr - need;
    if ((v >> 6) < 2 && n.is_final && n.osize > 0) n.final_output = unpack_uint(data_ + n.end, n.osize);
    return true;
}

bool FstMap::transition(const Node& n, std::size_t i, Trans& t) const {
    if (i >= n.ntrans) return false;
    const std::uint8_t kind = n.state >> 6;
    std::size_t tat = 0;  // the transition address's bytes
    if (kind >= 2) {
        const int common = common_input(n.state & 0x3F);
        t.inp = common < 0 ? data_[n.start - 1] : static_cast<std::uint8_t>(common);
        if (kind == 3) {
            t.out = 0;
            if (n.end == 0) return false;
            t.addr = n.end - 1;
            return t.addr >= 16;
        }
        const std::size_t base = n.start - n.input_len - 1;
        tat = base - n.tsize;
        t.out = n.osize == 0 ? 0 : unpack_uint(data_ + tat - n.osize, n.osize);
    } else {
        const std::size_t base = n.start - n.ntrans_len - 1 - n.index_size;
        t.inp = data_[base - i - 1];
        tat = base - n.ntrans - (i + 1) * n.tsize;
        const std::size_t total_trans = n.ntrans * (1 + n.tsize) + n.index_size;
        t.out = n.osize == 0 ? 0
                             : unpack_uint(data_ + (n.start - n.ntrans_len - 1 - total_trans -
                                                    (i + 1) * n.osize),
                                           n.osize);
    }
    const std::uint64_t delta = n.tsize == 0 ? 0 : unpack_uint(data_ + tat, n.tsize);
    if (delta == kEmptyAddress) {
        t.addr = kEmptyAddress;
        return true;
    }
    if (delta > n.end) return false;
    t.addr = n.end - static_cast<std::size_t>(delta);
    return t.addr >= 16 && t.addr < n.start;
}

std::optional<std::size_t> FstMap::find_input(const Node& n, std::uint8_t b) const {
    const std::uint8_t kind = n.state >> 6;
    if (n.start == 0) return std::nullopt;  // the empty final node
    if (kind >= 2) {
        const int common = common_input(n.state & 0x3F);
        const std::uint8_t inp = common < 0 ? data_[n.start - 1] : static_cast<std::uint8_t>(common);
        return inp == b ? std::optional<std::size_t>(0) : std::nullopt;
    }
    const std::size_t base = n.start - n.ntrans_len - 1;
    if (n.index_size) {
        const std::size_t i = data_[base - n.index_size + b];
        return i < n.ntrans ? std::optional<std::size_t>(i) : std::nullopt;
    }
    const std::uint8_t* inputs = data_ + base - n.ntrans;
    for (std::size_t j = 0; j < n.ntrans; ++j)
        if (inputs[j] == b) return n.ntrans - j - 1;
    return std::nullopt;
}

std::optional<std::uint64_t> FstMap::get(std::string_view key) const {
    if (!data_) return std::nullopt;
    Node n;
    if (!node(root_, n)) return std::nullopt;
    std::uint64_t out = 0;
    for (char c : key) {
        const auto i = find_input(n, static_cast<std::uint8_t>(c));
        Trans t;
        if (!i || !transition(n, *i, t)) return std::nullopt;
        out += t.out;
        if (!node(t.addr, n)) return std::nullopt;
    }
    if (!n.is_final) return std::nullopt;
    return out + n.final_output;
}

// ---- builder ---------------------------------------------------------------------------------

namespace {

struct BTrans {
    std::uint8_t inp = 0;
    std::uint64_t out = 0;
    std::size_t addr = 0;
    bool operator==(const BTrans& o) const { return inp == o.inp && out == o.out && addr == o.addr; }
};

struct BNode {
    bool is_final = false;
    std::uint64_t final_output = 0;
    std::vector<BTrans> trans;
    bool operator==(const BNode& o) const {
        return is_final == o.is_final && final_output == o.final_output && trans == o.trans;
    }
};

struct Unfinished {
    BNode node;
    bool has_last = false;
    std::uint8_t last_inp = 0;
    std::uint64_t last_out = 0;

    void last_compiled(std::size_t addr) {
        if (!has_last) return;
        node.trans.push_back({last_inp, last_out, addr});
        has_last = false;
    }
    void add_output_prefix(std::uint64_t prefix) {
        if (node.is_final) node.final_output += prefix;
        for (auto& t : node.trans) t.out += prefix;
        if (has_last) last_out += prefix;
    }
};

struct Cell {
    std::size_t addr = kNoneAddress;
    BNode node;
};

void compile_node(std::vector<std::uint8_t>& w, std::size_t last_addr, std::size_t addr,
                  const BNode& node) {
    if (node.trans.empty() && node.is_final && node.final_output == 0) return;
    if (node.trans.size() != 1 || node.is_final) {
        std::size_t tsize = 0;
        std::size_t osize = pack_size(node.final_output);
        bool any_outs = node.final_output != 0;
        for (const auto& t : node.trans) {
            tsize = std::max(tsize, pack_size(delta_of(addr, t.addr)));
            osize = std::max(osize, pack_size(t.out));
            any_outs = any_outs || t.out != 0;
        }
        const std::uint8_t sizes = static_cast<std::uint8_t>(tsize << 4 | (any_outs ? osize : 0));
        std::uint8_t state = node.is_final ? 0x40 : 0;
        const std::size_t ntrans = node.trans.size();
        if (ntrans <= 0x3F) state |= static_cast<std::uint8_t>(ntrans);
        if (any_outs) {
            if (node.is_final) pack_uint_in(w, node.final_output, osize);
            for (auto it = node.trans.rbegin(); it != node.trans.rend(); ++it)
                pack_uint_in(w, it->out, osize);
        }
        for (auto it = node.trans.rbegin(); it != node.trans.rend(); ++it)
            pack_uint_in(w, delta_of(addr, it->addr), tsize);
        for (auto it = node.trans.rbegin(); it != node.trans.rend(); ++it) w.push_back(it->inp);
        if (ntrans > kTransIndexThreshold) {
            std::array<std::uint8_t, 256> index;
            index.fill(255);
            for (std::size_t i = 0; i < ntrans; ++i)
                index[node.trans[i].inp] = static_cast<std::uint8_t>(i);
            w.insert(w.end(), index.begin(), index.end());
        }
        w.push_back(sizes);
        if ((state & 0x3F) == 0) w.push_back(ntrans == 256 ? 1 : static_cast<std::uint8_t>(ntrans));
        w.push_back(state);
        return;
    }
    const BTrans& t = node.trans[0];
    const std::uint8_t idx = common_idx(t.inp);
    if (t.addr == last_addr && t.out == 0) {
        if (idx == 0) w.push_back(t.inp);
        w.push_back(static_cast<std::uint8_t>(0xC0 | idx));
        return;
    }
    std::size_t osize = 0;
    if (t.out != 0) {
        osize = pack_size(t.out);
        pack_uint_in(w, t.out, osize);
    }
    const std::uint64_t delta = delta_of(addr, t.addr);
    const std::size_t tsize = pack_size(delta);
    pack_uint_in(w, delta, tsize);
    w.push_back(static_cast<std::uint8_t>(tsize << 4 | osize));
    if (idx == 0) w.push_back(t.inp);
    w.push_back(static_cast<std::uint8_t>(0x80 | idx));
}

}  // namespace

struct FstBuilder::Impl {
    static constexpr std::size_t kTableSize = 10000;

    std::vector<std::uint8_t> out;
    std::vector<Unfinished> stack;
    std::vector<Cell> registry = std::vector<Cell>(kTableSize * 2);
    std::string last;
    bool has_last = false;
    std::size_t last_addr = kNoneAddress;
    std::uint64_t len = 0;

    Impl() {
        out.reserve(10 * 1024);
        pack_uint_in(out, kVersion, 8);
        pack_uint_in(out, 0, 8);  // the fst type
        stack.emplace_back();
    }

    static std::size_t hash(const BNode& node) {
        constexpr std::uint64_t kPrime = 1099511628211ull;
        std::uint64_t h = 14695981039346656037ull;
        h = (h ^ static_cast<std::uint64_t>(node.is_final)) * kPrime;
        h = (h ^ node.final_output) * kPrime;
        for (const auto& t : node.trans) {
            h = (h ^ t.inp) * kPrime;
            h = (h ^ t.out) * kPrime;
            h = (h ^ static_cast<std::uint64_t>(t.addr)) * kPrime;
        }
        return static_cast<std::size_t>(h % kTableSize);
    }

    std::size_t compile(const BNode& node) {
        if (node.is_final && node.trans.empty() && node.final_output == 0) return kEmptyAddress;
        Cell* cells = &registry[2 * hash(node)];
        if (cells[0].addr != kNoneAddress && cells[0].node == node) return cells[0].addr;
        if (cells[1].addr != kNoneAddress && cells[1].node == node) {
            std::swap(cells[0], cells[1]);
            return cells[0].addr;
        }
        cells[1].node = node;
        std::swap(cells[0], cells[1]);
        compile_node(out, last_addr, out.size(), node);
        last_addr = out.size() - 1;
        cells[0].addr = last_addr;
        return last_addr;
    }

    void compile_from(std::size_t istate) {
        std::size_t addr = kNoneAddress;
        while (istate + 1 < stack.size()) {
            Unfinished u = std::move(stack.back());
            stack.pop_back();
            if (addr != kNoneAddress) u.last_compiled(addr);
            addr = compile(u.node);
        }
        stack.back().last_compiled(addr);
    }

    std::size_t common_prefix_and_set_output(std::string_view bs, std::uint64_t& out_value) {
        std::size_t i = 0;
        while (i < bs.size()) {
            Unfinished& u = stack[i];
            if (!u.has_last || u.last_inp != static_cast<std::uint8_t>(bs[i])) break;
            ++i;
            const std::uint64_t common = std::min(u.last_out, out_value);
            const std::uint64_t add_prefix = u.last_out - common;
            out_value -= common;
            u.last_out = common;
            if (add_prefix != 0) stack[i].add_output_prefix(add_prefix);
        }
        return i;
    }

    void add_suffix(std::string_view bs, std::uint64_t out_value) {
        if (bs.empty()) return;
        Unfinished& top = stack.back();
        top.has_last = true;
        top.last_inp = static_cast<std::uint8_t>(bs[0]);
        top.last_out = out_value;
        for (std::size_t k = 1; k < bs.size(); ++k) {
            Unfinished u;
            u.has_last = true;
            u.last_inp = static_cast<std::uint8_t>(bs[k]);
            stack.push_back(std::move(u));
        }
        Unfinished leaf;
        leaf.node.is_final = true;
        stack.push_back(std::move(leaf));
    }
};

FstBuilder::FstBuilder() : impl_(new Impl) {}
FstBuilder::~FstBuilder() { delete impl_; }

bool FstBuilder::insert(std::string_view key, std::uint64_t value, std::string& error) {
    Impl& b = *impl_;
    if (b.has_last && key <= std::string_view(b.last)) {
        error = key == b.last ? "fst: duplicate key" : "fst: keys out of order";
        return false;
    }
    b.last.assign(key);
    b.has_last = true;
    if (key.empty()) {
        b.len = 1;
        b.stack[0].node.is_final = true;
        b.stack[0].node.final_output = value;
        return true;
    }
    const std::size_t prefix = b.common_prefix_and_set_output(key, value);
    ++b.len;
    b.compile_from(prefix);
    b.add_suffix(key.substr(prefix), value);
    return true;
}

std::vector<std::uint8_t> FstBuilder::finish() {
    Impl& b = *impl_;
    b.compile_from(0);
    const BNode root = std::move(b.stack[0].node);
    b.stack.clear();
    const std::size_t root_addr = b.compile(root);
    pack_uint_in(b.out, b.len, 8);
    pack_uint_in(b.out, root_addr, 8);
    const std::uint32_t sum = masked(crc32c(0, b.out.data(), b.out.size()));
    pack_uint_in(b.out, sum, 4);
    return std::move(b.out);
}

}  // namespace nano_lance::fts
