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

#include "nanolance/data_file_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/page_layout.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace pl = nano_lance::page_layout;

namespace {

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

    int failures = 0;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        std::string name = "column " + std::to_string(c);
        std::size_t seen = 0;
        for (const auto& f : descriptor.fields) {
            if (f.logical_type == "struct") {
                continue;
            }
            if (seen++ == c) {
                name = f.name + " (" + f.logical_type + ")";
                break;
            }
        }
        if (columns[c].pages.empty()) {
            std::cout << label << "  " << name << ": no pages\n";
            continue;
        }
        const auto& encoding = columns[c].pages[0].encoding;
        if (encoding.empty()) {
            std::cout << label << "  " << name << ": no PageLayout descriptor\n";
            ++failures;
            continue;
        }
        pl::PageLayout parsed;
        std::string parse_error;
        if (!pl::decode_page_layout(encoding, parsed, parse_error)) {
            std::cout << label << "  " << name << ": PARSE FAILED: " << parse_error << '\n';
            ++failures;
            continue;
        }
        std::cout << label << "  " << name << ": " << pl::describe(parsed) << '\n';
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: nlance-pagelayout <dataset.lance> [more.lance ...]\n";
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        failures += report(argv[i]);
    }
    return failures == 0 ? 0 : 1;
}
