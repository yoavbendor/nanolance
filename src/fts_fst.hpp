// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The `fst` crate's (0.4.7) finite-state transducer maps, which Lance stores an INVERTED index's
/// vocabulary in (`_token_fst_bytes`: token -> token id). The reader takes any version 1-3 map; the
/// builder writes version 3 the way the crate's `MapBuilder` does, byte for byte (the same node
/// encodings, suffix sharing through the same 10000 x 2 registry, and the CRC32C trailer).
namespace nano_lance::fts {

std::uint32_t crc32c(std::uint32_t crc, const std::uint8_t* data, std::size_t size);

class FstMap {
public:
    /// Checks the header and footer (and, for version 3, the checksum). The bytes must outlive
    /// the map.
    bool open(const std::uint8_t* data, std::size_t size, std::string& error);

    std::uint64_t size() const { return len_; }

    std::optional<std::uint64_t> get(std::string_view key) const;

    /// Every (key, value) in key order; false if the transducer is malformed.
    template <class F>
    bool for_each(F&& f) const;

private:
    struct Node {
        std::size_t start = 0;  // address of the state byte (the node's last byte)
        std::size_t end = 0;    // first byte of the node
        std::uint8_t state = 0;
        bool is_final = false;
        std::size_t ntrans = 0;
        std::size_t tsize = 0;
        std::size_t osize = 0;
        std::size_t input_len = 0;
        std::size_t ntrans_len = 0;
        std::size_t index_size = 0;
        std::uint64_t final_output = 0;
    };
    struct Trans {
        std::uint8_t inp = 0;
        std::uint64_t out = 0;
        std::size_t addr = 0;
    };

    bool node(std::size_t addr, Node& out) const;
    bool transition(const Node& n, std::size_t i, Trans& out) const;
    std::optional<std::size_t> find_input(const Node& n, std::uint8_t b) const;

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;  // the bytes nodes live in (before the footer)
    std::uint64_t version_ = 0;
    std::size_t root_ = 0;
    std::uint64_t len_ = 0;
};

class FstBuilder {
public:
    FstBuilder();
    ~FstBuilder();
    FstBuilder(const FstBuilder&) = delete;
    FstBuilder& operator=(const FstBuilder&) = delete;

    /// Keys must come in strictly increasing byte order; false (with `error`) otherwise.
    bool insert(std::string_view key, std::uint64_t value, std::string& error);

    /// The finished map's bytes.
    std::vector<std::uint8_t> finish();

private:
    struct Impl;
    Impl* impl_;
};

template <class F>
bool FstMap::for_each(F&& f) const {
    struct Frame {
        Node node;
        std::size_t next;
        std::uint64_t out;
    };
    std::vector<Frame> stack;
    std::string key;
    Node root;
    if (!node(root_, root)) return false;
    if (root.is_final) f(std::string_view(key), root.final_output);
    stack.push_back({root, 0, 0});
    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next >= top.node.ntrans) {
            stack.pop_back();
            if (!key.empty()) key.pop_back();
            continue;
        }
        Trans t;
        if (!transition(top.node, top.next++, t)) return false;
        Node child;
        if (!node(t.addr, child)) return false;
        if (stack.size() > size_ + 1) return false;  // a cycle
        const std::uint64_t out = top.out + t.out;
        key.push_back(static_cast<char>(t.inp));
        if (child.is_final) f(std::string_view(key), out + child.final_output);
        stack.push_back({child, 0, out});
    }
    return true;
}

}  // namespace nano_lance::fts
