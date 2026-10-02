// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// End-to-end test for the opt-in blob v2 URI-dictionary mode (nanolance extension).
// Writes the same external-ref data twice — dictionary mode on and off — and checks that:
//   1. both round-trip the per-row URIs identically through the reader, and
//   2. the dictionary-mode data file is smaller when URIs repeat.
#include "nanolance/blob_builder.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& msg) {
    require(ok, msg.c_str());
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_blob_dict_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

std::uintmax_t data_dir_bytes(const std::filesystem::path& ds) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ds / "data", ec)) {
        if (e.is_regular_file() && e.path().extension() == ".lance") {
            total += std::filesystem::file_size(e.path(), ec);
        }
    }
    return total;
}

// Extract a utf8 ArrowArray into a vector<string>.
std::vector<std::string> read_strings(const ArrowArray& utf8) {
    std::vector<std::string> out;
    const auto* offsets = static_cast<const std::int32_t*>(utf8.buffers[1]);
    const auto* data = static_cast<const char*>(utf8.buffers[2]);
    for (std::int64_t i = 0; i < utf8.length; ++i) {
        out.emplace_back(data + offsets[i], static_cast<std::size_t>(offsets[i + 1] - offsets[i]));
    }
    return out;
}

// Find a struct child array by its schema name.
const ArrowArray* child_by_name(const ArrowSchema& schema, const ArrowArray& array, const char* name) {
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name != nullptr && std::string(schema.children[i]->name) == name) {
            return array.children[i];
        }
    }
    return nullptr;
}

void write_dataset(const std::filesystem::path& ds, bool dictionary_mode,
                   const std::vector<std::string>& uris) {
    ArrowSchema schema{};
    std::string error;
    require(nano_lance::build_epb_table_schema(schema, error), error);

    std::vector<std::uint64_t> packet_ids;
    std::vector<nano_lance::BlobV2Row> rows;
    for (std::size_t i = 0; i < uris.size(); ++i) {
        packet_ids.push_back(i);
        rows.push_back({std::nullopt, uris[i], i * 1500, 1500});
    }
    ArrowArray batch{};
    require(nano_lance::build_epb_table_array(packet_ids, rows, batch, error), error);

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 0) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_ignore_nullability(&writer, true) == NANO_LANCE_OK, "ignore nullability");
    require(nano_lance_writer_set_blob_uri_dictionary(&writer, dictionary_mode) == NANO_LANCE_OK, "set dict");
    if (nano_lance_write_batch(&writer, &batch, &schema) != NANO_LANCE_OK) {
        std::cerr << "write error: " << nano_lance_writer_last_error(&writer) << '\n';
        require(false, "write");
    }
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");

    batch.release(&batch);
    schema.release(&schema);
}

std::vector<std::string> read_uris(const std::filesystem::path& ds) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error);
    ArrowSchema* payload_schema = nullptr;
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (std::string(schema.children[i]->name) == "payload_ref") {
            payload_schema = schema.children[i];
        }
    }
    require(payload_schema != nullptr, "payload_ref schema missing");
    std::vector<std::string> uris;
    for (auto& batch : batches) {  // several with a parallel read: one per row range
        // Root struct -> payload_ref struct -> uri utf8 child.
        const ArrowArray* payload = child_by_name(schema, batch, "payload_ref");
        require(payload != nullptr, "payload_ref column missing");
        const ArrowArray* uri = child_by_name(*payload_schema, *payload, "uri");
        require(uri != nullptr, "uri child missing");
        const auto part = read_strings(*uri);
        uris.insert(uris.end(), part.begin(), part.end());
        ArrowArrayRelease(&batch);
    }
    ArrowSchemaRelease(&schema);
    return uris;
}

}  // namespace

int main() {
    // Many rows, mostly one shared URI -> dictionary should help and round-trip cleanly.
    const std::string a = "s3://my-bucket/train/capture_0001.pcapng";
    const std::string b = "s3://my-bucket/train/capture_0002.pcapng";
    std::vector<std::string> uris;
    for (int i = 0; i < 200; ++i) {
        uris.push_back((i % 50 == 49) ? b : a);  // 196x a, 4x b -> 2 distinct
    }

    const auto ds_dict = temp_dataset("dict");
    const auto ds_plain = temp_dataset("plain");
    write_dataset(ds_dict, /*dictionary_mode=*/true, uris);
    write_dataset(ds_plain, /*dictionary_mode=*/false, uris);

    const auto dict_uris = read_uris(ds_dict);
    const auto plain_uris = read_uris(ds_plain);
    require(dict_uris == uris, "dictionary-mode URIs must round-trip exactly");
    require(plain_uris == uris, "plain-mode URIs must round-trip exactly");

    const auto dict_bytes = data_dir_bytes(ds_dict);
    const auto plain_bytes = data_dir_bytes(ds_plain);
    std::cerr << "blob uri dictionary: dict=" << dict_bytes << "B plain=" << plain_bytes << "B\n";
    require(dict_bytes < plain_bytes, "dictionary mode must produce a smaller data file when URIs repeat");

    std::error_code ec;
    std::filesystem::remove_all(ds_dict, ec);
    std::filesystem::remove_all(ds_plain, ec);
    return 0;
}
