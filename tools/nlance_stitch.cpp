// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlance-stitch — gather/merge many free-standing Lance datasets into one self-contained dataset.
//
// Globs `<input_dir>/<prefix>*_<item>.lance` worker-output folders, orders them by the integer <item> in
// each folder name, then stitches them: every fragment's data file is MOVED into one `<master>/data/` and a
// single master manifest is written referencing them all (no column data is read or re-encoded). The
// emptied source folders are deleted. The result is an ordinary multi-fragment Lance dataset that stock
// tools open out of the box; row order follows <item>. See nanolance/dataset_stitcher.hpp.

#include "cli_subcommands.hpp"

#include "nanolance/dataset_stitcher.hpp"
#include "nanolance/version.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Parse the trailing `_<digits>.lance` of a folder name into its item number. Requires the name to start
// with `prefix`. Returns nullopt if it does not match (the folder is skipped).
std::optional<std::uint64_t> item_number(const std::string& name, const std::string& prefix) {
    if (name.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    static const std::regex re(R"(_(\d+)\.lance$)");
    std::smatch m;
    if (!std::regex_search(name, m, re)) {
        return std::nullopt;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(m[1].str()));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

int nanolance_cli_stitch(int argc, char** argv) {
    CLI::App app{"Stitch many free-standing Lance worker outputs into one self-contained dataset"};
    std::string input_dir;
    std::string master_out;
    std::string prefix = "results_";
    app.add_option("input_dir", input_dir, "Directory containing the <prefix>*_<item>.lance folders")->required();
    app.add_option("master_out", master_out, "Output master .lance dataset directory (must not exist)")->required();
    app.add_option("--prefix", prefix, "Folder-name prefix to match")->default_val("results_");
    app.set_version_flag("--version", std::string(nanolance::library_version()));
    CLI11_PARSE(app, argc, argv);

    std::error_code ec;
    if (!fs::is_directory(input_dir, ec)) {
        std::fprintf(stderr, "nlance-stitch: not a directory: %s\n", input_dir.c_str());
        return 1;
    }

    // Collect (item, path) for every matching subfolder, then order by item.
    std::vector<std::pair<std::uint64_t, fs::path>> found;
    for (const auto& entry : fs::directory_iterator(input_dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (const auto item = item_number(name, prefix)) {
            found.emplace_back(*item, entry.path());
        }
    }
    if (found.empty()) {
        std::fprintf(stderr, "nlance-stitch: no '%s*_<item>.lance' folders under %s\n", prefix.c_str(),
                     input_dir.c_str());
        return 1;
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<fs::path> sources;
    sources.reserve(found.size());
    for (const auto& [item, path] : found) {
        sources.push_back(path);
    }

    nano_lance::StitchSummary summary;
    std::string err;
    if (!nano_lance::stitch_datasets(master_out, sources, nano_lance::StitchOptions{}, summary, err)) {
        std::fprintf(stderr, "nlance-stitch: %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr, "nlance-stitch: merged %zu datasets -> %llu fragments, %llu rows -> %s\n",
                 summary.source_count, static_cast<unsigned long long>(summary.fragment_count),
                 static_cast<unsigned long long>(summary.total_rows), master_out.c_str());
    return 0;
}

#ifndef NANOLANCE_CLI_SUBCOMMAND
// Standalone build of this tool. The `nanolance` binary compiles the same file with
// NANOLANCE_CLI_SUBCOMMAND defined and calls nanolance_cli_stitch from its dispatcher instead.
int main(int argc, char** argv) { return nanolance_cli_stitch(argc, argv); }
#endif
