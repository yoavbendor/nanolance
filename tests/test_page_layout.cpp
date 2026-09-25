// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// DIFFERENTIAL ORACLE for the PageLayout descriptor.
//
// nanolance's reader has always dispatched on its own field metadata (`nanolance:packing` =
// constant / rle / dict / dict-rle / bitpack / bss-zstd) rather than on the
// /lance.encodings21.PageLayout descriptor the writer puts in every page. That private side channel
// is exactly why files from the Rust lance crate do not decode: they carry the descriptor but not
// the metadata.
//
// Before decode can be re-rooted onto the descriptor, the descriptor has to be shown to say the same
// thing. That is what this test is. For every encoding the writer can emit, it writes a dataset,
// reads back BOTH signals -- the legacy metadata and the newly-parsed descriptor -- and asserts they
// agree. Because the writer's whole output space is covered, a green run is evidence that the
// descriptor path can replace the metadata path without changing what any nanolance-written file
// decodes to.
//
// It doubles as the conformance test for the parser itself, including the malformed-input cases
// (these bytes are untrusted like everything else on the read path).

#include "nanolance/lance_column_decoder.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/page_layout.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace pl = nano_lance::page_layout;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FATAL: " << message << '\n';
        std::exit(1);
    }
}

/// What the legacy `nanolance:packing` metadata says for a field ("" when absent = plain page).
std::string packing_of(const nano_lance::pb::Field& field) {
    const auto it = field.metadata.find("nanolance:packing");
    if (it == field.metadata.end()) {
        return "";
    }
    return std::string(it->second.begin(), it->second.end());
}

/// Flatten a CompressiveEncoding tree to a shape string, ignoring widths and row counts -- those
/// legitimately differ per column; what must agree is WHICH encoding the page uses.
std::string shape_of(const pl::Compressive* node) {
    if (node == nullptr) {
        return "none";
    }
    switch (node->kind) {
        case pl::CompressiveKind::kFlat:
            return "Flat";
        case pl::CompressiveKind::kInlineBitpacking:
            return "InlineBitpacking";
        case pl::CompressiveKind::kVariable:
            return "Variable";
        case pl::CompressiveKind::kFsst:
            return "Fsst(" + shape_of(node->values.get()) + ")";
        case pl::CompressiveKind::kRle:
            return "Rle";
        case pl::CompressiveKind::kByteStreamSplit:
            return "ByteStreamSplit(" + shape_of(node->values.get()) + ")";
        case pl::CompressiveKind::kGeneral:
            return std::string("General(") +
                   (node->scheme == pl::BufferScheme::kZstd ? "ZSTD" : "NONE") + "," +
                   shape_of(node->values.get()) + ")";
        default:
            return "Unknown";
    }
}

/// The descriptor shape the legacy metadata implies. This is the oracle: two independent signals in
/// the same file, which must not disagree.
std::string expected_shape(const std::string& packing, bool variable_width, bool zstd) {
    if (packing == "constant") {
        return "CONSTANT";
    }
    if (packing == "rle" || packing == "dict-rle") {
        return "Rle";
    }
    if (packing == "bitpack" || packing == "dict") {
        return "InlineBitpacking";
    }
    if (packing == "bss-zstd") {
        return "General(ZSTD,ByteStreamSplit(Flat))";
    }
    if (variable_width) {
        return zstd ? "General(ZSTD,Variable)" : "Variable";
    }
    return "Flat";
}

struct Column {
    std::string name;
    ArrowType type;
    std::vector<std::int64_t> ints;
    std::vector<double> doubles;
    std::vector<std::string> strings;
};

