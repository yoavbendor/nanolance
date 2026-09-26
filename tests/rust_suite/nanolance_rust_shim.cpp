// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// What lance-encoding's tests call to read their round trips back with nanolance (see
// nanolance_hook.rs and tools/rust_suite.py). Built only for that run, as a shared library.
//
// nanolance_rust_write_file wraps the Rust encoder's output -- its data bytes, positioned as the start
// of a file, and its page tables -- into a Lance 2.2 file: the schema (a file descriptor), each
// column's metadata, and the footer, laid out as Lance's file writer lays them out.
// nanolance_rust_read then reads that file as a nanolance user would.

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/schema_mapper.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void set_message(char* msg, std::size_t cap, const std::string& text) {
    if (msg == nullptr || cap == 0U) {
        return;
    }
    const auto n = std::min(cap - 1U, text.size());
    std::memcpy(msg, text.data(), n);
    msg[n] = '\0';
}

struct Cursor {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool ok = true;

    template <typename T>
    T get() {
        T v{};
        if (static_cast<std::size_t>(end - p) < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::vector<std::uint8_t> bytes() {
        const auto n = get<std::uint32_t>();
        if (!ok || static_cast<std::size_t>(end - p) < n) {
            ok = false;
            return {};
        }
        std::vector<std::uint8_t> out(p, p + n);
        p += n;
        return out;
    }
};

void write_le(std::ofstream& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

void align64(std::ofstream& out) {
    const auto pos = static_cast<std::uint64_t>(out.tellp());
    for (auto i = pos; i % 64U != 0U; ++i) {
        out.put('\0');
    }
}

}  // namespace

extern "C" {

int nanolance_rust_write_file(const char* path, const std::uint8_t* data, std::size_t data_len,
                              const std::uint8_t* columns, std::size_t columns_len, const ArrowSchema* schema,
                              std::uint64_t num_rows, char* msg, std::size_t msg_cap) {
    // The field, named if the test left it unnamed (a name is all it changes).
    ArrowSchema copy{};
    if (ArrowSchemaDeepCopy(schema, &copy) != NANOARROW_OK) {
        set_message(msg, msg_cap, "cannot copy the schema");
        return 1;
    }
    if (copy.n_children == 1 && (copy.children[0]->name == nullptr || copy.children[0]->name[0] == '\0')) {
        ArrowSchemaSetName(copy.children[0], "c");
    }
    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    const bool mapped = nano_lance::map_arrow_schema(copy, mapping, error);
    ArrowSchemaRelease(&copy);
    if (!mapped) {
        set_message(msg, msg_cap, "schema: " + error);
        return 1;
    }

    Cursor in{columns, columns + columns_len};
    std::vector<nano_lance::pb::ColumnMetadata> metadata(in.get<std::uint32_t>());
    for (auto& column : metadata) {
        const auto pages = in.get<std::uint32_t>();
        for (std::uint32_t p = 0; in.ok && p < pages; ++p) {
            nano_lance::pb::ColumnPage page;
            page.length = in.get<std::uint64_t>();
            page.priority = in.get<std::uint64_t>();
            const auto buffers = in.get<std::uint32_t>();
            for (std::uint32_t b = 0; in.ok && b < buffers; ++b) {
                page.buffer_offsets.push_back(in.get<std::uint64_t>());
                page.buffer_sizes.push_back(in.get<std::uint64_t>());
            }
            page.encoding = in.bytes();
            column.pages.push_back(std::move(page));
        }
        if (in.get<std::uint32_t>() != 0U) {
            set_message(msg, msg_cap, "column-level buffers (not written by 2.1+ files)");
            return 1;
        }
        column.encoding = in.bytes();
        if (!in.ok) {
            break;
        }
    }
    if (!in.ok) {
        set_message(msg, msg_cap, "malformed column framing");
        return 2;
    }

    nano_lance::pb::FileDescriptor descriptor;
    descriptor.length = num_rows;
    for (const auto& f : mapping.fields) {
        nano_lance::pb::Field field;
        field.name = f.name;
        field.logical_type = nano_lance::lance_on_disk_logical_type(f.logical_type);
        field.id = f.id;
        field.parent_id = f.parent_id;
        field.type = 2;
        field.nullable = f.nullable;
        field.encoding = nano_lance::lance_on_disk_field_encoding(f.logical_type);
        for (const auto& kv : f.metadata) {
            field.metadata[kv.first] = std::vector<std::uint8_t>(kv.second.begin(), kv.second.end());
        }
        descriptor.fields.push_back(std::move(field));
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(data_len));
    const auto descriptor_bytes = nano_lance::pb::encode_file_descriptor(descriptor);
    align64(out);
    const auto global_buffer = static_cast<std::uint64_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(descriptor_bytes.data()),
              static_cast<std::streamsize>(descriptor_bytes.size()));
    align64(out);
    const auto column_metadata_start = static_cast<std::uint64_t>(out.tellp());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> positions;
    for (const auto& column : metadata) {
        const auto encoded = nano_lance::pb::encode_column_metadata(column);
        positions.emplace_back(static_cast<std::uint64_t>(out.tellp()), encoded.size());
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }
    align64(out);
    const auto column_offsets = static_cast<std::uint64_t>(out.tellp());
    for (const auto& [offset, size] : positions) {
        write_le(out, offset, 8);
        write_le(out, size, 8);
    }
    const auto global_offsets = static_cast<std::uint64_t>(out.tellp());
    write_le(out, global_buffer, 8);
    write_le(out, descriptor_bytes.size(), 8);
    write_le(out, column_metadata_start, 8);
    write_le(out, column_offsets, 8);
    write_le(out, global_offsets, 8);
    write_le(out, 1, 4);
    write_le(out, metadata.size(), 4);
    write_le(out, 2, 2);  // major
    write_le(out, 2, 2);  // minor
    out.write("LANC", 4);
    out.close();
    if (!out) {
        set_message(msg, msg_cap, "cannot write the file");
        return 2;
    }
    return 0;
}

int nanolance_rust_read(const char* path, std::uint64_t offset, std::uint64_t length, const std::uint64_t* indices,
                        std::size_t n_indices, int take, ArrowArrayStream* out, char* msg, std::size_t msg_cap) {
    nano_lance::LanceScanRequest request;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    bool ok = false;
    if (take != 0) {
        const std::vector<std::uint64_t> rows(indices, indices + n_indices);
        ok = nano_lance::lance_file_take(path, request, rows, schema, batches, error);
    } else {
        request.range.offset = offset;
        request.range.length = length;
        ok = nano_lance::lance_file_read(path, request, schema, batches, error);
    }
    if (!ok) {
        set_message(msg, msg_cap, error.empty() ? "read failed" : error);
        return 1;
    }
    if (ArrowBasicArrayStreamInit(out, &schema, static_cast<int64_t>(batches.size())) != NANOARROW_OK) {
        for (auto& b : batches) {
            ArrowArrayRelease(&b);
        }
        ArrowSchemaRelease(&schema);
        set_message(msg, msg_cap, "cannot build the stream");
        return 2;
    }
    for (std::size_t i = 0; i < batches.size(); ++i) {
        ArrowBasicArrayStreamSetArray(out, static_cast<int64_t>(i), &batches[i]);
    }
    return 0;
}

}  // extern "C"
