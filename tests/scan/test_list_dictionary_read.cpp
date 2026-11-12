// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// A list of dictionary items (list<dictionary<uint8, large_utf8>>) in a format 2.0 file that pylance 12 wrote
// (tests/golden/list_dictionary_v20/dataset.lance, from the rows below). The item column is read as Arrow's
// dictionary type, as a top-level dictionary is, and the rows come back whole, by a row range and by take,
// with nulls where they are: a null list, a null item, an empty list.

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Item = std::optional<std::string>;
using Row = std::optional<std::vector<Item>>;  // nullopt: a null list

/// The fixture's tags, row by row: 0..8.
const std::vector<Row> kWhole = {
    std::vector<Item>{"a", "bb", "ccc"}, std::vector<Item>{}, std::nullopt,
    std::vector<Item>{"ccc", std::nullopt}, std::vector<Item>{"bb"}, std::vector<Item>{"a"},
    std::nullopt, std::vector<Item>{"ccc", "a", "bb"}, std::vector<Item>{"bb", "bb"},
};

void check_type(const ArrowSchema& schema) {
    require(schema.n_children == 2, "two columns: id and tags");
    const ArrowSchema& tags = *schema.children[1];
    require(std::string(tags.name) == "tags" && std::string(tags.format) == "+l", "tags is a list");
    require(tags.n_children == 1 && tags.children[0] != nullptr, "tags has an item");
    const ArrowSchema& item = *tags.children[0];
    require(std::string(item.format) == "C", "items are indexed by uint8: " + std::string(item.format));
    require(item.dictionary != nullptr && std::string(item.dictionary->format) == "U",
            "the items' dictionary is large_utf8");
}

/// The rows of every batch, read through nanoarrow's views.
std::vector<Row> collect(const ArrowSchema& schema, std::vector<ArrowArray>& batches) {
    std::vector<Row> out;
    for (auto& batch : batches) {
        ArrowArrayView view;
        ArrowError err{};
        require(ArrowArrayViewInitFromSchema(&view, &schema, &err) == NANOARROW_OK, err.message);
        require(ArrowArrayViewSetArray(&view, &batch, &err) == NANOARROW_OK, err.message);
        const ArrowArrayView* tags = view.children[1];
        const ArrowArrayView* item = tags->children[0];
        const ArrowArrayView* values = item->dictionary;
        require(values != nullptr, "the items carry their dictionary");
        for (int64_t r = 0; r < batch.length; ++r) {
            if (ArrowArrayViewIsNull(tags, r)) {
                out.emplace_back(std::nullopt);
                continue;
            }
            const auto* offsets = static_cast<const int32_t*>(tags->buffer_views[1].data.data);
            const int64_t lo = offsets[tags->offset + r];
            const int64_t hi = offsets[tags->offset + r + 1];
            std::vector<Item> row;
            for (int64_t k = lo; k < hi; ++k) {
                if (ArrowArrayViewIsNull(item, k)) {
                    row.emplace_back(std::nullopt);
                    continue;
                }
                const auto index = ArrowArrayViewGetIntUnsafe(item, k);
                const auto bytes = ArrowArrayViewGetBytesUnsafe(values, index);
                row.emplace_back(std::string(bytes.data.as_char, static_cast<std::size_t>(bytes.size_bytes)));
            }
            out.emplace_back(std::move(row));
        }
        ArrowArrayViewReset(&view);
    }
    return out;
}

void check_rows(const std::vector<Row>& got, const std::vector<Row>& want, const std::string& what) {
    require(got.size() == want.size(), what + ": " + std::to_string(want.size()) + " rows, got " +
                                           std::to_string(got.size()));
    for (std::size_t r = 0; r < want.size(); ++r) {
        require(got[r].has_value() == want[r].has_value(), what + ": row " + std::to_string(r) + " null");
        if (!want[r]) {
            continue;
        }
        require(got[r]->size() == want[r]->size(), what + ": row " + std::to_string(r) + " length");
        for (std::size_t k = 0; k < want[r]->size(); ++k) {
            require((*got[r])[k] == (*want[r])[k], what + ": row " + std::to_string(r) + " item " +
                                                       std::to_string(k));
        }
    }
}

/// Read with `read`, check the Arrow type and the rows, then release what it returned.
template <typename Read>
void check_read(const std::string& what, const std::vector<Row>& want, Read read) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    const bool ok = read(schema, batches, error);
    require(ok, what + ": " + error);
    check_type(schema);
    check_rows(collect(schema, batches), want, what);
    schema.release(&schema);
    for (auto& batch : batches) {
        batch.release(&batch);
    }
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: test_list_dictionary_read <dataset.lance>");
    const std::filesystem::path dataset = argv[1];

    check_read("whole dataset", kWhole, [&](ArrowSchema& s, std::vector<ArrowArray>& b, std::string& e) {
        return nano_lance::lance_table_read_dataset(dataset, s, b, e);
    });

    // Rows 2, 3, 4: a null list, a list with a null item, and a one-item list.
    const std::vector<Row> range(kWhole.begin() + 2, kWhole.begin() + 5);
    check_read("rows 2..5", range, [&](ArrowSchema& s, std::vector<ArrowArray>& b, std::string& e) {
        nano_lance::LanceRowRange rows;
        rows.offset = 2;
        rows.length = 3;
        return nano_lance::lance_table_read_dataset_range(dataset, nullptr, rows, s, b, e);
    });

    const std::vector<Row> taken = {kWhole[0], kWhole[4], kWhole[8]};
    check_read("take 0, 4, 8", taken, [&](ArrowSchema& s, std::vector<ArrowArray>& b, std::string& e) {
        return nano_lance::lance_table_take(dataset, nullptr, {0, 4, 8}, s, b, e);
    });

    std::cout << "list_dictionary_read: ok\n";
    return 0;
}
