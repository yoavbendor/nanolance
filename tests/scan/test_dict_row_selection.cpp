// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// A row range or a take of a dictionary-coded column decodes only the rows it asks for. Three nullable columns
// nanolance writes -- strings and int64s with few distinct values, and runs of strings (dictionary + RLE) -- are read
// back by many ranges and takes (chunk edges, empty and whole ranges, unsorted and repeated take rows), and each read
// must equal the same rows of a full read. The work counter dict_rows_expanded must count exactly the rows a read
// returned (a take's distinct rows): a dictionary page that copied out every row of the chunks it touched, or the
// whole page, fails here.
//
// The take cache is switched off for this process (NANOLANCE_TAKE_CACHE_MB=0): a small column is otherwise decoded
// whole and cached on its first take, and the chunk and whole-page take paths would go untested.

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/work_stats.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

namespace ws = nano_lance::work_stats;

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

// A column's values: nullopt is a null. An int64 is its eight bytes, so one type serves both.
using Values = std::vector<std::optional<std::string>>;

std::filesystem::path temp_dataset(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_dict_selection_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

// Writes one nullable column named "c": utf8 when `strings`, else int64.
void write_column(const std::filesystem::path& ds, bool strings, const Values& vals) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetFormat(&schema, strings ? "u" : "l") == NANOARROW_OK, "format");
    require(ArrowSchemaSetName(&schema, "c") == NANOARROW_OK, "name");
    ArrowArray array{};
    require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array init");
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "append start");
    for (const auto& v : vals) {
        if (!v.has_value()) {
            require(ArrowArrayAppendNull(&array, 1) == NANOARROW_OK, "append null");
        } else if (strings) {
            require(ArrowArrayAppendString(&array, {v->data(), static_cast<int64_t>(v->size())}) == NANOARROW_OK,
                    "append string");
        } else {
            std::int64_t x = 0;
            std::memcpy(&x, v->data(), sizeof x);
            require(ArrowArrayAppendInt(&array, x) == NANOARROW_OK, "append int");
        }
    }
    require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish");
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_ignore_nullability(&writer, true) == NANO_LANCE_OK, "ignore null");
    require(nano_lance_writer_set_compression(&writer, false) == NANO_LANCE_OK, "compress");
    require(nano_lance_write_batch(&writer, &array, &schema) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    ArrowArrayRelease(&array);
    ArrowSchemaRelease(&schema);
}

// The values of the batches an read returned, in order, for the one column.
Values collect(bool strings, std::vector<ArrowArray>& batches, ArrowSchema& schema) {
    Values out;
    for (auto& batch : batches) {  // several with a parallel read: one per row range
        const ArrowArray* col = batch.n_children > 0 ? batch.children[0] : &batch;
        for (std::int64_t i = 0; i < col->length; ++i) {
            const auto row = col->offset + i;
            const auto* validity = static_cast<const std::uint8_t*>(col->n_buffers > 0 ? col->buffers[0] : nullptr);
            if (validity != nullptr && ((validity[row >> 3] >> (row & 7)) & 1U) == 0U) {
                out.emplace_back(std::nullopt);
            } else if (strings) {
                const auto* off = static_cast<const std::int32_t*>(col->buffers[1]);
                const auto* data = static_cast<const char*>(col->buffers[2]);
                out.emplace_back(std::string(data + off[row], static_cast<std::size_t>(off[row + 1] - off[row])));
            } else {
                const auto* data = static_cast<const std::int64_t*>(col->buffers[1]);
                out.emplace_back(std::string(reinterpret_cast<const char*>(&data[row]), sizeof(std::int64_t)));
            }
        }
        ArrowArrayRelease(&batch);
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
    return out;
}

const std::vector<std::string> kColumn{"c"};

Values read_range(const std::filesystem::path& ds, bool strings, std::uint64_t first, std::uint64_t count) {
    nano_lance::LanceRowRange range;
    range.offset = first;
    range.length = count;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset_range(ds, &kColumn, range, schema, batches, error),
            "range [" + std::to_string(first) + ", +" + std::to_string(count) + "): " + error);
    return collect(strings, batches, schema);
}

Values read_take(const std::filesystem::path& ds, bool strings, const std::vector<std::uint64_t>& rows) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_take(ds, &kColumn, rows, schema, batches, error), "take: " + error);
    return collect(strings, batches, schema);
}

// Rows of `full` at `picked` (ascending, distinct), as a full read would have them.
Values pick(const Values& full, const std::vector<std::uint64_t>& picked) {
    Values out;
    for (const auto r : picked) {
        out.push_back(full.at(static_cast<std::size_t>(r)));
    }
    return out;
}

struct Dataset {
    std::string name;
    bool strings;
    std::uint64_t rows;
    Values values;
};

