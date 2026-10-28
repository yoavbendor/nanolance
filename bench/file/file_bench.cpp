// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer file, measured: what it costs to read a .lance file's framing. Three cases, each checked before it is timed:
//   footer_small_1col     the footer and schema of a 1-column, 100-row file
//   footer_wide_50col     the footer and schema of a 50-column, 10,000-row file
//   column_metadata_wide_50col  every column's metadata of that file (the footer read beforehand)
//   end_to_end_int64_1col the footer, the metadata and one column of a one-column int64 file, decoded: the path a
//                         reader takes to one column. The file size is --rows (default 1,000,000).
//
// The reader keeps a per-thread cache of open handles, validated by size and mtime on every call, so the footer
// cases after the first call open no file; the numbers include that check, not a fresh open.
//
// Usage: nl_bench_file [--rows N] [--runs N] [--quick] [--json PATH]

#include "../common/bench_support.hpp"

#include "nanolance/data_file_reader.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/lance_file_writer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using nl_bench::Column;
using nl_bench::Kind;

/// Writes the columns as one standalone .lance file at `path`.
bool write_file(const std::filesystem::path& path, const std::vector<Column>& columns, std::string& error) {
    nl_bench::ArrowBatch batch(columns);
    const std::vector<const ArrowArray*> batches{&batch.array()};
    nano_lance::LanceFileExtras extras;
    extras.path = path;
    std::uint64_t size = 0;
    return nano_lance::write_lance_file(batch.schema(), batches, extras, size, error);
}

/// A column of `rows` int64 values, `value(row)` each.
template <typename Value>
Column int64_column(const std::string& name, std::uint64_t rows, Value value) {
    Column column{name, Kind::Int64, {}};
    column.rows.reserve(rows);
    for (std::uint64_t i = 0; i < rows; ++i) {
        column.rows.push_back(nl_bench::le_bytes(static_cast<std::int64_t>(value(i))));
    }
    return column;
}

}  // namespace

