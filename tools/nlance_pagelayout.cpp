// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Print the /lance.encodings21.PageLayout descriptor of each column's first page.
//
// The point is files nanolance did NOT write. nanolance's reader dispatches on its own
// `nanolance:packing` field metadata, which a dataset from the Rust lance crate does not carry, so
// most of them fail to decode. The descriptor they DO carry is the same one nanolance writes -- this
// tool prints it, which is how you tell "we have no decoder for that encoding" apart from "we have
// the decoder and never look at the descriptor that would have selected it".
//
//   nlance-pagelayout <dataset.lance> [more.lance ...]
//   nlance-pagelayout --dump-corpus <dir> <dataset.lance> [...]
//
// --dump-corpus writes each distinct descriptor to <dir> as its own file, which is how the fuzz
// corpus is seeded (tests/fuzz/fuzz_page_layout.cpp). Starting libFuzzer from real descriptors --
// nanolance's AND pylance's -- means it mutates valid grammar instead of spending its budget
// rediscovering the wrapper.

#include "nanolance/data_file_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/page_layout.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace pl = nano_lance::page_layout;

namespace {

/// When set, descriptors are written here instead of (as well as) being printed.
std::filesystem::path g_corpus_dir;
std::filesystem::path g_pages_dir;  // --dump-fuzz-pages: seed inputs for fuzz_column_decode
bool g_verbose = false;  // -v: also print every page's row count and buffer sizes
std::set<std::vector<std::uint8_t>> g_seen;

void maybe_dump(const std::vector<std::uint8_t>& encoding) {
    if (g_corpus_dir.empty() || !g_seen.insert(encoding).second) {
        return;  // deduplicated: identical descriptors are common across pages and columns
    }
    const auto path = g_corpus_dir / ("descriptor_" + std::to_string(g_seen.size()) + ".bin");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(encoding.data()), static_cast<std::streamsize>(encoding.size()));
}

