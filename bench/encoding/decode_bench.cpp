// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer encoding, measured: how fast one column of each encoding kind decodes. Each column is written to its own
// .lance file, with the writer encoding it one way (the field tag `nanolance:packing` picks the kind, as the dataset
// writer's heuristics do). The page layout names the kind, the values read back are checked, and then
// decode_lance_physical_column is timed on the column. The file is in the OS cache after the first read.
//
// Each column is sized to the same decoded bytes, not to the same row count: a column gets rows until it holds --mb
// MiB of decoded values (at least 1000 rows). So each case takes milliseconds, not microseconds, and the cases are
// comparable by bytes per second. Rows of a column are the same in every file that has the column, because the rows
// are a function of the row number.
//
// Usage: nl_bench_encoding [--mb N] [--rows N] [--runs N] [--quick] [--json PATH]
//   --mb N    decoded MiB per column (default 16; --quick 0.5). A column holds the rows that fill it.
//   --rows N  forces exactly N rows for every column, overriding --mb.
//   --runs N  timed runs per column after one warm-up (default 7; --quick 2).
//   --json    also write the results as JSON (see bench/common/bench_support.hpp).
//
// The full-zip column holds 4200-byte strings: the writer lays a variable-width column out as FullZip only when its
// values average 4096 bytes or more (kFullZipMinAverageValueBytes in src/file/data_file_writer.cpp).

#include "../common/bench_support.hpp"

#include "nanolance/array_accessor.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/page_layout.hpp"
#include "nanolance/schema_mapper.hpp"

#include <functional>
#include <string>
#include <vector>