/// Write one dataset from `columns` and return its path.
std::filesystem::path write_dataset(const std::filesystem::path& root, const std::string& name,
                                    const std::vector<Column>& columns, bool compress) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, static_cast<std::int64_t>(columns.size())) == NANOARROW_OK,
            "schema struct init");
    for (std::size_t i = 0; i < columns.size(); ++i) {
        require(ArrowSchemaSetType(schema.children[i], columns[i].type) == NANOARROW_OK, "set type");
        require(ArrowSchemaSetName(schema.children[i], columns[i].name.c_str()) == NANOARROW_OK, "set name");
        schema.children[i]->flags = 0;
    }
    schema.flags = 0;

    ArrowArray array{};
    require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array init");
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start appending");

    std::size_t rows = 0;
    for (const auto& c : columns) {
        rows = std::max({rows, c.ints.size(), c.doubles.size(), c.strings.size()});
    }
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            auto* child = array.children[i];
            const auto& c = columns[i];
            int rc = NANOARROW_OK;
            if (!c.strings.empty()) {
                const auto& v = c.strings[r];
                ArrowStringView sv{v.c_str(), static_cast<std::int64_t>(v.size())};
                rc = ArrowArrayAppendString(child, sv);
            } else if (!c.doubles.empty()) {
                rc = ArrowArrayAppendDouble(child, c.doubles[r]);
            } else {
                rc = ArrowArrayAppendInt(child, c.ints[r]);
            }
            require(rc == NANOARROW_OK, "append value");
        }
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "finish element");
    }
    require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish building");

    const auto path = root / (name + ".lance");
    std::filesystem::remove_all(path);
    NanoLanceWriter w{};
    require(nano_lance_writer_init(&w, path.string().c_str(), 3) == NANO_LANCE_OK, "writer init");
    if (compress) {
        require(nano_lance_writer_set_compression(&w, true) == NANO_LANCE_OK, "set compression");
    }
    require(nano_lance_write_batch(&w, &array, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(&w));
    require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK,
            std::string("commit: ") + nano_lance_writer_last_error(&w));
    nano_lance_writer_close(&w);
    // Both, and in this order. An ArrowArray owns its buffers and its children; releasing only the
    // schema leaks every one of them. Caught by the ASan/UBSan CI job (LeakSanitizer), which found it
    // the first time that job was able to run.
    ArrowArrayRelease(&array);
    ArrowSchemaRelease(&schema);
    return path;
}

