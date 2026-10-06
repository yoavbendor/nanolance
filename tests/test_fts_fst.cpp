// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The fst port against the Rust fst crate: for each `<case>.txt` (lines "key<TAB>value", keys
// sorted; escapes \xNN) in the directory given, FstBuilder must write `<case>.fst` (made by
// tools/fts_tables `fst` mode) byte for byte, and FstMap must read it back to the same pairs.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../src/fts_fst.hpp"

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