namespace {

using nl_bench::Column;
using nl_bench::Kind;

/// Fewest rows a column gets, however small --mb is.
constexpr std::uint64_t kMinRows = 1000;

/// A 64-bit mixing function: a deterministic pseudo-random sequence for the random columns.
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/// `head` followed by `x` until it is `size` bytes long.
std::string padded(const std::string& head, std::size_t size) {
    std::string out = head;
    out.resize(size, 'x');
    return out;
}

/// One column the benchmark writes: its kind, the value of row i, what the writer is asked for (the field tag, and for
/// "constant" the value), and the encoding kind the writer should give it (the label encoding_kind gives).
struct Spec {
    std::string name;
    Kind kind;
    std::string tag;
    std::string const_value;
    std::string expect;
    std::function<std::string(std::uint64_t)> value;
};

std::vector<Spec> make_specs() {
    return {
        {"int64_seq", Kind::Int64, "bitpack", "", "bitpack",
         [](std::uint64_t i) { return nl_bench::le_bytes(static_cast<std::int64_t>(i)); }},
        {"int64_random", Kind::Int64, "", "", "flat",
         [](std::uint64_t i) { return nl_bench::le_bytes(static_cast<std::int64_t>(splitmix64(i))); }},
        {"int64_constant", Kind::Int64, "constant", nl_bench::le_bytes(std::int64_t{42}), "constant",
         [](std::uint64_t) { return nl_bench::le_bytes(std::int64_t{42}); }},
        {"int64_runs", Kind::Int64, "rle", "", "rle",
         [](std::uint64_t i) { return nl_bench::le_bytes(static_cast<std::int64_t>((i / 500) * 7)); }},
        {"float64_random", Kind::Float64, "bss-zstd", "", "byte-stream-split+zstd",
         [](std::uint64_t i) {
             return nl_bench::le_bytes(static_cast<double>(splitmix64(i) >> 11) * 0x1.0p-53);
         }},
        {"bool", Kind::Bool, "", "", "bool-packed",
         [](std::uint64_t i) { return std::string(1, i % 3 == 0 ? '\1' : '\0'); }},
        {"str_few_values", Kind::Utf8, "dict", "", "dict",
         [](std::uint64_t i) { return "category-" + std::to_string(splitmix64(i) % 8); }},
        {"str_runs", Kind::Utf8, "dict-rle", "", "dict-rle",
         [](std::uint64_t i) { return "run-" + std::to_string((i / 300) % 5); }},
        {"str_unique", Kind::Utf8, "", "", "variable",
         [](std::uint64_t i) { return "id-" + std::to_string(i); }},
        {"str_320_bytes", Kind::Utf8, "", "", "variable",
         [](std::uint64_t i) { return padded("row-" + std::to_string(i) + "-", 320); }},
        {"str_4200_bytes", Kind::Utf8, "", "", "full-zip",
         [](std::uint64_t i) { return padded("doc-" + std::to_string(i) + "-", 4200); }},
    };
}

/// Decoded bytes one row of a column takes: its value, or for text its bytes and one 4-byte offset.
std::uint64_t decoded_row_bytes(Kind kind, const std::string& row) {
    switch (kind) {
        case Kind::Int64:
        case Kind::Float64:
            return 8;
        case Kind::Bool:
            return 1;
        case Kind::Utf8:
            return row.size() + 4;
    }
    return 0;
}

/// The column's rows: exactly `rows` of them when rows > 0, else as many as fill `target_bytes` (at least kMinRows).
Column make_column(const Spec& spec, std::uint64_t rows, std::uint64_t target_bytes) {
    Column column{spec.name, spec.kind, {}};
    std::uint64_t bytes = 0;
    for (std::uint64_t i = 0;; ++i) {
        if (rows > 0 ? i >= rows : (i >= kMinRows && bytes >= target_bytes)) {
            break;
        }
        column.rows.push_back(spec.value(i));
        bytes += decoded_row_bytes(spec.kind, column.rows.back());
    }
    return column;
}

/// The encoding kind of a column's first page, named from its page layout descriptor: constant, full-zip, flat,
/// bool-packed, bitpack, rle, dict, dict-rle, byte-stream-split, variable, fixed-size-list, with +zstd, +lz4 or +fsst
/// where the value buffers are wrapped. "other" or "undecodable" for a layout this names no kind for.
std::string encoding_kind(const std::vector<std::uint8_t>& encoding) {
    using namespace nano_lance::page_layout;
    PageLayout layout;
    std::string error;
    if (!decode_page_layout(encoding, layout, error)) {
        return "undecodable";
    }
    if (layout.kind == LayoutKind::kConstant) {
        return "constant";
    }
    if (layout.kind == LayoutKind::kFullZip) {
        return "full-zip";
    }
    if (layout.kind != LayoutKind::kMiniBlock) {
        return "other";
    }
    std::string suffix;
    const Compressive* node = layout.mini_block.value_compression.get();
    while (node != nullptr && (node->kind == CompressiveKind::kGeneral || node->kind == CompressiveKind::kFsst)) {
        if (node->kind == CompressiveKind::kFsst) {
            suffix += "+fsst";
        } else if (node->scheme == BufferScheme::kZstd) {
            suffix += "+zstd";
        } else if (node->scheme == BufferScheme::kLz4) {
            suffix += "+lz4";
        }
        node = node->values.get();
    }
    if (node == nullptr) {
        return "other";
    }
    if (layout.mini_block.dictionary != nullptr) {
        return (node->kind == CompressiveKind::kRle ? "dict-rle" : "dict") + suffix;
    }
    switch (node->kind) {
        case CompressiveKind::kRle:
            return "rle" + suffix;
        case CompressiveKind::kInlineBitpacking:
        case CompressiveKind::kBitpacked:
            return "bitpack" + suffix;
        case CompressiveKind::kVariable:
            return "variable" + suffix;
        case CompressiveKind::kByteStreamSplit:
            return "byte-stream-split" + suffix;
        case CompressiveKind::kFlat:
            return (node->bits_per_value == 1 ? "bool-packed" : "flat") + suffix;
        case CompressiveKind::kFixedSizeList:
            return "fixed-size-list" + suffix;
        default:
            return "other";
    }
}

/// One column's measurement, and the kind the writer gave it.
struct Measured {
    std::string kind;
    nl_bench::Result result;
};

/// Writes the column to `path` with its field tag, reads the kind back, checks the decoded values and times the decode.
/// Returns "" on success, else the failure message.
std::string measure_column(const Spec& spec, Column column, const std::filesystem::path& path, int runs,
                           Measured& out) {
    std::string error;
    std::vector<Column> columns;
    columns.push_back(std::move(column));  // moved, not copied: a column can hold 16 million rows
    const Column& written = columns.front();
    nl_bench::ArrowBatch batch(columns);
    nano_lance::LanceSchemaMapping mapping;
    if (!nano_lance::map_arrow_schema(batch.schema(), mapping, error, true)) {
        return "map_arrow_schema: " + error;
    }
    // The tags go on the field before the writer reads them, which is how the dataset writer picks a column's
    // encoding too.
    for (auto& field : mapping.fields) {
        if (field.column_index < 0) {
            continue;
        }
        if (!spec.tag.empty()) {
            field.metadata["nanolance:packing"] = spec.tag;
        }
        if (spec.tag == "constant") {
            field.metadata["nanolance:const-value"] = spec.const_value;
        }
    }
    std::vector<nano_lance::ColumnValues> values(nano_lance::lance_physical_fields(mapping).size());
    if (!nano_lance::append_batch_column_values(batch.array(), mapping, values, error)) {
        return "append_batch_column_values: " + error;
    }
    nano_lance::LanceFileExtras extras;
    extras.path = path;
    nano_lance::DataFileResult written_info;
    if (!nano_lance::write_lance_data_file(path.parent_path(), path.filename().string(), mapping, values,
                                           written.rows.size(), 0, false, written_info, error, true, &extras)) {
        return "write_lance_data_file: " + error;
    }

    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout;
    if (!nano_lance::read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
        return "read footer: " + error;
    }
    std::vector<nano_lance::pb::ColumnMetadata> metas;
    if (!nano_lance::read_lance_data_file_column_metadatas(path, layout, metas, error)) {
        return "read column metadata: " + error;
    }
    if (descriptor.fields.size() != 1 || metas.size() != 1 || metas[0].pages.empty()) {
        return written.name + ": the file should hold one column with pages";
    }
    out.kind = encoding_kind(metas[0].pages.front().encoding);

    nano_lance::DataFileIo io(path);
    const auto& field = descriptor.fields[0];
    const auto& meta = metas[0];
    nano_lance::ColumnValues decoded;
    if (!nano_lance::decode_lance_physical_column(io, field, meta, decoded, error)) {
        return written.name + ": decode_lance_physical_column: " + error;
    }
    std::string why;
    if (!nl_bench::check_column(decoded, written, why)) {
        return why;
    }
    std::uint64_t median = 0;
    if (!nl_bench::median_nanoseconds(
            runs,
            [&](std::string& err) {
                nano_lance::ColumnValues again;
                return nano_lance::decode_lance_physical_column(io, field, meta, again, err);
            },
            median, error)) {
        return written.name + ": timing: " + error;
    }
    out.result = {"decode/" + written.name, written.rows.size(), nl_bench::decoded_bytes(decoded), median};
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    nl_bench::Options options;
    if (const int rc = nl_bench::parse_options(argc, argv, "nl_bench_encoding", options); rc != -1) {
        return rc;
    }
    const double mb = options.mb > 0 ? options.mb : (options.quick ? 0.5 : 16.0);
    const auto target_bytes = static_cast<std::uint64_t>(mb * 1024.0 * 1024.0);
    const int runs = options.runs ? options.runs : (options.quick ? 2 : 7);

    nl_bench::ScratchDir scratch("nl_bench_encoding");
    std::vector<std::string> kinds;
    std::vector<nl_bench::Result> results;
    std::vector<std::string> notes;
    std::vector<std::string> names;
    for (const auto& spec : make_specs()) {
        Column column = make_column(spec, options.rows, target_bytes);
        Measured measured;
        const auto path = scratch.path() / (spec.name + ".lance");
        if (const auto error = measure_column(spec, std::move(column), path, runs, measured); !error.empty()) {
            return nl_bench::fail(error);
        }
        kinds.push_back(measured.kind);
        results.push_back(measured.result);
        names.push_back(spec.name);
        if (measured.kind != spec.expect) {
            notes.push_back(spec.name + " got " + measured.kind + ", not the " + spec.expect +
                            " this benchmark aims for");
        }
    }

    auto info = nl_bench::make_run_info("nl_bench_encoding", options, options.rows, runs);
    if (options.rows == 0) {
        std::ostringstream scale;
        scale << "about " << mb << " MiB decoded per column";
        info.scale = scale.str();
    }
    nl_bench::print_run_info(info);
    std::cout << "\ncolumn kinds, as the file's page layouts name them:\n";
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::cout << "  " << names[i] << std::string(20 - std::min<std::size_t>(20, names[i].size()), ' ') << kinds[i]
                  << "\n";
    }
    for (const auto& note : notes) {
        std::cout << "  note: " << note << "\n";
    }
    std::cout << "\n";
    nl_bench::print_results(results);

    std::string error;
    if (!options.json_path.empty() && !nl_bench::write_json(options.json_path, info, results, error)) {
        return nl_bench::fail(error);
    }
    return 0;
}
