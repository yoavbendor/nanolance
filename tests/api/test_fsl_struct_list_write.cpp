// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A list, large list or map under a fixed_size_list of structs, as pylance 12 writes it (format 2.2): written, and read
// back by nanolance with the struct items, their ids and the first item's one element intact.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

ArrowSchema schema_of(const char* format, const char* name, std::int64_t children, ArrowSchema** kids,
                      std::int64_t flags) {
    ArrowSchema s{};
    s.format = format;
    s.name = name;
    s.flags = flags;
    s.n_children = children;
    s.children = kids;
    return s;
}

/// Writes one batch of one row, FixedSizeList(2 x Struct(id int32, props)), where props is a map or a list, with the
/// first item holding one element (5) and the second none. Returns the writer's status, with its error.
int write_one_row(const std::string& path, bool map, std::string& error) {
    const std::int32_t ids[] = {1, 2};
    const std::int32_t offsets[] = {0, 1, 1};  // two items: the first holds one element, the second none
    const std::int32_t element[] = {5};
    const char key_bytes[] = "k";
    const std::int32_t key_offsets[] = {0, 1};

    // props' child: a map's entries (key utf8, value int32), or a list's items (int32).
    ArrowArray key{};
    key.length = 1;
    key.n_buffers = 3;
    const void* key_buffers[] = {nullptr, key_offsets, key_bytes};
    key.buffers = key_buffers;
    ArrowArray value{};
    value.length = 1;
    value.n_buffers = 2;
    const void* value_buffers[] = {nullptr, element};
    value.buffers = value_buffers;
    ArrowArray* entry_children[] = {&key, &value};
    ArrowArray entries{};
    entries.length = 1;
    entries.n_buffers = 1;
    const void* entries_buffers[] = {nullptr};
    entries.buffers = entries_buffers;
    entries.n_children = 2;
    entries.children = entry_children;
    ArrowArray item_leaf{};
    item_leaf.length = 1;
    item_leaf.n_buffers = 2;
    const void* item_buffers[] = {nullptr, element};
    item_leaf.buffers = item_buffers;

    ArrowArray* props_children[] = {map ? &entries : &item_leaf};
    ArrowArray props{};
    props.length = 2;
    props.n_buffers = 2;
    const void* props_buffers[] = {nullptr, offsets};
    props.buffers = props_buffers;
    props.n_children = 1;
    props.children = props_children;

    ArrowArray id{};
    id.length = 2;
    id.n_buffers = 2;
    const void* id_buffers[] = {nullptr, ids};
    id.buffers = id_buffers;

    ArrowArray* item_struct_children[] = {&id, &props};
    ArrowArray item{};
    item.length = 2;
    item.n_buffers = 1;
    const void* item_struct_buffers[] = {nullptr};
    item.buffers = item_struct_buffers;
    item.n_children = 2;
    item.children = item_struct_children;

    ArrowArray* fsl_children[] = {&item};
    ArrowArray fsl{};
    fsl.length = 1;
    fsl.n_buffers = 1;
    const void* fsl_buffers[] = {nullptr};
    fsl.buffers = fsl_buffers;
    fsl.n_children = 1;
    fsl.children = fsl_children;

    ArrowArray* batch_children[] = {&fsl};
    ArrowArray batch{};
    batch.length = 1;
    batch.n_buffers = 1;
    const void* batch_buffers[] = {nullptr};
    batch.buffers = batch_buffers;
    batch.n_children = 1;
    batch.children = batch_children;

    // The schema of the same shape.
    ArrowSchema key_s = schema_of("u", "key", 0, nullptr, 0);
    ArrowSchema value_s = schema_of("i", "value", 0, nullptr, ARROW_FLAG_NULLABLE);
    ArrowSchema* entries_schema_children[] = {&key_s, &value_s};
    ArrowSchema entries_s = schema_of("+s", "entries", 2, entries_schema_children, 0);
    ArrowSchema item_s_leaf = schema_of("i", "item", 0, nullptr, ARROW_FLAG_NULLABLE);
    ArrowSchema* map_children[] = {&entries_s};
    ArrowSchema* list_children[] = {&item_s_leaf};
    ArrowSchema props_s = map ? schema_of("+m", "props", 1, map_children, ARROW_FLAG_NULLABLE)
                              : schema_of("+l", "props", 1, list_children, ARROW_FLAG_NULLABLE);
    ArrowSchema id_s = schema_of("i", "id", 0, nullptr, ARROW_FLAG_NULLABLE);
    ArrowSchema* item_schema_children[] = {&id_s, &props_s};
    ArrowSchema item_s = schema_of("+s", "item", 2, item_schema_children, ARROW_FLAG_NULLABLE);
    ArrowSchema* fsl_schema_children[] = {&item_s};
    ArrowSchema field = schema_of("+w:2", "f", 1, fsl_schema_children, ARROW_FLAG_NULLABLE);
    ArrowSchema* top_children[] = {&field};
    ArrowSchema top = schema_of("+s", "", 1, top_children, 0);

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, path.c_str(), 3) == NANO_LANCE_OK, "init");
    const int rc = nano_lance_write_batch(&writer, &batch, &top);
    error = nano_lance_writer_last_error(&writer);
    if (rc == NANO_LANCE_OK) {
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    }
    nano_lance_writer_close(&writer);
    return rc;
}

/// Reads the one row back: the two items' ids, and the first item's one element (5, a list's item or a map's value).
void check_read_back(const std::string& path, bool map) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(path, schema, batches, error), "read: " + error);
    std::int64_t rows = 0;
    for (auto& b : batches) {
        require(b.n_children == 1, "one column");
        const ArrowArray& fsl = *b.children[0];
        require(fsl.length == 1 && fsl.null_count == 0, "one valid row");
        const ArrowArray& item = *fsl.children[0];
        require(item.length == 2, "two struct items");
        const ArrowArray& ids_array = *item.children[0];
        const auto* ids = static_cast<const std::int32_t*>(ids_array.buffers[1]);
        require(ids[ids_array.offset] == 1 && ids[ids_array.offset + 1] == 2, "ids 1 and 2");
        const ArrowArray& props = *item.children[1];
        const ArrowArray& entries = *props.children[0];
        const ArrowArray& leaf = map ? *entries.children[1] : entries;
        require(leaf.length == 1, "one element in the first item");
        const auto* values = static_cast<const std::int32_t*>(leaf.buffers[1]);
        require(values[leaf.offset] == 5, "the element is 5");
        rows += fsl.length;
    }
    require(rows == 1, "one row read back");
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path();
    for (const bool map : {false, true}) {
        const std::string name = map ? "map" : "list";
        const std::string path = (dir / ("nano_lance_fsl_struct_" + name + "_write")).string();
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::string error;
        const int rc = write_one_row(path, map, error);
        require(rc == NANO_LANCE_OK,
                name + " under a fixed_size_list must be written, got rc " + std::to_string(rc) + ": " + error);
        check_read_back(path, map);
        std::filesystem::remove_all(path, ec);
    }
    std::cerr << "a list or map under a fixed_size_list of structs is written and reads back\n";
    return 0;
}