int main(int argc, char** argv) {
    nl_bench::Options options;
    if (const int rc = nl_bench::parse_options(argc, argv, "nl_bench_file", options); rc != -1) {
        return rc;
    }
    const std::uint64_t big_rows = options.rows ? options.rows : (options.quick ? 10000 : 1000000);
    const int runs = options.runs ? options.runs : (options.quick ? 2 : 7);
    constexpr std::uint64_t kWideRows = 10000;
    constexpr std::size_t kWideColumns = 50;

    nl_bench::ScratchDir scratch("nl_bench_file");
    const auto small_path = scratch.path() / "small.lance";
    const auto wide_path = scratch.path() / "wide.lance";
    const auto big_path = scratch.path() / "big.lance";
    std::string error;

    std::vector<Column> small{int64_column("id", 100, [](std::uint64_t i) { return i; })};
    std::vector<Column> wide;
    for (std::size_t c = 0; c < kWideColumns; ++c) {
        wide.push_back(int64_column("c" + std::string(c < 10 ? "0" : "") + std::to_string(c), kWideRows,
                                    [c](std::uint64_t i) { return i * (c + 1) + c; }));
    }
    const Column big = int64_column("id", big_rows, [](std::uint64_t i) { return 3 * i + 7; });
    if (!write_file(small_path, small, error) || !write_file(wide_path, wide, error) ||
        !write_file(big_path, {big}, error)) {
        return nl_bench::fail("write: " + error);
    }

    std::vector<nl_bench::Result> results;

    // Footer and schema, small and wide. Checked: the column count, and the first column's name.
    struct FooterCase {
        std::string name;
        const std::filesystem::path* path;
        std::size_t columns;
        std::string first_column;
        std::uint64_t rows;
    };
    for (const auto& c : {FooterCase{"footer_small_1col", &small_path, 1, "id", 100},
                          FooterCase{"footer_wide_50col", &wide_path, kWideColumns, "c00", kWideRows}}) {
        const auto& path = *c.path;
        nano_lance::pb::FileDescriptor descriptor;
        nano_lance::LanceDataFileFooterLayout layout;
        if (!nano_lance::read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
            return nl_bench::fail(c.name + ": " + error);
        }
        if (descriptor.fields.size() != c.columns || descriptor.fields.front().name != c.first_column) {
            return nl_bench::fail(c.name + ": the schema has " + std::to_string(descriptor.fields.size()) +
                                  " columns, want " + std::to_string(c.columns));
        }
        std::uint64_t median = 0;
        if (!nl_bench::median_nanoseconds(
                runs,
                [&](std::string& err) {
                    nano_lance::pb::FileDescriptor again;
                    nano_lance::LanceDataFileFooterLayout again_layout;
                    return nano_lance::read_lance_data_file_footer_and_descriptor(path, again, again_layout, err);
                },
                median, error)) {
            return nl_bench::fail(c.name + ": timing: " + error);
        }
        results.push_back({c.name, c.rows, std::nullopt, median});
    }

    // Column metadata of the wide file. Checked: 50 columns, each with pages whose lengths add up to the row count.
    nano_lance::pb::FileDescriptor wide_descriptor;
    nano_lance::LanceDataFileFooterLayout wide_layout;
    if (!nano_lance::read_lance_data_file_footer_and_descriptor(wide_path, wide_descriptor, wide_layout, error)) {
        return nl_bench::fail("column_metadata_wide_50col: " + error);
    }
    std::vector<nano_lance::pb::ColumnMetadata> metas;
    if (!nano_lance::read_lance_data_file_column_metadatas(wide_path, wide_layout, metas, error)) {
        return nl_bench::fail("column_metadata_wide_50col: " + error);
    }
    if (metas.size() != kWideColumns) {
        return nl_bench::fail("the wide file has " + std::to_string(metas.size()) + " column metadatas, want 50");
    }
    for (std::size_t c = 0; c < metas.size(); ++c) {
        std::uint64_t total = 0;
        for (const auto& page : metas[c].pages) {
            total += page.length;
        }
        if (total != kWideRows) {
            return nl_bench::fail("column " + std::to_string(c) + " has " + std::to_string(total) +
                                  " rows in its pages, want " + std::to_string(kWideRows));
        }
    }
    std::uint64_t median = 0;
    if (!nl_bench::median_nanoseconds(
            runs,
            [&](std::string& err) {
                std::vector<nano_lance::pb::ColumnMetadata> again;
                return nano_lance::read_lance_data_file_column_metadatas(wide_path, wide_layout, again, err);
            },
            median, error)) {
        return nl_bench::fail("column_metadata_wide_50col: timing: " + error);
    }
    results.push_back({"column_metadata_wide_50col", kWideRows, std::nullopt, median});

    // One column end to end: footer, metadata, then the column decoded. Checked: the row count and sampled values.
    auto read_one_column = [&](std::string& err, nano_lance::ColumnValues& out) {
        nano_lance::pb::FileDescriptor descriptor;
        nano_lance::LanceDataFileFooterLayout layout;
        if (!nano_lance::read_lance_data_file_footer_and_descriptor(big_path, descriptor, layout, err)) {
            return false;
        }
        std::vector<nano_lance::pb::ColumnMetadata> columns;
        if (!nano_lance::read_lance_data_file_column_metadatas(big_path, layout, columns, err) ||
            columns.size() != 1 || descriptor.fields.size() != 1) {
            err = err.empty() ? "the file should have one column" : err;
            return false;
        }
        nano_lance::DataFileIo io(big_path);
        return nano_lance::decode_lance_physical_column(io, descriptor.fields[0], columns[0], out, err);
    };
    nano_lance::ColumnValues decoded;
    if (!read_one_column(error, decoded)) {
        return nl_bench::fail("end_to_end_int64_1col: " + error);
    }
    std::string why;
    if (!nl_bench::check_column(decoded, big, why)) {
        return nl_bench::fail("end_to_end_int64_1col: " + why);
    }
    if (!nl_bench::median_nanoseconds(
            runs,
            [&](std::string& err) {
                nano_lance::ColumnValues again;
                return read_one_column(err, again);
            },
            median, error)) {
        return nl_bench::fail("end_to_end_int64_1col: timing: " + error);
    }
    results.push_back({"end_to_end_int64_1col", big_rows, nl_bench::decoded_bytes(decoded), median});

    const auto info = nl_bench::make_run_info("nl_bench_file", options, big_rows, runs);
    nl_bench::print_run_info(info);
    std::cout << "\n";
    nl_bench::print_results(results);

    if (!options.json_path.empty() && !nl_bench::write_json(options.json_path, info, results, error)) {
        return nl_bench::fail(error);
    }
    return 0;
}