/// THE decisive test for step 2 of the plan.
///
/// Decode every column twice: once normally, and once with EVERY nanolance-private key stripped from
/// the on-disk field -- exactly what a file written by the Rust lance crate looks like. If the two
/// agree byte for byte, decode genuinely selects its path from the page descriptor and no longer
/// depends on the private side channel. If it still read the metadata, the stripped run would take
/// the wrong branch (or the flat fallback) and disagree.
void check_metadata_is_not_load_bearing(const std::filesystem::path& dataset, const std::string& label) {
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    std::string error;
    require(nano_lance::load_latest_manifest(dataset, manifest, version, error),
            label + ": load manifest: " + error);
    require(!manifest.fragments.empty() && !manifest.fragments[0].files.empty(), label + ": no data file");
    const auto data_file = dataset / "data" / manifest.fragments[0].files[0].path;

    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout{};
    require(nano_lance::read_lance_data_file_footer_and_descriptor(data_file, descriptor, layout, error),
            label + ": footer: " + error);
    std::vector<nano_lance::pb::ColumnMetadata> columns;
    require(nano_lance::read_lance_data_file_column_metadatas(data_file, layout, columns, error),
            label + ": column metadata: " + error);

    std::size_t col = 0;
    for (const auto& field : descriptor.fields) {
        if (field.logical_type == "struct") {
            continue;
        }
        require(col < columns.size(), label + ": more fields than columns");
        const std::string where = label + "/" + field.name;

        nano_lance::ColumnValues with_metadata;
        std::string with_error;
        const bool with_ok =
            nano_lance::decode_lance_physical_column(data_file, field, columns[col], with_metadata, with_error);
        check(with_ok, where + ": decode failed: " + with_error);

        // Strip every nanolance:* key. `lance-encoding:*` keys stay -- those are Lance-standard and a
        // stock-Lance file carries them too.
        nano_lance::pb::Field stripped = field;
        std::size_t removed = 0;
        for (auto it = stripped.metadata.begin(); it != stripped.metadata.end();) {
            if (it->first.rfind("nanolance:", 0) == 0) {
                it = stripped.metadata.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }

        nano_lance::ColumnValues without_metadata;
        std::string without_error;
        const bool without_ok = nano_lance::decode_lance_physical_column(data_file, stripped, columns[col],
                                                                        without_metadata, without_error);
        check(without_ok, where + ": decode WITHOUT nanolance metadata failed (" +
                              std::to_string(removed) + " keys stripped): " + without_error);
        if (!with_ok || !without_ok) {
            ++col;
            continue;
        }

        check(with_metadata.kind == without_metadata.kind,
              where + ": column kind changed when the private metadata was stripped");
        check(with_metadata.fixed == without_metadata.fixed,
              where + ": fixed bytes differ without the private metadata (" +
                  std::to_string(with_metadata.fixed.size()) + " vs " +
                  std::to_string(without_metadata.fixed.size()) + ")");
        check(with_metadata.variable.data == without_metadata.variable.data,
              where + ": variable data differs without the private metadata");
        check(with_metadata.variable.offsets == without_metadata.variable.offsets,
              where + ": variable offsets differ without the private metadata");
        check(with_metadata.variable.large == without_metadata.variable.large,
              where + ": offset width differs without the private metadata");
        ++col;
    }
}

/// Read every page of every column and compare the parsed descriptor against the legacy metadata.
void check_dataset(const std::filesystem::path& dataset, const std::string& label, bool compress) {
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    std::string error;
    require(nano_lance::load_latest_manifest(dataset, manifest, version, error),
            label + ": load manifest: " + error);

    std::map<std::int32_t, nano_lance::pb::Field> fields_by_id;
    for (const auto& f : manifest.fields) {
        fields_by_id[f.id] = f;
    }

    require(!manifest.fragments.empty() && !manifest.fragments[0].files.empty(), label + ": no data file");
    const auto data_file = dataset / "data" / manifest.fragments[0].files[0].path;

    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout{};
    require(nano_lance::read_lance_data_file_footer_and_descriptor(data_file, descriptor, layout, error),
            label + ": footer: " + error);
    std::vector<nano_lance::pb::ColumnMetadata> columns;
    require(nano_lance::read_lance_data_file_column_metadatas(data_file, layout, columns, error),
            label + ": column metadata: " + error);

    // The data file's own field list is in column order, which is how column metadata is indexed.
    std::size_t col = 0;
    for (const auto& field : descriptor.fields) {
        if (field.logical_type == "struct") {
            continue;  // logical-only, owns no column
        }
        require(col < columns.size(), label + ": more fields than columns");
        const auto packing = packing_of(field);
        const bool variable_width =
            field.logical_type == "string" || field.logical_type == "binary" ||
            field.logical_type == "large_string" || field.logical_type == "large_binary";
        const auto want = expected_shape(packing, variable_width, compress);

        const auto& meta = columns[col];
        check(!meta.pages.empty(), label + "/" + field.name + ": column has no pages");
        for (std::size_t p = 0; p < meta.pages.size(); ++p) {
            const auto& page = meta.pages[p];
            const std::string where = label + "/" + field.name + " page " + std::to_string(p);

            // (1) The descriptor must actually be there. It was written all along and dropped on
            //     read; if this regresses, every claim below becomes vacuous.
            check(!page.encoding.empty(), where + ": page carries no PageLayout descriptor");
            if (page.encoding.empty()) {
                continue;
            }

            // (2) It must parse.
            pl::PageLayout parsed;
            std::string parse_error;
            const bool ok = pl::decode_page_layout(page.encoding, parsed, parse_error);
            check(ok, where + ": descriptor failed to parse: " + parse_error);
            if (!ok) {
                continue;
            }

            // (3) It must agree with the legacy metadata.
            const std::string got = parsed.kind == pl::LayoutKind::kConstant
                                        ? "CONSTANT"
                                        : shape_of(parsed.mini_block.value_compression.get());
            // An FSST-tagged column (roadmap F2) is FSST wherever that pays and otherwise keeps the
            // untagged shape -- plain Variable, or zstd when the column asked for it.
            const bool fsst_ok = packing == "fsst" && (got == "Fsst(Variable)" || got == "Variable" ||
                                                       got == expected_shape("", true, compress));
            check(got == want || fsst_ok, where + ": descriptor says " + got + " but nanolance:packing='" +
                                              packing + "' implies " + want + " (" + pl::describe(parsed) + ")");

            // (4) Dictionary pages must carry their dictionary in the descriptor too.
            if (packing == "dict" || packing == "dict-rle") {
                check(parsed.mini_block.dictionary != nullptr,
                      where + ": dictionary page has no dictionary in its descriptor");
                check(parsed.mini_block.num_dictionary_items > 0,
                      where + ": dictionary page declares 0 dictionary items");
            }

            // (5) The row count in the descriptor must match the page's own.
            if (parsed.kind == pl::LayoutKind::kMiniBlock) {
                check(parsed.mini_block.num_items == page.length,
                      where + ": descriptor says " + std::to_string(parsed.mini_block.num_items) +
                          " rows, page says " + std::to_string(page.length));
            }
        }
        ++col;
    }
}

/// The parser sees untrusted bytes; malformed input must be refused, never misread.
void check_malformed_inputs() {
    pl::PageLayout out;
    std::string error;

    // Empty is not an error -- it means "no descriptor", so old files keep reading.
    check(pl::decode_page_layout({}, out, error), "empty descriptor should parse as 'none'");
    check(out.kind == pl::LayoutKind::kNone, "empty descriptor should yield kNone");

    const std::vector<std::vector<std::uint8_t>> malformed = {
        {0x0a},                          // tag with no payload
        {0x0a, 0x7f},                    // length runs past the end
        {0x08, 0xff, 0xff, 0xff, 0xff,   // varint that never terminates
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {0x12, 0x04, 0x0a, 0x7f, 0x00, 0x00},  // nested length past the end
        {0x0b},                                // group wire type, refused rather than guessed
    };
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        pl::PageLayout parsed;
        std::string err;
        const bool ok = pl::decode_page_layout(malformed[i], parsed, err);
        check(!ok, "malformed descriptor #" + std::to_string(i) + " should be refused");
        check(!ok || !err.empty(), "refusal #" + std::to_string(i) + " should carry a reason");
    }

    // A failed parse must leave NOTHING behind. The layout kind is chosen before its body is parsed
    // (PageLayout field 1 means MiniBlock whether or not the MiniBlock parses), so an earlier version
    // returned false with kind already set to kMiniBlock and an empty body. Decode dispatch reads the
    // kind to pick a decoder, so that would have selected one from a descriptor that did not parse.
    // Found by tests/fuzz/fuzz_page_layout.cpp: a well-formed MiniBlock header followed by a tag with
    // wire type 6.
    {
        const std::vector<std::uint8_t> truncated_miniblock{
            0x0a, 0x1d, '/',  'l',  'a',  'n',  'c',  'e',  '.',  'e',  'n',  'c',  'o',  'd',  'i',
            'n',  'g',  's',  '2',  '1',  '.',  'P',  'a',  'g',  'e',  'L',  'a',  'y',  'o',  'u',
            't',  0x12, 0x1c, 0x0a, 0x1a, 0x1a, 0x0e, 0x42, 0x0c, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x40,
            0x12, 0x04, 0x0a, 0x02, 0x08, 0x08, 0xce, 0xfe, 0xfe, 0xc7, 0x02, 0x48, 0xa0, 0x1f, 0x50,
            0x01};
        pl::PageLayout parsed;
        std::string err;
        check(!pl::decode_page_layout(truncated_miniblock, parsed, err),
              "a MiniBlock body with an invalid wire type should be refused");
        check(parsed.kind == pl::LayoutKind::kNone,
              "a refused parse must leave kind == kNone, got " +
                  std::to_string(static_cast<int>(parsed.kind)));
        check(parsed.mini_block.value_compression == nullptr,
              "a refused parse must not leave a partial encoding tree behind");
    }

    // Same guarantee when a previously-successful parse is reused: the output is reset, not merged.
    {
        pl::PageLayout reused;
        std::string err;
        const std::vector<std::uint8_t> good{
            0x0a, 0x1d, '/',  'l',  'a',  'n',  'c',  'e',  '.',  'e',  'n',  'c', 'o',  'd',  'i',
            'n',  'g',  's',  '2',  '1',  '.',  'P',  'a',  'g',  'e',  'L',  'a', 'y',  'o',  'u',
            't',  0x12, 0x08, 0x0a, 0x06, 0x1a, 0x04, 0x2a, 0x02, 0x08, 0x40};
        check(pl::decode_page_layout(good, reused, err), "well-formed bitpacking descriptor: " + err);
        check(reused.kind == pl::LayoutKind::kMiniBlock, "expected a MiniBlock layout");
        check(pl::decode_page_layout({0x0a}, reused, err) == false, "malformed reuse should be refused");
        check(reused.kind == pl::LayoutKind::kNone, "a refused reuse must clear the previous result");
    }

    // A type url that is not a PageLayout is refused by name rather than parsed as one.
    std::vector<std::uint8_t> wrong_url{0x0a, 0x05};
    for (char ch : std::string("hello")) {
        wrong_url.push_back(static_cast<std::uint8_t>(ch));
    }
    pl::PageLayout parsed;
    std::string err;
    check(!pl::decode_page_layout(wrong_url, parsed, err), "unexpected type url should be refused");

    // An unmodeled layout variant parses but is reported, so decode refuses by name instead of
    // misreading the page's buffers. PageLayout field 4 (BlobLayout) with an empty body -- field 3,
    // FullZip, used to serve here until it became a modelled layout.
    const std::vector<std::uint8_t> unknown_layout{
        0x0a, 0x1d, '/', 'l', 'a', 'n', 'c', 'e', '.', 'e', 'n', 'c', 'o', 'd', 'i', 'n',
        'g',  's',  '2', '1', '.', 'P', 'a', 'g', 'e', 'L', 'a', 'y', 'o', 'u', 't',
        0x12, 0x02, 0x22, 0x00};
    pl::PageLayout unknown;
    std::string unknown_err;
    check(pl::decode_page_layout(unknown_layout, unknown, unknown_err),
          "unmodeled layout should parse: " + unknown_err);
    check(unknown.unknown_layout_field == 4U,
          "unmodeled layout should record its field number, got " +
              std::to_string(unknown.unknown_layout_field));
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root = argc > 1 ? argv[1] : "page_layout_probe";
    std::filesystem::create_directories(root);

    std::vector<std::int64_t> wide;      // high-range -> InlineBitpacking
    std::vector<std::int64_t> constant;  // all equal  -> ConstantLayout
    std::vector<std::int64_t> runs;      // long runs  -> Rle
    std::vector<double> smooth;          // float      -> ByteStreamSplit + zstd under --compress
    std::vector<std::string> high_card;  // distinct   -> Variable (zstd under --compress)
    std::vector<std::string> low_card;   // few values -> dictionary
    std::vector<std::string> const_str;  // all equal  -> ConstantLayout
    for (int i = 0; i < 4000; ++i) {
        wide.push_back(static_cast<std::int64_t>(i) * 7919);
        constant.push_back(42);
        runs.push_back(i / 500);
        smooth.push_back(static_cast<double>(i) * 0.25);
        high_card.push_back("value-" + std::to_string(i));
        low_card.push_back(i % 3 == 0 ? "alpha" : (i % 3 == 1 ? "beta" : "gamma"));
        const_str.push_back("file:///capture.pcapng");
    }

    // Both knobs, since zstd changes the descriptor for exactly the columns it touches.
    for (const bool compress : {false, true}) {
        const std::string suffix = compress ? "_zstd" : "_plain";
        const auto ds = write_dataset(root, "mixed" + suffix,
                                      {
                                          {"wide", NANOARROW_TYPE_INT64, wide, {}, {}},
                                          {"constant", NANOARROW_TYPE_INT64, constant, {}, {}},
                                          {"runs", NANOARROW_TYPE_INT64, runs, {}, {}},
                                          {"smooth", NANOARROW_TYPE_DOUBLE, {}, smooth, {}},
                                          {"high_card", NANOARROW_TYPE_STRING, {}, {}, high_card},
                                          {"low_card", NANOARROW_TYPE_STRING, {}, {}, low_card},
                                          {"const_str", NANOARROW_TYPE_STRING, {}, {}, const_str},
                                      },
                                      compress);
        check_dataset(ds, "mixed" + suffix, compress);
        check_metadata_is_not_load_bearing(ds, "mixed" + suffix);
    }

    check_malformed_inputs();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "page layout: descriptor agrees with nanolance:packing on every page, and\n"
                 "            every column decodes identically with that metadata stripped\n";
    return 0;
}
