// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The fst port against the Rust fst crate: for each `<case>.txt` (lines "key<TAB>value", keys
// sorted; escapes \xNN) in the directory given, FstBuilder must write `<case>.fst` (made by
// tools/fts_tables `fst` mode) byte for byte, and FstMap must read it back to the same pairs.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../src/fts_fst.hpp"
#include "../src/fts_posting.hpp"

using nano_lance::fts::FstBuilder;
using nano_lance::fts::FstMap;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 2, 2), nullptr, 16)));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

using Pairs = std::vector<std::pair<std::string, std::uint64_t>>;

std::vector<std::uint8_t> build(const Pairs& pairs) {
    FstBuilder b;
    std::string error;
    for (const auto& [k, v] : pairs) {
        if (!b.insert(k, v, error)) {
            check(false, "insert: " + error);
            break;
        }
    }
    return b.finish();
}

void round_trip(const Pairs& pairs, const std::vector<std::uint8_t>& bytes, const std::string& name) {
    FstMap map;
    std::string error;
    if (!map.open(bytes.data(), bytes.size(), error)) {
        check(false, name + ": open: " + error);
        return;
    }
    check(map.size() == pairs.size(), name + ": size");
    Pairs got;
    check(map.for_each([&](std::string_view k, std::uint64_t v) { got.emplace_back(k, v); }),
          name + ": for_each");
    check(got == pairs, name + ": stream differs");
    for (const auto& [k, v] : pairs) {
        auto r = map.get(k);
        if (!r || *r != v) {
            check(false, name + ": get");
            break;
        }
        auto miss = map.get(k + "\x01");
        if (miss && !std::binary_search(pairs.begin(), pairs.end(), std::make_pair(k + "\x01", *miss))) {
            check(false, name + ": phantom key");
            break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Builder <-> reader on random maps.
    std::mt19937_64 rng(7);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::string> keys;
        const int n = static_cast<int>(rng() % 300);
        const int alphabet = round % 3 == 0 ? 256 : 4 + static_cast<int>(rng() % 30);
        for (int i = 0; i < n; ++i) {
            std::string k;
            const int len = static_cast<int>(rng() % 8);
            for (int j = 0; j < len; ++j) k.push_back(static_cast<char>(rng() % alphabet));
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        Pairs pairs;
        for (auto& k : keys) pairs.emplace_back(k, round % 4 == 0 ? rng() : rng() % 1000);
        round_trip(pairs, build(pairs), "random " + std::to_string(round));
    }

    // Out-of-order and duplicate keys are refused.
    {
        FstBuilder b;
        std::string error;
        check(b.insert("b", 1, error), "insert b");
        check(!b.insert("a", 1, error), "out of order refused");
        check(!b.insert("b", 1, error), "duplicate refused");
    }

    // A corrupt map is refused, not crashed on.
    {
        Pairs pairs{{"apple", 1}, {"apply", 2}, {"banana", 3}};
        auto bytes = build(pairs);
        FstMap map;
        std::string error;
        bytes[20] ^= 1;
        check(!map.open(bytes.data(), bytes.size(), error), "checksum mismatch refused");
    }

    // Corrupt maps (their checksums made good, so the nodes get parsed) are read without crashing.
    {
        std::mt19937_64 frng(11);
        Pairs pairs;
        for (int i = 0; i < 400; ++i) {
            pairs.emplace_back("k" + std::to_string(i * 7919 % 100000), static_cast<std::uint64_t>(i) * 31U);
        }
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                    pairs.end());
        const auto good = build(pairs);
        for (int round = 0; round < 3000; ++round) {
            auto bytes = good;
            const int flips = 1 + static_cast<int>(frng() % 4);
            for (int f = 0; f < flips; ++f) {
                bytes[16 + frng() % (bytes.size() - 36)] ^= static_cast<std::uint8_t>(1U + frng() % 255U);
            }
            const std::uint32_t sum = nano_lance::fts::crc32c(0, bytes.data(), bytes.size() - 4);
            const std::uint32_t masked = ((sum >> 15) | (sum << 17)) + 0xA282EAD8U;
            std::memcpy(bytes.data() + bytes.size() - 4, &masked, 4);
            FstMap map;
            std::string error;
            if (map.open(bytes.data(), bytes.size(), error)) {
                std::size_t n = 0;
                map.for_each([&](std::string_view, std::uint64_t) { ++n; });
                for (int k = 0; k < 20; ++k) {
                    (void)map.get(pairs[frng() % pairs.size()].first);
                }
            }
        }
    }

    // Random posting blocks are refused or decoded, never read out of bounds.
    {
        std::mt19937_64 prng(5);
        for (int round = 0; round < 20000; ++round) {
            const std::uint32_t length = static_cast<std::uint32_t>(prng() % 400);
            const std::size_t nblocks = (length + 127) / 128;
            std::vector<std::vector<std::uint8_t>> storage(nblocks);
            std::vector<nano_lance::fts::PostingBlockView> blocks;
            for (auto& b : storage) {
                b.resize(prng() % 600);
                for (auto& x : b) {
                    x = static_cast<std::uint8_t>(prng());
                }
                if (!b.empty() && prng() % 2 == 0 && b.size() > 8) {
                    b[8] = static_cast<std::uint8_t>(prng() % 33);  // a plausible bit width
                }
                blocks.push_back({b.data(), b.size()});
            }
            std::vector<std::uint32_t> docs;
            std::vector<std::uint32_t> freqs;
            std::string error;
            (void)nano_lance::fts::decode_posting(blocks, length, nano_lance::fts::TailCodec::VarintDelta, docs, freqs,
                                                  error);
            docs.clear();
            freqs.clear();
            (void)nano_lance::fts::decode_posting(blocks, length, nano_lance::fts::TailCodec::Fixed32, docs, freqs,
                                                  error);
        }
        // And encoded blocks round-trip.
        for (int round = 0; round < 200; ++round) {
            const std::size_t n = 1 + prng() % 128;
            std::vector<std::uint32_t> docs(n);
            std::vector<std::uint32_t> freqs(n);
            std::uint32_t d = static_cast<std::uint32_t>(prng() % 1000);
            for (std::size_t i = 0; i < n; ++i) {
                d += static_cast<std::uint32_t>(prng() % (round % 2 == 0 ? 3 : 100000));
                docs[i] = d;
                freqs[i] = 1U + static_cast<std::uint32_t>(prng() % (round % 3 == 0 ? 2 : 70000));
            }
            std::vector<std::uint8_t> block;
            nano_lance::fts::encode_posting_block(docs.data(), freqs.data(), n, 1.5F, block);
            std::vector<std::uint32_t> d2;
            std::vector<std::uint32_t> f2;
            std::string error;
            const bool ok = nano_lance::fts::decode_posting({{block.data(), block.size()}}, static_cast<std::uint32_t>(n),
                                                            nano_lance::fts::TailCodec::VarintDelta, d2, f2, error);
            check(ok && d2 == docs && f2 == freqs, "posting block round trip");
        }
    }

    // Byte-for-byte against the Rust crate.
    if (argc > 1) {
        int cases = 0;
        for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
            if (entry.path().extension() != ".txt") continue;
            Pairs pairs;
            std::ifstream in(entry.path());
            std::string line;
            while (std::getline(in, line)) {
                const auto tab = line.rfind('\t');
                pairs.emplace_back(unescape(line.substr(0, tab)), std::stoull(line.substr(tab + 1)));
            }
            auto fst_path = entry.path();
            fst_path.replace_extension(".fst");
            std::ifstream fin(fst_path, std::ios::binary);
            std::vector<std::uint8_t> expected((std::istreambuf_iterator<char>(fin)),
                                               std::istreambuf_iterator<char>());
            const auto got = build(pairs);
            check(got == expected, entry.path().filename().string() + ": bytes differ from fst crate (" +
                                       std::to_string(got.size()) + " vs " +
                                       std::to_string(expected.size()) + ")");
            round_trip(pairs, expected, entry.path().filename().string());
            ++cases;
        }
        check(cases > 0, "no golden cases");
        std::cout << cases << " golden cases\n";
    }
    if (failures) return 1;
    std::cout << "fst: ok\n";
    return 0;
}
