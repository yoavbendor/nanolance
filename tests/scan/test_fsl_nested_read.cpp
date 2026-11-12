// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// A fixed_size_list of structs whose field is a list or a map, in format 2.2 files that pylance 12 wrote
// (tests/golden/fsl_struct_v22/{list,large,map}.lance). The shapes are FixedSizeList(2 x Struct("id": Int32,
// "values": List<Int32> | LargeList<Int32>)) and FixedSizeList(2 x Struct("id": Int32, "props": Map<Utf8, Int32>)).
// The rows come back whole, by a row range and by take, with the nulls where they are: a null fixed_size_list row,
// a null struct item, a null list or map, an empty one, and a null element inside a list or a map.

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

constexpr std::int64_t kDimension = 2;  // FixedSizeList(2 x Struct)

/// Each row as text: "null" for a null row, else "[item,item]" with an item "null" or "{id:props}". Props is
/// "null" or the list "[e,e]" (an element "null" or its integer), or the map "{k:v,k:v}" (a value "null" or its
/// integer). The expectations below are written the same way.
const std::vector<std::string> kListRows = {
    "[{1:[1,2]},{2:null}]",
    "null",
    "[null,{3:[]}]",
    "[{4:[5,null,6]},{5:[7]}]",
    "[{6:[]},null]",
    "[{7:null},{8:[9]}]",
};

const std::vector<std::string> kMapRows = {
    "[{1:{a:1,b:null}},{2:null}]",
    "null",
    "[null,{3:{}}]",
    "[{4:{c:5,d:6,e:null}},{5:{f:7}}]",
    "[{6:{}},null]",
    "[{7:null},{8:{g:9}}]",
};

/// A list or large list value at `index` of `list`, as text.
std::string list_text(const ArrowArrayView* list, std::int64_t index) {
    if (ArrowArrayViewIsNull(list, index)) {
        return "null";
    }
    const ArrowArrayView* elements = list->children[0];
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    if (list->storage_type == NANOARROW_TYPE_LARGE_LIST) {
        const auto* offsets = static_cast<const std::int64_t*>(list->buffer_views[1].data.data);
        lo = offsets[list->offset + index];
        hi = offsets[list->offset + index + 1];
    } else {
        const auto* offsets = static_cast<const std::int32_t*>(list->buffer_views[1].data.data);
        lo = offsets[list->offset + index];
        hi = offsets[list->offset + index + 1];
    }
    std::string out = "[";
    for (std::int64_t k = lo; k < hi; ++k) {
        if (k != lo) {
            out += ",";
        }
        out += ArrowArrayViewIsNull(elements, k) ? "null" : std::to_string(ArrowArrayViewGetIntUnsafe(elements, k));
    }
    return out + "]";
}

/// A map value at `index` of `map` (Utf8 keys, Int32 values), as text.
std::string map_text(const ArrowArrayView* map, std::int64_t index) {
    if (ArrowArrayViewIsNull(map, index)) {
        return "null";
    }
    const ArrowArrayView* entries = map->children[0];
    const ArrowArrayView* keys = entries->children[0];
    const ArrowArrayView* values = entries->children[1];
    const auto* offsets = static_cast<const std::int32_t*>(map->buffer_views[1].data.data);
    const std::int64_t lo = offsets[map->offset + index];
    const std::int64_t hi = offsets[map->offset + index + 1];
    std::string out = "{";
    for (std::int64_t k = lo; k < hi; ++k) {
        if (k != lo) {
            out += ",";
        }
        const auto bytes = ArrowArrayViewGetBytesUnsafe(keys, k);
        out.append(bytes.data.as_char, static_cast<std::size_t>(bytes.size_bytes));
        out += ":";
        out += ArrowArrayViewIsNull(values, k) ? "null" : std::to_string(ArrowArrayViewGetIntUnsafe(values, k));
    }
    return out + "}";
}

