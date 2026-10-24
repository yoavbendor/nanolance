// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// blob_v2_layout: packing an external blob v2 descriptor into its Lance record (every field at its documented
// offset, little-endian, the prefix excluding itself), appending legacy blob rows as inline descriptors (with
// the validity bitmap for nulls), and parsing a control buffer back into per-row packed sizes: the 1, 2, 4 and
// 8-byte widths, the older nanolance layout, and the refusals (a size no width explains, a first offset that is
// not 0, offsets that go backwards or step by more than a u32, a row count that overflows).

#include "nanolance/blob_v2_layout.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace nano_lance;
using Bytes = std::vector<std::uint8_t>;

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::uint64_t le(const Bytes& b, std::size_t at, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v |= static_cast<std::uint64_t>(b.at(at + i)) << (8U * i);
    }
    return v;
}

// Cumulative offsets 0, s0, s0+s1, ... written `width` bytes each.
Bytes control_of(const std::vector<std::uint64_t>& sizes, std::size_t width) {
    Bytes out;
    std::uint64_t cum = 0;
    auto put = [&](std::uint64_t v) {
        for (std::size_t i = 0; i < width; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
        }
    };
    put(0);
    for (auto s : sizes) {
        cum += s;
        put(cum);
    }
    return out;
}

bool parse(const Bytes& control, std::uint64_t rows, std::vector<std::uint32_t>& sizes, std::string& err) {
    return blob_v2_control_buffer_to_row_sizes(control, rows, sizes, err);
}

void test_pack() {
    BlobV2ExternalDescriptor d;
    d.position = 0x0102030405060708ULL;
    d.size = 0x1112131415161718ULL;
    d.blob_id = 0xA1B2C3D4U;
    d.blob_uri = "s3://bucket/key";
    const auto row = blob_v2_pack_descriptor_row(d);
    require(row.size() == kBlobV2RecordHeaderBytes + d.blob_uri.size(), "record is header + uri");
    require(le(row, 0, 4) == kBlobV2FixedDescriptorBytes + d.blob_uri.size(), "prefix excludes itself");
    require(row[4] == kBlobKindExternal, "kind 3 by default");
    require(le(row, 5, 8) == d.position, "position");
    require(le(row, 13, 8) == d.size, "size");
    require(le(row, 21, 4) == d.blob_id, "blob id");
    require(le(row, 25, 4) == d.blob_uri.size(), "uri length");
    require(std::string(row.begin() + 29, row.end()) == d.blob_uri, "uri bytes");

    // Empty uri, extreme values, another kind.
    BlobV2ExternalDescriptor e;
    e.kind = kBlobKindDedicated;
    e.position = ~0ULL;
    e.size = 0;
    const auto er = blob_v2_pack_descriptor_row(e);
    require(er.size() == 29U && le(er, 0, 4) == 25U && er[4] == 2U && le(er, 5, 8) == ~0ULL && le(er, 13, 8) == 0U &&
                le(er, 25, 4) == 0U,
            "empty uri record");

    // Every uri length yields a record whose prefix + 4 is its length.
    for (std::size_t n : {1U, 255U, 256U, 70000U}) {
        BlobV2ExternalDescriptor f;
        f.blob_uri.assign(n, 'u');
        const auto r = blob_v2_pack_descriptor_row(f);
        require(r.size() == 29U + n && le(r, 0, 4) + 4U == r.size() && le(r, 25, 4) == n, "uri length " + std::to_string(n));
    }
}

void test_legacy_rows() {
    ColumnValues cv;
    blob_append_legacy_row(cv, 100, 40, false);
    require(cv.kind == ColumnValues::Kind::BlobV2External, "kind becomes blob v2");
    require(cv.validity.empty() && cv.null_count == 0U, "no validity while nothing is null");
    blob_append_legacy_row(cv, 0, 0, true);
    require(cv.null_count == 1U && cv.validity.size() == 1U && (cv.validity[0] & 0x3U) == 0x1U,
            "row 1 null, row 0 still valid");
    blob_append_legacy_row(cv, 200, 8, false);
    require((cv.validity[0] & 0x7U) == 0x5U && cv.null_count == 1U, "row 2 valid after the null");

    require(cv.blob_v2.row_packed_sizes.size() == 3U, "one size per row");
    require(cv.blob_v2.row_packed_sizes[0] == 29U && cv.blob_v2.row_packed_sizes[1] == 0U &&
                cv.blob_v2.row_packed_sizes[2] == 29U,
            "a null row packs no bytes");
    require(cv.blob_v2.packed_payload.size() == 58U, "two records");
    const Bytes second(cv.blob_v2.packed_payload.begin() + 29, cv.blob_v2.packed_payload.end());
    require(second[4] == kBlobKindInline && le(second, 5, 8) == 200U && le(second, 13, 8) == 8U && le(second, 25, 4) == 0U,
            "legacy rows are inline descriptors");

    // The validity bitmap grows a byte every 8 rows, and a leading null is handled.
    ColumnValues nulls;
    for (int i = 0; i < 9; ++i) {
        blob_append_legacy_row(nulls, 0, 0, i == 0 || i == 8);
    }
    require(nulls.null_count == 2U && nulls.validity.size() == 2U && nulls.validity[0] == 0xFEU &&
                nulls.validity[1] == 0xFEU,
            "9 rows with the first and last null");
}

