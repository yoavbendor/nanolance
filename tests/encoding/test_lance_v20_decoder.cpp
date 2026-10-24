// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance/lance_v20_decoder: format 2.0 pages, hand-built from encodings_v2_0.proto's ArrayEncoding
// (protobuf written by a few lines here, so the bytes are the spec's, not the decoder's own model). An
// int32 column of three pages -- Flat, Nullable{some_nulls{Flat(1) validity, Flat(32) values}}, and
// Nullable{all_nulls} -- in an in-memory EncodingsIo: is_v20_column tells it from a 2.1 page, it is neither
// a list nor a packed struct, and decode_column, decode_column_range (across page boundaries) and
// decode_column_rows return its values and nulls. Refused: a page naming a buffer it does not have, a
// variant the decoder does not model, and rows to take that are not ascending.

#include "nanolance/encodings_io.hpp"
#include "nanolance/lance_v20_decoder.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

class MemoryIo final : public nano_lance::EncodingsIo {
public:
    Bytes bytes;
    bool read(std::uint64_t offset, std::uint64_t size, std::vector<std::uint8_t>& out,
              std::string& error) const override {
        if (offset > bytes.size() || size > bytes.size() - offset) {
            error = "memory io: read past the end";
            return false;
        }
        out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        return true;
    }
    bool identity(std::uint64_t& size, std::int64_t& version) const override {
        size = bytes.size();
        version = 0;
        return true;
    }
    const std::string& name() const override { return name_; }

private:
    std::string name_ = "memory://v20";
};

// --- a little protobuf ---------------------------------------------------------------------------

void varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}
Bytes uint_field(std::uint32_t number, std::uint64_t v) {
    Bytes out;
    varint(out, static_cast<std::uint64_t>(number) << 3U);
    varint(out, v);
    return out;
}
Bytes message_field(std::uint32_t number, const Bytes& body) {
    Bytes out;
    varint(out, (static_cast<std::uint64_t>(number) << 3U) | 2U);
    varint(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// ArrayEncoding { flat = 1 { bits_per_value = 1, buffer = 2 { buffer_index = 1, buffer_type = 2 } } }.
Bytes flat(std::uint64_t bits, std::uint64_t buffer_index) {
    const Bytes buffer = buffer_index == 0U ? Bytes{} : uint_field(1, buffer_index);  // page buffer: type 0
    return message_field(1, cat({uint_field(1, bits), message_field(2, buffer)}));
}
// ArrayEncoding { nullable = 2 { some_nulls = 2 { validity = 1, values = 2 } } }.
Bytes some_nulls(const Bytes& validity, const Bytes& values) {
    return message_field(2, message_field(2, cat({message_field(1, validity), message_field(2, values)})));
}
// ArrayEncoding { nullable = 2 { all_nulls = 3 {} } }.
Bytes all_nulls() { return message_field(2, message_field(3, Bytes{})); }

Bytes any(const std::string& url, const Bytes& value) {
    return cat({message_field(1, Bytes(url.begin(), url.end())), message_field(2, value)});
}
Bytes array_encoding(const Bytes& e) { return any("/lance.encodings.ArrayEncoding", e); }

// --- the column ----------------------------------------------------------------------------------

std::uint64_t put(MemoryIo& io, const Bytes& b) {
    while (io.bytes.size() % 8U != 0U) {
        io.bytes.push_back(0xEE);
    }
    const auto at = io.bytes.size();
    io.bytes.insert(io.bytes.end(), b.begin(), b.end());
    return at;
}

Bytes ints(std::initializer_list<std::int32_t> values) {
    Bytes out;
    for (const auto v : values) {
        const auto at = out.size();
        out.resize(at + 4U);
        std::memcpy(out.data() + at, &v, 4U);
    }
    return out;
}

nano_lance::pb::ColumnPage page_of(MemoryIo& io, const Bytes& encoding, std::uint64_t length,
                                   const std::vector<Bytes>& buffers) {
    nano_lance::pb::ColumnPage page;
    page.length = length;
    page.encoding = array_encoding(encoding);
    for (const auto& b : buffers) {
        page.buffer_offsets.push_back(put(io, b));
        page.buffer_sizes.push_back(b.size());
    }
    return page;
}

std::int32_t int_at(const nano_lance::ColumnValues& v, std::size_t i) {
    std::int32_t x = 0;
    std::memcpy(&x, v.fixed.data() + i * 4U, 4U);
    return x;
}
bool valid_at(const nano_lance::ColumnValues& v, std::size_t i) {
    return v.validity.empty() || (v.validity[i / 8U] >> (i % 8U) & 1U) != 0U;
}

/// The column's expected rows: 10, 11, 12 | 20, null, 22, null | null, null.
struct Row {
    bool valid;
    std::int32_t value;
};
const std::vector<Row> kRows = {{true, 10}, {true, 11}, {true, 12}, {true, 20}, {false, 0},
                                {true, 22}, {false, 0}, {false, 0}, {false, 0}};

void check_rows(const nano_lance::ColumnValues& got, const std::vector<std::size_t>& which, const std::string& what) {
    require(got.kind == nano_lance::ColumnValues::Kind::FixedWidth && got.rows == which.size(),
            what + ": " + std::to_string(which.size()) + " rows, got " + std::to_string(got.rows));
    std::uint64_t nulls = 0;
    for (std::size_t k = 0; k < which.size(); ++k) {
        const auto& want = kRows[which[k]];
        require(valid_at(got, k) == want.valid, what + ": row " + std::to_string(which[k]) + " validity");
        if (want.valid) {
            require(int_at(got, k) == want.value, what + ": row " + std::to_string(which[k]) + " value");
        } else {
            ++nulls;
        }
    }
    require(got.null_count == nulls, what + ": null count");
}

}  // namespace

int main() {
    MemoryIo io;
    io.bytes.assign(16, 0xEE);
    nano_lance::pb::ColumnMetadata column;
    column.pages.push_back(page_of(io, flat(32, 0), 3, {ints({10, 11, 12})}));
    // Validity bits LSB first: rows 0 and 2 valid, 1 and 3 null.
    column.pages.push_back(
        page_of(io, some_nulls(flat(1, 0), flat(32, 1)), 4, {Bytes{0b0101}, ints({20, 0, 22, 0})}));
    column.pages.push_back(page_of(io, all_nulls(), 2, {}));

    nano_lance::pb::Field field;
    field.name = "x";
    field.logical_type = "int32";
    std::string error;

    require(nano_lance::v20::is_v20_column(column), "a 2.0 column");
    require(!nano_lance::v20::column_is_list_encoded(column), "not a list");
    require(!nano_lance::v20::column_is_packed_struct(column), "not a packed struct");
    nano_lance::pb::ColumnMetadata v21;
    v21.pages.push_back(nano_lance::pb::ColumnPage{});
    v21.pages[0].encoding = any("/lance.encodings21.PageLayout", Bytes{});
    require(!nano_lance::v20::is_v20_column(v21), "a 2.1 page is not 2.0");

    nano_lance::ColumnValues all;
    require(nano_lance::v20::decode_column(io, field, column, nullptr, all, error), "decode_column: " + error);
    check_rows(all, {0, 1, 2, 3, 4, 5, 6, 7, 8}, "whole column");

    nano_lance::ColumnValues range;
    require(nano_lance::v20::decode_column_range(io, field, column, nullptr, 2, 6, 4, range, error),
            "decode_column_range: " + error);
    check_rows(range, {2, 3, 4, 5, 6, 7}, "rows 2..8, across all three pages");

    nano_lance::ColumnValues rows;
    require(nano_lance::v20::decode_column_rows(io, field, column, nullptr, {0, 4, 5, 8}, 4, rows, error),
            "decode_column_rows: " + error);
    check_rows(rows, {0, 4, 5, 8}, "rows 0, 4, 5, 8");

    nano_lance::ColumnValues refused;
    require(!nano_lance::v20::decode_column_rows(io, field, column, nullptr, {5, 4}, 4, refused, error),
            "rows not ascending are refused");
    require(!nano_lance::v20::decode_column_range(io, field, column, nullptr, 8, 2, 4, refused, error),
            "a range past the column is refused");

    // A page naming page buffer 3 of 1.
    nano_lance::pb::ColumnMetadata missing;
    missing.pages.push_back(page_of(io, flat(32, 3), 3, {ints({1, 2, 3})}));
    error.clear();
    require(!nano_lance::v20::decode_column(io, field, missing, nullptr, refused, error), "a missing buffer is refused");
    require(error.find("buffer 3") != std::string::npos, "the refusal names the buffer: " + error);

    // A variant the model does not declare (field 99): refused, not guessed at.
    nano_lance::pb::ColumnMetadata unknown;
    unknown.pages.push_back(page_of(io, message_field(99, Bytes{}), 3, {ints({1, 2, 3})}));
    error.clear();
    require(!nano_lance::v20::decode_column(io, field, unknown, nullptr, refused, error), "an unknown variant is refused");

    std::cout << "lance_v20_decoder: ok\n";
    return 0;
}
