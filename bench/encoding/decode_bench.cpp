// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer encoding, measured: how fast one column of each encoding kind decodes. The benchmark writes one .lance file
// whose columns the writer encodes one way each (the field tag `nanolance:packing` picks the kind, as the dataset
// writer's heuristics do), reads the kind back from each column's page layout, checks the values, and times
// decode_lance_physical_column on every column. The file is in the OS cache after the first read.
//
// Usage: nl_bench_encoding [--rows N] [--runs N] [--quick] [--json PATH]
//   --rows N  rows per column (default 20000; --quick 2000). The full-zip column holds 4200-byte strings, so the
//             file is about 4.2 KB times the rows for that column alone.
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

#include <string>
#include <vector>

namespace {

using nl_bench::Column;
using nl_bench::Kind;

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

/// The field tag the benchmark asks the writer for, the encoding kind it expects back (the label encoding_kind gives),
/// and for "constant" the value.
struct Request {
    std::string tag;
    std::string const_value;
    std::string expect;
};

/// The columns, one per kind, and what each asks the writer for. Rows are built here, one string per row.
void make_columns(std::uint64_t rows, std::vector<Column>& columns, std::vector<Request>& requests) {
    auto add = [&](const std::string& name, Kind kind, Request request) -> Column& {
        columns.push_back(Column{name, kind, {}});
        requests.push_back(std::move(request));
        columns.back().rows.reserve(rows);
        return columns.back();
    };
    auto& seq = add("int64_seq", Kind::Int64, {"bitpack", "", "bitpack"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        seq.rows.push_back(nl_bench::le_bytes(static_cast<std::int64_t>(i)));
    }
    auto& random = add("int64_random", Kind::Int64, {"", "", "flat"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        random.rows.push_back(nl_bench::le_bytes(static_cast<std::int64_t>(splitmix64(i))));
    }
    auto& constant = add("int64_constant", Kind::Int64, {"constant", nl_bench::le_bytes(std::int64_t{42}), "constant"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        constant.rows.push_back(nl_bench::le_bytes(std::int64_t{42}));
    }
    auto& runs = add("int64_runs", Kind::Int64, {"rle", "", "rle"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        runs.rows.push_back(nl_bench::le_bytes(static_cast<std::int64_t>((i / 500) * 7)));
    }
    auto& floats = add("float64_random", Kind::Float64, {"bss-zstd", "", "byte-stream-split+zstd"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        const double unit = static_cast<double>(splitmix64(i) >> 11) * 0x1.0p-53;
        floats.rows.push_back(nl_bench::le_bytes(unit));
    }
    auto& flags = add("bool", Kind::Bool, {"", "", "bool-packed"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        flags.rows.push_back(std::string(1, i % 3 == 0 ? '\1' : '\0'));
    }
    auto& few = add("str_few_values", Kind::Utf8, {"dict", "", "dict"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        few.rows.push_back("category-" + std::to_string(splitmix64(i) % 8));
    }
    auto& runs_text = add("str_runs", Kind::Utf8, {"dict-rle", "", "dict-rle"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        runs_text.rows.push_back("run-" + std::to_string((i / 300) % 5));
    }
    auto& unique = add("str_unique", Kind::Utf8, {"", "", "variable"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        unique.rows.push_back("id-" + std::to_string(i));
    }
    auto& long_text = add("str_320_bytes", Kind::Utf8, {"", "", "variable"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        long_text.rows.push_back(padded("row-" + std::to_string(i) + "-", 320));
    }
    auto& huge = add("str_4200_bytes", Kind::Utf8, {"", "", "full-zip"});
    for (std::uint64_t i = 0; i < rows; ++i) {
        huge.rows.push_back(padded("doc-" + std::to_string(i) + "-", 4200));
    }
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

}  // namespace

int main(int argc, char** argv) {
    nl_bench::Options options;
    if (const int rc = nl_bench::parse_options(argc, argv, "nl_bench_encoding", options); rc != -1) {
        return rc;
    }
    const std::uint64_t rows = options.rows ? options.rows : (options.quick ? 2000 : 20000);
    const int runs = options.runs ? options.runs : (options.quick ? 2 : 7);

    std::vector<Column> columns;
    std::vector<Request> requests;
    make_columns(rows, columns, requests);

    // Write one file. The mapping is the one write_lance_file makes; the tags go on its fields before the writer
    // reads them, which is how the dataset writer picks a column's encoding too.
    nl_bench::ScratchDir scratch("nl_bench_encoding");
    const auto path = scratch.path() / "kinds.lance";
    nl_bench::ArrowBatch batch(columns);
    nano_lance::LanceSchemaMapping mapping;
    std::string error;
    if (!nano_lance::map_arrow_schema(batch.schema(), mapping, error, true)) {
        return nl_bench::fail("map_arrow_schema: " + error);
    }
    for (auto& field : mapping.fields) {
        if (field.column_index < 0) {
            continue;
        }
        const auto& request = requests.at(static_cast<std::size_t>(field.column_index));
        if (!request.tag.empty()) {
            field.metadata["nanolance:packing"] = request.tag;
        }
        if (request.tag == "constant") {
            field.metadata["nanolance:const-value"] = request.const_value;
        }
    }
    std::vector<nano_lance::ColumnValues> values(nano_lance::lance_physical_fields(mapping).size());
    if (!nano_lance::append_batch_column_values(batch.array(), mapping, values, error)) {
        return nl_bench::fail("append_batch_column_values: " + error);
    }
    nano_lance::LanceFileExtras extras;
    extras.path = path;
    nano_lance::DataFileResult written;
    if (!nano_lance::write_lance_data_file(path.parent_path(), path.filename().string(), mapping, values, rows, 0,
                                           false, written, error, true, &extras)) {
        return nl_bench::fail("write_lance_data_file: " + error);
    }

    // Read the footer, then each column's metadata: the kind comes from the first page's layout.
    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout;
    if (!nano_lance::read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
        return nl_bench::fail("read footer: " + error);
    }
    std::vector<nano_lance::pb::ColumnMetadata> metas;
    if (!nano_lance::read_lance_data_file_column_metadatas(path, layout, metas, error)) {
        return nl_bench::fail("read column metadata: " + error);
    }
    if (descriptor.fields.size() != columns.size() || metas.size() != columns.size()) {
        return nl_bench::fail("the file has " + std::to_string(descriptor.fields.size()) + " columns, want " +
                              std::to_string(columns.size()));
    }
    std::vector<std::string> kinds;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (metas[i].pages.empty()) {
            return nl_bench::fail(columns[i].name + ": the column has no pages");
        }
        kinds.push_back(encoding_kind(metas[i].pages.front().encoding));
    }

    // Decode each column once and check it before timing it.
    nano_lance::DataFileIo io(path);
    std::vector<nl_bench::Result> results;
    std::vector<std::string> notes;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const auto& field = descriptor.fields[i];
        const auto& meta = metas[i];
        nano_lance::ColumnValues decoded;
        if (!nano_lance::decode_lance_physical_column(io, field, meta, decoded, error)) {
            return nl_bench::fail(columns[i].name + ": decode_lance_physical_column: " + error);
        }
        std::string why;
        if (!nl_bench::check_column(decoded, columns[i], why)) {
            return nl_bench::fail(why);
        }
        if (kinds[i] != requests[i].expect) {
            notes.push_back(columns[i].name + " got " + kinds[i] + ", not the " + requests[i].expect +
                            " this benchmark aims for");
        }
        std::uint64_t median = 0;
        if (!nl_bench::median_nanoseconds(
                runs,
                [&](std::string& err) {
                    nano_lance::ColumnValues again;
                    return nano_lance::decode_lance_physical_column(io, field, meta, again, err);
                },
                median, error)) {
            return nl_bench::fail(columns[i].name + ": timing: " + error);
        }
        results.push_back({"decode/" + columns[i].name, rows, nl_bench::decoded_bytes(decoded), median});
    }

    const auto info = nl_bench::make_run_info("nl_bench_encoding", options, rows, runs);
    nl_bench::print_run_info(info);
    std::cout << "\ncolumn kinds, as the file's page layouts name them:\n";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        std::cout << "  " << columns[i].name << std::string(20 - std::min<std::size_t>(20, columns[i].name.size()), ' ')
                  << kinds[i] << "\n";
    }
    for (const auto& note : notes) {
        std::cout << "  note: " << note << "\n";
    }
    std::cout << "\n";
    nl_bench::print_results(results);

    if (!options.json_path.empty() && !nl_bench::write_json(options.json_path, info, results, error)) {
        return nl_bench::fail(error);
    }
    return 0;
}
