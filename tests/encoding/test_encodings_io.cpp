// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance/encodings_io: the page decoders read a column only through an EncodingsIo, so any byte store
// can serve them. An in-memory EncodingsIo serves two hand-built format 2.1 pages: a constant utf8 page
// whose value is in a buffer (read through the io at exactly that buffer's offset and size) and a constant
// int64 page with its value inline in the descriptor (no read at all). Whole-column and row-subset decodes
// come back right; a failing read's message reaches the caller; and a page whose buffer lies past the
// store's end is refused, not read out of bounds.

#include "encoding/lance_descriptors.hpp"
#include "encoding/values/scalar_value.hpp"
#include "nanolance/encodings_io.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/schema_mapper.hpp"

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

/// A file held in memory; every read is recorded, and `fail` makes reads fail with a message of its own.
class MemoryIo final : public nano_lance::EncodingsIo {
public:
    explicit MemoryIo(Bytes bytes) : bytes_(std::move(bytes)) {}
    bool read(std::uint64_t offset, std::uint64_t size, std::vector<std::uint8_t>& out,
              std::string& error) const override {
        reads.emplace_back(offset, size);
        if (fail) {
            error = "memory io: refused on purpose";
            return false;
        }
        if (offset > bytes_.size() || size > bytes_.size() - offset) {
            error = "memory io: read past the end";
            return false;
        }
        out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                   bytes_.begin() + static_cast<std::ptrdiff_t>(offset + size));
        return true;
    }
    bool identity(std::uint64_t& size, std::int64_t& version) const override {
        size = bytes_.size();
        version = 1;
        return true;
    }
    const std::string& name() const override { return name_; }

    mutable std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    bool fail = false;

private:
    Bytes bytes_;
    std::string name_ = "memory://test";
};

nano_lance::pb::Field field(const std::string& name, const std::string& type) {
    nano_lance::pb::Field f;
    f.name = name;
    f.logical_type = type;
    f.nullable = false;
    f.encoding = nano_lance::lance_on_disk_field_encoding(type);  // as the writer records it
    return f;
}

std::string value_at(const nano_lance::ColumnValues& v, std::size_t i) {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::memcpy(&begin, v.variable.offsets.data() + i * 4U, 4U);
    std::memcpy(&end, v.variable.offsets.data() + (i + 1U) * 4U, 4U);
    return std::string(v.variable.data.begin() + begin, v.variable.data.begin() + end);
}

void test_value_in_a_buffer() {
    const std::string text = "hello lance";
    const auto scalar = nano_lance::scalar_value::encode_variable(Bytes(text.begin(), text.end()), false);
    Bytes file(100, 0xEE);  // something before the buffer, so its offset matters
    const std::uint64_t at = file.size();
    file.insert(file.end(), scalar.begin(), scalar.end());
    file.insert(file.end(), 20, 0xEE);

    nano_lance::descriptor::Constant c;
    c.layers = {1};
    nano_lance::pb::ColumnMetadata column;
    nano_lance::pb::ColumnPage page;
    page.buffer_offsets = {at};
    page.buffer_sizes = {scalar.size()};
    page.length = 5;
    page.encoding = nano_lance::descriptor::page_encoding(c);
    column.pages.push_back(page);

    MemoryIo io(file);
    nano_lance::ColumnValues out;
    std::string error;
    require(nano_lance::decode_lance_physical_column(io, field("s", "string"), column, out, error), "decode: " + error);
    require(out.kind == nano_lance::ColumnValues::Kind::VariableWidth && out.variable.offsets.size() == 6U * 4U, "five string rows (kind " + std::to_string(static_cast<int>(out.kind)) + ", offsets " + std::to_string(out.variable.offsets.size()) + ", data " + std::to_string(out.variable.data.size()) + ")");
    for (std::size_t i = 0; i < 5U; ++i) {
        require(value_at(out, i) == text, "row " + std::to_string(i) + " is the constant");
    }
    require(!io.reads.empty(), "the value was read through the io");
    for (const auto& [offset, size] : io.reads) {
        require(offset == at && size == scalar.size(), "only the page's buffer is read");
    }

    nano_lance::ColumnValues some;
    require(nano_lance::decode_lance_physical_column_rows(io, field("s", "string"), column, {1, 4}, 0, some, error),
            "decode rows: " + error);
    require(some.variable.offsets.size() == 3U * 4U && value_at(some, 0) == text && value_at(some, 1) == text, "rows 1 and 4");

    MemoryIo failing(file);
    failing.fail = true;
    nano_lance::ColumnValues none;
    error.clear();
    require(!nano_lance::decode_lance_physical_column(failing, field("s", "string"), column, none, error),
            "a failing read fails the decode");
    require(error.find("refused on purpose") != std::string::npos, "the io's message reaches the caller: " + error);

    // The page claims a buffer past the store's end.
    column.pages[0].buffer_offsets = {file.size() - 4U};
    MemoryIo short_io(file);
    error.clear();
    require(!nano_lance::decode_lance_physical_column(short_io, field("s", "string"), column, none, error),
            "a buffer past the end is refused");
}

void test_inline_value() {
    const Bytes value = {42, 0, 0, 0, 0, 0, 0, 0};
    nano_lance::descriptor::Constant c;
    c.layers = {1};
    c.inline_value = &value;
    nano_lance::pb::ColumnMetadata column;
    nano_lance::pb::ColumnPage page;
    page.length = 3;
    page.encoding = nano_lance::descriptor::page_encoding(c);
    column.pages.push_back(page);

    MemoryIo io(Bytes{});
    nano_lance::ColumnValues out;
    std::string error;
    require(nano_lance::decode_lance_physical_column(io, field("n", "int64"), column, out, error), "decode: " + error);
    require(out.kind == nano_lance::ColumnValues::Kind::FixedWidth && out.fixed.size() == 24U,
            "three int64 rows");
    for (std::size_t i = 0; i < 3U; ++i) {
        std::int64_t v = 0;
        std::memcpy(&v, out.fixed.data() + i * 8U, 8U);
        require(v == 42, "row " + std::to_string(i) + " is 42");
    }
    require(io.reads.empty(), "an inline constant reads nothing");
    std::uint64_t size = 1;
    std::int64_t version = 0;
    require(io.identity(size, version) && size == 0U && version == 1 && io.name() == "memory://test", "identity");
}

}  // namespace

int main() {
    test_value_in_a_buffer();
    test_inline_value();
    std::cout << "encodings_io: ok\n";
    return 0;
}