void test_control_roundtrip() {
    const std::vector<std::uint64_t> sizes{29, 0, 40, 29};
    for (std::size_t width : {1U, 2U, 4U, 8U}) {
        std::vector<std::uint32_t> out;
        std::string err = "stale";
        require(parse(control_of(sizes, width), sizes.size(), out, err), "width " + std::to_string(width) + ": " + err);
        require(err.empty(), "error cleared");
        require(out == std::vector<std::uint32_t>(sizes.begin(), sizes.end()), "sizes at width " + std::to_string(width));
    }

    // Large sizes need the wide widths.
    std::vector<std::uint32_t> out;
    std::string err;
    require(parse(control_of({70000, 5}, 4), 2, out, err) && out[0] == 70000U && out[1] == 5U, "u32 offsets");
    require(parse(control_of({0xFFFFFFFFULL, 1}, 8), 2, out, err) && out[0] == 0xFFFFFFFFU && out[1] == 1U,
            "a u32-sized row in u64 offsets");

    // Zero rows: a single offset 0.
    require(parse(Bytes{0}, 0, out, err) && out.empty(), "0 rows, width 1");
    require(parse(Bytes(8, 0), 0, out, err) && out.empty(), "0 rows, width 8");

    // The older nanolance layout: [0][1] then each row's u32 end.
    Bytes older{0, 1};
    for (std::uint32_t end : {29U, 29U, 70U}) {
        for (int i = 0; i < 4; ++i) {
            older.push_back(static_cast<std::uint8_t>(end >> (8 * i)));
        }
    }
    require(parse(older, 3, out, err), "older layout: " + err);
    require(out == std::vector<std::uint32_t>({29U, 0U, 41U}), "older layout sizes");
}

void test_control_refusals() {
    std::vector<std::uint32_t> out{1, 2};
    std::string err;
    require(!parse({}, 3, out, err) && !err.empty() && out.empty(), "empty control for 3 rows");
    require(!parse(Bytes(5, 0), 2, out, err), "5 bytes for 3 entries");
    require(!parse(Bytes(9, 0), 2, out, err), "width 3 is not a width");
    require(parse(Bytes(16, 0), 1, out, err) && out == std::vector<std::uint32_t>{0U}, "16 zero bytes for 1 row is width 8, offsets 0 0");
    require(!parse(Bytes(40, 0), 1, out, err), "width 20 is not a width");

    // First offset must be 0.
    auto bad_start = control_of({5, 5}, 2);
    bad_start[0] = 1;
    require(!parse(bad_start, 2, out, err) && err.find("start at 0") != std::string::npos, "first offset not 0");

    // Offsets must not go backwards.
    Bytes backwards{0, 10, 4};
    require(!parse(backwards, 2, out, err) && err.find("not increasing") != std::string::npos, "offsets go backwards");

    // A row larger than a u32 cannot be a packed record size.
    Bytes huge = control_of({0x100000000ULL}, 8);
    require(!parse(huge, 1, out, err) && err.find("not increasing") != std::string::npos, "row over u32");

    // Row-count overflow is refused before any allocation.
    require(!parse(Bytes(8, 0), 0xFFFFFFFFULL, out, err) && err == "blob row count overflow", "row count overflow");

    // The older layout needs its [0][1] marker.
    Bytes not_older{0, 2, 9, 0, 0, 0, 9, 0, 0, 0};
    require(!parse(not_older, 2, out, err), "older-sized buffer without the marker");
    // With no rows the marker is just a 2-byte offset 0x0100, which does not start at 0.
    require(!parse(Bytes{0, 1}, 0, out, err), "marker bytes alone for 0 rows");
}

}  // namespace

int main() {
    test_pack();
    test_legacy_rows();
    test_control_roundtrip();
    test_control_refusals();
    std::cout << "blob_v2_layout: ok\n";
    return 0;
}