/// The rows of every batch, read through nanoarrow's views.
std::vector<std::string> collect(const ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    std::vector<std::string> out;
    for (auto& batch : batches) {
        ArrowArrayView view;
        ArrowError err{};
        require(ArrowArrayViewInitFromSchema(&view, &schema, &err) == NANOARROW_OK, err.message);
        require(ArrowArrayViewSetArray(&view, &batch, &err) == NANOARROW_OK, err.message);
        const ArrowArrayView* fsl = view.children[0];
        const ArrowArrayView* item = fsl->children[0];
        const ArrowArrayView* id = item->children[0];
        const ArrowArrayView* props = item->children[1];
        const bool is_map = props->storage_type == NANOARROW_TYPE_MAP;
        for (int64_t r = 0; r < batch.length; ++r) {
            if (ArrowArrayViewIsNull(fsl, r)) {
                out.emplace_back("null");
                continue;
            }
            std::string row = "[";
            for (int64_t k = 0; k < kDimension; ++k) {
                const std::int64_t at = (fsl->offset + r) * kDimension + k;
                if (k != 0) {
                    row += ",";
                }
                if (ArrowArrayViewIsNull(item, at)) {
                    row += "null";
                    continue;
                }
                const std::string value = is_map ? map_text(props, at) : list_text(props, at);
                row += "{" + std::to_string(ArrowArrayViewGetIntUnsafe(id, at)) + ":" + value + "}";
            }
            out.emplace_back(row + "]");
        }
        ArrowArrayViewReset(&view);
    }
    return out;
}

void check_rows(const std::vector<std::string>& got, const std::vector<std::string>& want, const std::string& what) {
    require(got.size() == want.size(),
            what + ": " + std::to_string(want.size()) + " rows, got " + std::to_string(got.size()));
    for (std::size_t r = 0; r < want.size(); ++r) {
        require(got[r] == want[r], what + ": row " + std::to_string(r) + " is " + got[r] + ", want " + want[r]);
    }
}

/// Read with `read`, check the Arrow type and the rows, then release what it returned.
template <typename Read>
void check_read(const std::string& what, const std::vector<std::string>& want, Read read) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    const bool ok = read(schema, batches, error);
    require(ok, what + ": " + error);
    require(schema.n_children == 1, what + ": one column");
    check_rows(collect(schema, batches), want, what);
    schema.release(&schema);
    for (auto& batch : batches) {
        batch.release(&batch);
    }
}

/// The whole table, a range of rows, and a take, from one dataset.
void check_dataset(const std::filesystem::path& dataset, const std::vector<std::string>& rows) {
    const std::string name = dataset.filename().string();
    check_read(name + " whole", rows, [&](ArrowSchema& s, std::vector<ArrowArray>& b, std::string& e) {
        return nano_lance::lance_table_read_dataset(dataset, s, b, e);
    });

    // Rows 2, 3, 4: a row with a null struct item, a row with a null element, a row with a null item.
    const std::vector<std::string> range(rows.begin() + 2, rows.begin() + 5);
    check_read(name + " rows 2..5", range, [&](ArrowSchema& s, std::vector<ArrowArray>& b, std::string& e) {
        nano_lance::LanceRowRange window;
        window.offset = 2;
        window.length = 3;
        return nano_lance::lance_table_read_dataset_range(dataset, nullptr, window, s, b, e);
    });

    const std::vector<std::string> taken = {rows[1], rows[3], rows[5]};
    check_read(name + " take 1, 3, 5", taken, [&](ArrowSchema& s, std::vector<ArrowArray>& b, std::string& e) {
        return nano_lance::lance_table_take(dataset, nullptr, {1, 3, 5}, s, b, e);
    });
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: test_fsl_nested_read <fsl_struct_v22 directory>");
    const std::filesystem::path root = argv[1];
    check_dataset(root / "list.lance", kListRows);
    check_dataset(root / "large.lance", kListRows);
    check_dataset(root / "map.lance", kMapRows);
    std::cout << "fsl_nested_read: ok\n";
    return 0;
}
