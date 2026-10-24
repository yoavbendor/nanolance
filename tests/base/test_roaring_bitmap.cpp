// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The Roaring bitmap codec and the manifest's index section. Lance keeps each index's fragment
// coverage as a Roaring bitmap; nanolance rewrites it when a commit narrows that coverage, and must
// write back everything else about an index exactly as it read it.

#include "lance_minimal.pb.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v >> 8U));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16U);
}

std::vector<std::uint32_t> roundtrip(const std::vector<std::uint32_t>& ids) {
    const auto bytes = nano_lance::roaring::encode(ids);
    std::vector<std::uint32_t> back;
    std::string error;
    require(nano_lance::roaring::decode(bytes.data(), bytes.size(), back, error), error);
    return back;
}

void varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field, const std::vector<std::uint8_t>& value) {
    varint(out, (static_cast<std::uint64_t>(field) << 3U) | 2U);
    varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

}  // namespace

int main() {
    // Round trips: empty, array containers, a bitmap container (over 4096 in one), several containers.
    {
        require(roundtrip({}).empty(), "the empty set");
        const std::vector<std::uint32_t> small = {0, 1, 2, 7, 65535, 65536, 70000, 4000000000U};
        require(roundtrip(small) == small, "array containers");
        std::vector<std::uint32_t> dense;
        for (std::uint32_t v = 100; v < 20000; v += 2) {
            dense.push_back(v);
        }
        require(roundtrip(dense) == dense, "a bitmap container");
        std::mt19937 rng(7);
        for (int trial = 0; trial < 200; ++trial) {
            std::set<std::uint32_t> s;
            const auto n = rng() % 9000U;
            const auto span = 1U + rng() % (1U << 20U);
            for (std::uint32_t k = 0; k < n; ++k) {
                s.insert(rng() % span);
            }
            const std::vector<std::uint32_t> ids(s.begin(), s.end());
            require(roundtrip(ids) == ids, "random set " + std::to_string(trial));
        }
    }

    // Run containers, as a writer that optimizes its bitmaps stores them (cookie 12347): one container
    // (no offset header), then five (with one).
    {
        std::vector<std::uint8_t> bytes;
        put32(bytes, 12347U);  // one container
        bytes.push_back(0x01U);                        // it is a run container
        put16(bytes, 0);                               // key
        put16(bytes, 999);                             // cardinality - 1
        put16(bytes, 1);                               // one run
        put16(bytes, 5);                               // from 5
        put16(bytes, 999);                             // 1000 values
        std::vector<std::uint32_t> got;
        std::string error;
        require(nano_lance::roaring::decode(bytes.data(), bytes.size(), got, error), error);
        require(got.size() == 1000U && got.front() == 5U && got.back() == 1004U, "a run container");

        bytes.clear();
        put32(bytes, 12347U | (4U << 16U));  // five containers
        bytes.push_back(0x15U);              // containers 0, 2, 4 are runs
        for (std::uint32_t c = 0; c < 5; ++c) {
            put16(bytes, c);
            put16(bytes, c % 2U == 0U ? 9U : 1U);  // runs of 10; arrays of 2
        }
        for (int c = 0; c < 5; ++c) {
            put32(bytes, 0);  // offsets: present from 4 containers on (a reader may ignore them)
        }
        for (std::uint32_t c = 0; c < 5; ++c) {
            if (c % 2U == 0U) {
                put16(bytes, 1);
                put16(bytes, 20);
                put16(bytes, 9);
            } else {
                put16(bytes, 3);
                put16(bytes, 9);
            }
        }
        require(nano_lance::roaring::decode(bytes.data(), bytes.size(), got, error), error);
        require(got.size() == 34U && got[0] == 20U && got[10] == 65536U + 3U && got.back() == (4U << 16U) + 29U,
                "runs and arrays mixed, with offsets");
    }

    // Malformed input is refused, not misread.
    {
        std::string error;
        std::vector<std::uint32_t> got;
        const std::vector<std::uint8_t> unknown = {1, 2, 3, 4, 0, 0, 0, 0};
        require(!nano_lance::roaring::decode(unknown.data(), unknown.size(), got, error), "an unknown cookie");
        auto good = nano_lance::roaring::encode({1, 2, 3, 70000});
        for (std::size_t cut = 0; cut < good.size(); ++cut) {
            require(!nano_lance::roaring::decode(good.data(), cut, got, error), "truncated at " + std::to_string(cut));
        }
        std::vector<std::uint8_t> out_of_order;
        put32(out_of_order, 12346U);
        put32(out_of_order, 2);
        put16(out_of_order, 5);
        put16(out_of_order, 0);
        put16(out_of_order, 3);
        put16(out_of_order, 0);
        put32(out_of_order, 0);
        put32(out_of_order, 0);
        put16(out_of_order, 1);
        put16(out_of_order, 1);
        require(!nano_lance::roaring::decode(out_of_order.data(), out_of_order.size(), got, error),
                "container keys out of order");
    }

    // The index section: an index read and written back unchanged is the same bytes; one whose
    // coverage the writer narrowed keeps every other field, including ones this codec does not model.
    {
        std::vector<std::uint8_t> index;
        std::vector<std::uint8_t> uuid = {0x0A, 0x10};
        for (int i = 0; i < 16; ++i) {
            uuid.push_back(static_cast<std::uint8_t>(i));
        }
        bytes_field(index, 1, uuid);
        bytes_field(index, 2, {3, 7});  // fields 3 and 7, packed
        bytes_field(index, 3, {'i', 'd', '_', 'i', 'd', 'x'});
        varint(index, 4U << 3U);
        varint(index, 12);  // built at version 12
        bytes_field(index, 5, nano_lance::roaring::encode({0, 1, 2, 5}));
        bytes_field(index, 6, {0x0A, 0x03, 'x', 'y', 'z'});  // index details, not modelled
        varint(index, 8U << 3U);
        varint(index, 1700000000000ULL);  // created_at
        std::vector<std::uint8_t> section;
        bytes_field(section, 1, index);

        std::vector<nano_lance::pb::IndexMetadata> indices;
        std::string error;
        require(nano_lance::pb::decode_index_section(section, indices, error), error);
        require(indices.size() == 1U, "one index");
        const auto& idx = indices.front();
        require(idx.name == "id_idx" && idx.dataset_version == 12U, "name and version");
        require(idx.fields == std::vector<std::int32_t>({3, 7}), "packed fields");
        require(idx.has_fragment_bitmap && idx.fragment_ids == std::vector<std::uint32_t>({0, 1, 2, 5}), "coverage");
        require(nano_lance::pb::encode_index_section(indices) == section, "an unchanged index is written as read");

        indices.front().fragment_ids = {0, 5};
        indices.front().fragment_bitmap_changed = true;
        std::vector<nano_lance::pb::IndexMetadata> again;
        require(nano_lance::pb::decode_index_section(nano_lance::pb::encode_index_section(indices), again, error), error);
        require(again.front().fragment_ids == std::vector<std::uint32_t>({0, 5}), "narrowed coverage");
        require(again.front().name == "id_idx" && again.front().fields == std::vector<std::int32_t>({3, 7}) &&
                    again.front().dataset_version == 12U,
                "the rest of the index survives");
        const auto& raw = again.front().raw;
        const std::string details = "xyz";
        require(std::search(raw.begin(), raw.end(), details.begin(), details.end()) != raw.end() &&
                    std::search(raw.begin(), raw.end(), uuid.begin() + 2, uuid.end()) != raw.end(),
                "fields this codec does not model are kept");

        std::vector<std::uint8_t> broken = section;
        broken.resize(broken.size() - 3U);
        require(!nano_lance::pb::decode_index_section(broken, again, error), "a truncated index section");
    }

    std::cout << "roaring bitmap ok\n";
    return 0;
}