// The three shapes: a scattered string column, a scattered int64 column, and runs of strings.
std::vector<Dataset> datasets() {
    constexpr std::uint64_t n = 40000;
    std::mt19937_64 rng(20260510);
    std::vector<Dataset> out;

    Values strings;
    std::vector<std::string> pool;
    for (int i = 0; i < 50; ++i) {
        pool.push_back("value-" + std::string(40 - 6 - std::to_string(i).size(), '.') + std::to_string(i));
    }
    for (std::uint64_t i = 0; i < n; ++i) {
        const auto h = rng();
        strings.push_back(h % 11U == 0U ? std::nullopt : std::optional<std::string>(pool[h % pool.size()]));
    }
    out.push_back({"strings", true, n, std::move(strings)});

    Values ints;
    std::vector<std::int64_t> ipool;
    for (int i = 0; i < 30; ++i) {
        ipool.push_back(static_cast<std::int64_t>(rng()));
    }
    for (std::uint64_t i = 0; i < n; ++i) {
        const auto h = rng();
        if (h % 9U == 0U) {
            ints.push_back(std::nullopt);
            continue;
        }
        const auto x = ipool[h % ipool.size()];
        ints.emplace_back(std::string(reinterpret_cast<const char*>(&x), sizeof x));
    }
    out.push_back({"ints", false, n, std::move(ints)});

    Values runs;
    std::vector<std::string> rpool;
    for (int i = 0; i < 12; ++i) {
        rpool.push_back("uri/" + std::to_string(i * 7919) + "/segment");
    }
    while (runs.size() < n) {
        const auto len = 1U + rng() % 200U;
        const auto h = rng();
        for (std::uint64_t k = 0; k < len && runs.size() < n; ++k) {
            runs.push_back(std::optional<std::string>(rpool[h % rpool.size()]));
        }
    }
    out.push_back({"runs", true, n, std::move(runs)});
    return out;
}

}  // namespace

int main() {
    // Set before any read: the take cache is sized once per process.
    setenv("NANOLANCE_TAKE_CACHE_MB", "0", 1);

    std::size_t checks = 0;
    for (auto& d : datasets()) {
        const auto ds = temp_dataset(d.name);
        write_column(ds, d.strings, d.values);

        // A full read: the values the rest are compared with, and proof the column is dictionary coded
        // (its pages expand their rows, which the counter counts).
        ws::reset();
        const auto full = read_range(ds, d.strings, 0, d.rows);
        require(full == d.values, d.name + ": full read differs from what was written");
        require(ws::counters().dict_rows_expanded.load() >= d.rows,
                d.name + ": not dictionary coded (no dictionary rows expanded)");

        // Ranges at chunk edges and inside chunks, empty, the last rows, and random ones.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        const std::uint64_t n = d.rows;
        for (const std::uint64_t s : std::initializer_list<std::uint64_t>{0, 1, 1023, 1024, 1025, 2047, 2048, 4095, 4096,
                                      4097, n - 1025, n - 1024, n - 1, n - 2}) {
            for (const std::uint64_t l : std::initializer_list<std::uint64_t>{0, 1, 2, 3, 1000, 1023, 1024, 1025, 2049,
                                          5000}) {
                if (s + l <= n) {
                    ranges.emplace_back(s, l);
                }
            }
        }
        std::mt19937_64 rng(7);
        for (int i = 0; i < 40; ++i) {
            const auto s = rng() % n;
            ranges.emplace_back(s, rng() % (n - s + 1U));
        }
        for (const auto& [s, l] : ranges) {
            ws::reset();
            const auto got = read_range(ds, d.strings, s, l);
            const Values want(full.begin() + static_cast<std::ptrdiff_t>(s),
                              full.begin() + static_cast<std::ptrdiff_t>(s + l));
            require(got == want, d.name + ": range [" + std::to_string(s) + ", +" + std::to_string(l) + ") differs");
            require(ws::counters().dict_rows_expanded.load() == l,
                    d.name + ": range [" + std::to_string(s) + ", +" + std::to_string(l) + ") expanded " +
                        std::to_string(ws::counters().dict_rows_expanded.load()) + " rows for " + std::to_string(l));
            ++checks;
        }

        // Takes: few rows (chunk path), many rows (whole-page path), rows repeated and out of order, rows
        // straddling a chunk edge, the first and the last row.
        std::vector<std::vector<std::uint64_t>> takes = {
            {0}, {n - 1}, {0, n - 1}, {5, 5, 5}, {900, 100, 1000}, {1020, 1021, 1022, 1023, 1024, 1025, 1026},
        };
        for (const std::size_t size : {1UL, 2UL, 5UL, 17UL, 64UL, 300UL, 3000UL}) {
            std::vector<std::uint64_t> rows;
            for (std::size_t i = 0; i < size; ++i) {
                rows.push_back(rng() % n);
            }
            takes.push_back(rows);
        }
        for (const auto& rows : takes) {
            std::set<std::uint64_t> distinct(rows.begin(), rows.end());
            const std::vector<std::uint64_t> picked(distinct.begin(), distinct.end());
            ws::reset();
            const auto got = read_take(ds, d.strings, rows);
            require(got == pick(full, picked), d.name + ": take of " + std::to_string(rows.size()) + " rows differs");
            require(ws::counters().dict_rows_expanded.load() == picked.size(),
                    d.name + ": take of " + std::to_string(picked.size()) + " distinct rows expanded " +
                        std::to_string(ws::counters().dict_rows_expanded.load()));
            ++checks;
        }
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }
    std::cout << "dict row selection: " << checks << " reads matched a full read, each expanding only its rows\n";
    return 0;
}
