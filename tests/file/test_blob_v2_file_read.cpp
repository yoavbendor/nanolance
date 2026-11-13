// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A blob v2 column in a standalone Lance 2.2 data file, as Lance writes it: a struct marked `lance-encoding:blob`
// whose children (kind, position, size, blob_id, blob_uri) have no ids of their own. The file is one column, so
// the reader reads it as the blob it is, and as Lance describes it (BlobHandling::Descriptions). The golden file in
// tests/golden/blob_v2_file is written by tests/generate_golden_blob_v2_file.py, with the values its docstring gives:
// an inline row, a null row, an external row (kind 3, position 100, size 64) and a second inline row.
// Each inline row's bytes are at its position in the file. Read whole and by a take. Rust is not needed.

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifndef NANO_LANCE_BLOB_V2_FILE_GOLDEN
#error "NANO_LANCE_BLOB_V2_FILE_GOLDEN must name tests/golden/blob_v2_file/v22_blob_v2.lance"
#endif

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

const std::string kInlineA = "hello blob v2";
const std::string kInlineB(300, 'x');
const std::string kExternalUri = "s3://lance-golden/capture.bin";

bool bit(const void* validity, std::int64_t at) {
    if (validity == nullptr) {
        return true;
    }
    const auto* bits = static_cast<const std::uint8_t*>(validity);
    return ((bits[at >> 3] >> (at & 7)) & 1U) != 0U;
}

int child_index(const ArrowSchema& schema, const std::string& name) {
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (name == schema.children[i]->name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// One row of the blob column as its descriptor.
struct Row {
    bool valid = false;
    std::uint8_t kind = 0;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
    std::uint32_t blob_id = 0;
    std::string uri;
};

/// The rows of one read, the blob column's descriptors, in order.
std::vector<Row> read_rows(const ArrowSchema& schema, const std::vector<ArrowArray>& batches) {
    std::vector<Row> rows;
    const int blob_at = child_index(schema, "payload");
    require(blob_at >= 0, "the payload column is read");
    const ArrowSchema& blob = *schema.children[blob_at];
    require(blob.n_children == 5, "payload has five children: kind, position, size, blob_id, blob_uri");
    const int kind_at = child_index(blob, "kind");
    const int position_at = child_index(blob, "position");
    const int size_at = child_index(blob, "size");
    const int id_at = child_index(blob, "blob_id");
    const int uri_at = child_index(blob, "blob_uri");
    require(kind_at >= 0 && position_at >= 0 && size_at >= 0 && id_at >= 0 && uri_at >= 0,
            "payload's children are the five descriptor fields");
    require(std::string(blob.children[kind_at]->format) == "C", "kind is uint8");
    require(std::string(blob.children[position_at]->format) == "L", "position is uint64");
    require(std::string(blob.children[size_at]->format) == "L", "size is uint64");
    require(std::string(blob.children[id_at]->format) == "I", "blob_id is uint32");
    require(std::string(blob.children[uri_at]->format) == "u", "blob_uri is utf8");

    for (const auto& batch : batches) {
        const ArrowArray& column = *batch.children[blob_at];
        const ArrowArray& kind = *column.children[kind_at];
        const ArrowArray& position = *column.children[position_at];
        const ArrowArray& size = *column.children[size_at];
        const ArrowArray& blob_id = *column.children[id_at];
        const ArrowArray& uri = *column.children[uri_at];
        const auto* offsets = static_cast<const std::int32_t*>(uri.buffers[1]);
        const auto* text = static_cast<const char*>(uri.buffers[2]);
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t at = column.offset + r;
            Row row;
            row.valid = bit(column.buffers[0], at);
            if (!row.valid) {
                rows.push_back(row);
                continue;
            }
            const std::int64_t k = kind.offset + r;
            row.valid = true;
            row.kind = static_cast<const std::uint8_t*>(kind.buffers[1])[k];
            row.position = static_cast<const std::uint64_t*>(position.buffers[1])[position.offset + r];
            row.size = static_cast<const std::uint64_t*>(size.buffers[1])[size.offset + r];
            row.blob_id = static_cast<const std::uint32_t*>(blob_id.buffers[1])[blob_id.offset + r];
            const std::int64_t u = uri.offset + r;
            row.uri.assign(text + offsets[u], static_cast<std::size_t>(offsets[u + 1] - offsets[u]));
            rows.push_back(row);
        }
    }
    return rows;
}

void release(ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    for (auto& batch : batches) {
        if (batch.release != nullptr) {
            ArrowArrayRelease(&batch);
        }
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
}

std::string file_bytes(const std::filesystem::path& path, std::uint64_t at, std::uint64_t size) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "the golden file opens");
    in.seekg(static_cast<std::streamoff>(at));
    std::string out(static_cast<std::size_t>(size), '\0');
    in.read(out.data(), static_cast<std::streamsize>(size));
    require(in.gcount() == static_cast<std::streamsize>(size), "the inline bytes are inside the file");
    return out;
}

/// Row `i` of the file is what the script wrote; `r` is its position in the read.
void check_row(const Row& row, std::uint64_t i, const std::filesystem::path& path, const std::string& what) {
    const std::string where = what + ": row " + std::to_string(i);
    switch (i) {
        case 0:
            require(row.valid && row.kind == 0 && row.size == kInlineA.size() && row.uri.empty(), where + " is inline");
            require(file_bytes(path, row.position, row.size) == kInlineA, where + " has the wrong bytes");
            break;
        case 1:
            require(!row.valid, where + " is null");
            break;
        case 2:
            require(row.valid && row.kind == 3 && row.position == 100 && row.size == 64 && row.uri == kExternalUri,
                    where + " is external at the URI with position 100 and size 64");
            break;
        case 3:
            require(row.valid && row.kind == 0 && row.size == kInlineB.size() && row.uri.empty(), where + " is inline");
            require(file_bytes(path, row.position, row.size) == kInlineB, where + " has the wrong bytes");
            break;
        default:
            require(false, where + " is past the file's rows");
    }
}

void check_file(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    std::string error;

    // Whole: every row, in order.
    {
        nano_lance::LanceScanRequest request;
        request.blob_handling = nano_lance::BlobHandling::Descriptions;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_read(path, request, schema, batches, error), name + " whole: " + error);
        const auto rows = read_rows(schema, batches);
        require(rows.size() == 4, name + " whole: " + std::to_string(rows.size()) + " rows");
        for (std::uint64_t i = 0; i < 4; ++i) {
            check_row(rows[i], i, path, name + " whole");
        }
        release(schema, batches);
    }

    // A take of the null row and the external row, and of the second inline row.
    {
        nano_lance::LanceScanRequest request;
        request.blob_handling = nano_lance::BlobHandling::Descriptions;
        const std::vector<std::uint64_t> wanted = {1, 2, 3};
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        require(nano_lance::lance_file_take(path, request, wanted, schema, batches, error), name + " take: " + error);
        const auto rows = read_rows(schema, batches);
        require(rows.size() == wanted.size(), name + " take: " + std::to_string(rows.size()) + " rows");
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            check_row(rows[k], wanted[k], path, name + " take");
        }
        release(schema, batches);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path path = argc > 1 ? std::filesystem::path(argv[1])
                                                : std::filesystem::path(NANO_LANCE_BLOB_V2_FILE_GOLDEN);
    check_file(path);
    std::cout << "blob v2 data file read back: " << path.filename().string() << ", 4 rows whole and by take\n";
    return 0;
}
