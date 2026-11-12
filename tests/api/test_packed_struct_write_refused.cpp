// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A struct marked `lance-encoding:packed` is read as one packed column (Lance 2.1+), but this writer writes its
// children as ordinary columns. The marker must be refused by name at write_batch rather than written over them.
// The same struct without the marker is the control: it is written.
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

/// Arrow's field metadata encoding: an int32 count, then (int32 key length, key, int32 value length, value) pairs.
std::vector<std::uint8_t> metadata_bytes(const std::string& key, const std::string& value) {
    std::vector<std::uint8_t> out;
    auto put32 = [&out](std::int32_t v) {
        std::uint8_t b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    };
    put32(1);
    put32(static_cast<std::int32_t>(key.size()));
    out.insert(out.end(), key.begin(), key.end());
    put32(static_cast<std::int32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

/// Writes one batch of { s: struct<a: uint64, b: uint32> } (two rows), with `s`'s metadata `meta` (may be null).
int write_struct_batch(const std::filesystem::path& ds, const std::vector<std::uint8_t>* meta, std::string& err) {
    const std::uint64_t a_values[] = {1, 2};
    const std::uint32_t b_values[] = {3, 4};

    ArrowArray a{};
    a.length = 2;
    a.n_buffers = 2;
    const void* a_buffers[] = {nullptr, a_values};
    a.buffers = a_buffers;
    ArrowArray b{};
    b.length = 2;
    b.n_buffers = 2;
    const void* b_buffers[] = {nullptr, b_values};
    b.buffers = b_buffers;
    ArrowArray* s_children[] = {&a, &b};
    ArrowArray s{};
    s.length = 2;
    s.n_buffers = 1;
    const void* s_buffers[] = {nullptr};
    s.buffers = s_buffers;
    s.n_children = 2;
    s.children = s_children;
    ArrowArray* batch_children[] = {&s};
    ArrowArray batch{};
    batch.length = 2;
    batch.n_buffers = 1;
    const void* batch_buffers[] = {nullptr};
    batch.buffers = batch_buffers;
    batch.n_children = 1;
    batch.children = batch_children;

    ArrowSchema a_schema{};
    a_schema.format = "L";
    a_schema.name = "a";
    ArrowSchema b_schema{};
    b_schema.format = "I";
    b_schema.name = "b";
    ArrowSchema* s_schema_children[] = {&a_schema, &b_schema};
    ArrowSchema s_schema{};
    s_schema.format = "+s";
    s_schema.name = "s";
    s_schema.n_children = 2;
    s_schema.children = s_schema_children;
    s_schema.metadata = meta != nullptr ? reinterpret_cast<const char*>(meta->data()) : nullptr;
    ArrowSchema* top_children[] = {&s_schema};
    ArrowSchema top{};
    top.format = "+s";
    top.name = "";
    top.n_children = 1;
    top.children = top_children;

    NanoLanceWriter writer{};
    if (nano_lance_writer_init(&writer, ds.string().c_str(), 3) != NANO_LANCE_OK) {
        err = "init";
        return NANO_LANCE_IO_ERROR;
    }
    const int rc = nano_lance_write_batch(&writer, &batch, &top);
    err = nano_lance_writer_last_error(&writer);
    nano_lance_writer_close(&writer);
    return rc;
}

}  // namespace

int main() {
    std::error_code ec;
    const auto refused = std::filesystem::temp_directory_path() / "nano_lance_packed_struct_write_refused";
    std::filesystem::remove_all(refused, ec);
    const auto control = std::filesystem::temp_directory_path() / "nano_lance_packed_struct_write_control";
    std::filesystem::remove_all(control, ec);

    // The marker on a struct: refused by name, before a file is started.
    const auto marked = metadata_bytes("lance-encoding:packed", "true");
    std::string err;
    const int rc = write_struct_batch(refused, &marked, err);
    require(rc == NANO_LANCE_UNSUPPORTED, "a packed-marked struct must be refused at write");
    require(err.find("a packed struct is read but not written yet") != std::string::npos,
            ("the refusal must name the shape, got: " + err).c_str());
    std::filesystem::remove_all(refused, ec);

    // Control: the same struct, without the marker, is written.
    std::string ignored;
    require(write_struct_batch(control, nullptr, ignored) == NANO_LANCE_OK, "an unmarked struct is written");
    std::filesystem::remove_all(control, ec);

    std::cerr << "a packed struct is refused at write, and an unmarked one is written\n";
    return 0;
}
