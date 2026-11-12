// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// list<dictionary<uint8, utf8>> is read, but the writer writes a dictionary inside a struct or list only with
// int32 indices. It must refuse this one by name at write_batch, not write it unverified.
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto ds = std::filesystem::temp_directory_path() / "nano_lance_nested_dictionary_write_refused";
    std::error_code ec;
    std::filesystem::remove_all(ds, ec);

    // One row: a list of three items, indexed into the dictionary ["a", "bb"] (uint8 indices).
    const std::uint8_t indices[] = {0, 1, 0};
    const std::int32_t list_offsets[] = {0, 3};
    const std::int32_t dict_offsets[] = {0, 1, 3};
    const char dict_data[] = "abb";

    ArrowArray dict{};
    dict.length = 2;
    dict.n_buffers = 3;
    const void* dict_buffers[] = {nullptr, dict_offsets, dict_data};
    dict.buffers = dict_buffers;

    ArrowArray leaf{};
    leaf.length = 3;
    leaf.n_buffers = 2;
    const void* leaf_buffers[] = {nullptr, indices};
    leaf.buffers = leaf_buffers;
    leaf.dictionary = &dict;

    ArrowArray list{};
    list.length = 1;
    list.n_buffers = 2;
    const void* list_buffers[] = {nullptr, list_offsets};
    list.buffers = list_buffers;
    list.n_children = 1;
    ArrowArray* list_children[] = {&leaf};
    list.children = list_children;
    // The record batch: a struct whose one column is the list.
    ArrowArray* batch_children[] = {&list};
    ArrowArray batch{};
    batch.length = 1;
    batch.n_buffers = 1;
    const void* batch_buffers[] = {nullptr};
    batch.buffers = batch_buffers;
    batch.n_children = 1;
    batch.children = batch_children;

    ArrowSchema dict_schema{};
    dict_schema.format = "u";
    dict_schema.name = "values";
    ArrowSchema leaf_schema{};
    leaf_schema.format = "C";
    leaf_schema.name = "item";
    leaf_schema.dictionary = &dict_schema;
    ArrowSchema* list_schema_children[] = {&leaf_schema};
    ArrowSchema field{};
    field.format = "+l";
    field.name = "tags";
    field.n_children = 1;
    field.children = list_schema_children;
    ArrowSchema* record_children[] = {&field};
    ArrowSchema record{};
    record.format = "+s";
    record.name = "";
    record.n_children = 1;
    record.children = record_children;

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    const int rc = nano_lance_write_batch(&writer, &batch, &record);
    require(rc == NANO_LANCE_UNSUPPORTED, "a nested dictionary with uint8 indices must be refused at write");
    const std::string err = nano_lance_writer_last_error(&writer);
    require(err.find("written only with int32 indices") != std::string::npos,
            ("refusal must name the index type, got: " + err).c_str());
    nano_lance_writer_close(&writer);
    std::filesystem::remove_all(ds, ec);
    std::cerr << "a nested dictionary with non-int32 indices is refused at write\n";
    return 0;
}