int report(const std::filesystem::path& dataset) {
    const auto label = dataset.filename().string();
    nano_lance::pb::Manifest manifest;
    std::uint64_t version = 0;
    std::string error;
    if (!nano_lance::load_latest_manifest(dataset, manifest, version, error)) {
        std::cout << label << ": cannot read manifest: " << error << '\n';
        return 1;
    }
    if (manifest.fragments.empty() || manifest.fragments[0].files.empty()) {
        std::cout << label << ": no data files\n";
        return 1;
    }
    const auto data_file = dataset / "data" / manifest.fragments[0].files[0].path;

    nano_lance::pb::FileDescriptor descriptor;
    nano_lance::LanceDataFileFooterLayout layout{};
    if (!nano_lance::read_lance_data_file_footer_and_descriptor(data_file, descriptor, layout, error)) {
        std::cout << label << ": cannot read footer: " << error << '\n';
        return 1;
    }
    std::vector<nano_lance::pb::ColumnMetadata> columns;
    if (!nano_lance::read_lance_data_file_column_metadatas(data_file, layout, columns, error)) {
        std::cout << label << ": cannot read column metadata: " << error << '\n';
        return 1;
    }

    // Name each physical column from the manifest's own field-id -> column-index table, and print the
    // field's full path. Guessing from schema order (skip structs, count the rest) mislabelled every
    // list and map: their parent fields hold no column, so a list's leaf was named after the list,
    // and a map's `value` column after its `key`.
    const auto& file_entry = manifest.fragments[0].files[0];
    const auto field_path = [&manifest](std::int32_t id) {
        std::string path;
        std::string type;
        for (int guard = 0; guard < 64 && id >= 0; ++guard) {
            const nano_lance::pb::Field* found = nullptr;
            for (const auto& f : manifest.fields) {
                if (f.id == id) {
                    found = &f;
                    break;
                }
            }
            if (found == nullptr) {
                break;
            }
            if (type.empty()) {
                type = found->logical_type;
            }
            path = path.empty() ? found->name : found->name + "." + path;
            id = found->parent_id;
        }
        return path + " (" + type + ")";
    };

    int failures = 0;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        std::string name = "column " + std::to_string(c);
        for (std::size_t i = 0; i < file_entry.column_indices.size() && i < file_entry.fields.size(); ++i) {
            if (file_entry.column_indices[i] == static_cast<std::int32_t>(c)) {
                name = field_path(file_entry.fields[i]);
                break;
            }
        }
        if (columns[c].pages.empty()) {
            std::cout << label << "  " << name << ": no pages\n";
            continue;
        }
        if (!g_corpus_dir.empty()) {
            // Seeding wants every page: later pages of a column carry different row counts and, for
            // the last chunk, different bit widths.
            for (const auto& page : columns[c].pages) {
                maybe_dump(page.encoding);
            }
        }
        const auto& encoding = columns[c].pages[0].encoding;
        if (encoding.empty()) {
            std::cout << label << "  " << name << ": no PageLayout descriptor\n";
            ++failures;
            continue;
        }
        maybe_dump(encoding);
        pl::PageLayout parsed;
        std::string parse_error;
        if (!pl::decode_page_layout(encoding, parsed, parse_error)) {
            std::cout << label << "  " << name << ": PARSE FAILED: " << parse_error << '\n';
            ++failures;
            continue;
        }
        std::cout << label << "  " << name << ": " << pl::describe(parsed) << '\n';
        if (!g_pages_dir.empty()) {
            // One file per page, in the input format tests/fuzz/fuzz_column_decode.cpp documents:
            // [u8 n][logical type][u32 rows][u32 n][descriptor][u8 count]{[u32 n][buffer]}.
            std::string logical_type;
            for (std::size_t i = 0; i < file_entry.column_indices.size() && i < file_entry.fields.size(); ++i) {
                if (file_entry.column_indices[i] == static_cast<std::int32_t>(c)) {
                    for (const auto& f : manifest.fields) {
                        if (f.id == file_entry.fields[i]) {
                            logical_type = f.logical_type;
                        }
                    }
                }
            }
            for (std::size_t pg = 0; pg < columns[c].pages.size(); ++pg) {
                const auto& page = columns[c].pages[pg];
                std::vector<std::uint8_t> blob;
                const auto put_u32 = [&blob](std::uint64_t v) {
                    for (int k = 0; k < 4; ++k) {
                        blob.push_back(static_cast<std::uint8_t>((v >> (8 * k)) & 0xFFU));
                    }
                };
                blob.push_back(static_cast<std::uint8_t>(logical_type.size()));
                blob.insert(blob.end(), logical_type.begin(), logical_type.end());
                put_u32(page.length);
                put_u32(page.encoding.size());
                blob.insert(blob.end(), page.encoding.begin(), page.encoding.end());
                blob.push_back(static_cast<std::uint8_t>(page.buffer_sizes.size()));
                bool ok = true;
                for (std::size_t b = 0; b < page.buffer_sizes.size() && ok; ++b) {
                    std::vector<std::uint8_t> bytes;
                    std::string read_error;
                    // Seeds are for exploring structure, not volume: a page larger than this is
                    // skipped rather than written as a multi-megabyte corpus entry.
                    if (page.buffer_sizes[b] > (1U << 20U) ||
                        !nano_lance::read_lance_data_file_bytes(data_file, page.buffer_offsets[b], page.buffer_sizes[b],
                                                                bytes, read_error)) {
                        ok = false;
                        break;
                    }
                    put_u32(bytes.size());
                    blob.insert(blob.end(), bytes.begin(), bytes.end());
                }
                if (!ok) {
                    continue;
                }
                const auto path = g_pages_dir / (label + "_c" + std::to_string(c) + "_p" + std::to_string(pg) + ".page");
                std::ofstream out(path, std::ios::binary);
                out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
            }
        }
        if (g_verbose) {
            // Per page: row count and every buffer's size. The descriptor says how a page is
            // encoded; the buffer sizes are what settle questions about its byte layout.
            for (std::size_t pg = 0; pg < columns[c].pages.size(); ++pg) {
                const auto& page = columns[c].pages[pg];
                std::cout << "    page " << pg << ": rows=" << page.length << " buffers=[";
                for (std::size_t b = 0; b < page.buffer_sizes.size(); ++b) {
                    std::cout << (b != 0U ? "," : "") << page.buffer_sizes[b];
                }
                std::cout << "]\n";
            }
        }
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    int first = 1;
    if (argc >= 2 && std::string(argv[1]) == "-v") {
        g_verbose = true;
        ++first;
    }
    if (argc >= first + 2 && std::string(argv[first]) == "--dump-fuzz-pages") {
        g_pages_dir = argv[first + 1];
        std::error_code ec;
        std::filesystem::create_directories(g_pages_dir, ec);
        first += 2;
    }
    if (argc >= first + 2 && std::string(argv[first]) == "--dump-corpus") {
        g_corpus_dir = argv[first + 1];
        std::error_code ec;
        std::filesystem::create_directories(g_corpus_dir, ec);
        if (ec) {
            std::cerr << "cannot create corpus dir " << g_corpus_dir << ": " << ec.message() << '\n';
            return 2;
        }
        first += 2;
    }
    if (argc <= first) {
        std::cerr << "usage: nlance-pagelayout [-v] [--dump-fuzz-pages <dir>] [--dump-corpus <dir>] <dataset.lance> [more.lance ...]\n";
        return 2;
    }
    int failures = 0;
    for (int i = first; i < argc; ++i) {
        failures += report(argv[i]);
    }
    if (!g_corpus_dir.empty()) {
        std::cout << "wrote " << g_seen.size() << " distinct descriptors to " << g_corpus_dir << '\n';
    }
    return failures == 0 ? 0 : 1;
}
