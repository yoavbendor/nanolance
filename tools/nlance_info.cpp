// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlance_info — print fragment/chunk statistics for a Lance dataset.
//
// Usage:
//   nlance_info <dataset.lance>
//
// Output (to stdout):
//   One line per fragment: id, physical_rows, data_file size(s).
//   Summary: total fragments, total rows, min/max/median rows-per-fragment.
//
// This is the fastest way to see why fuselance (or any batch reader) is slow:
// a single giant fragment means the whole table is read as one batch before
// any FUSE operation can be served.

#include "nanolance/nano_lance_reader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

static const char* human_size(uint64_t bytes, char* buf, size_t n) {
    if (bytes >= 1024ULL*1024*1024)
        std::snprintf(buf, n, "%.1f GiB", static_cast<double>(bytes) / (1024.*1024*1024));
    else if (bytes >= 1024ULL*1024)
        std::snprintf(buf, n, "%.1f MiB", static_cast<double>(bytes) / (1024.*1024));
    else if (bytes >= 1024ULL)
        std::snprintf(buf, n, "%.1f KiB", static_cast<double>(bytes) / 1024.);
    else
        std::snprintf(buf, n, "%" PRIu64 " B", bytes);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        std::fprintf(stderr,
            "Usage: nlance_info <dataset.lance>\n\n"
            "Print fragment/chunk statistics: rows per fragment, data file sizes.\n"
            "Useful for diagnosing why fuselance startup is slow (one huge fragment\n"
            "= one giant batch that must be read before any file is accessible).\n");
        return argc < 2 ? 1 : 0;
    }

    NanoLanceDatasetMetadata meta{};
    char err[512];
    if (nano_lance_dataset_read_latest(argv[1], &meta, err, sizeof(err)) != NANO_LANCE_READER_OK) {
        std::fprintf(stderr, "nlance_info: %s\n", err);
        return 1;
    }

    std::fprintf(stdout,
        "Dataset:         %s\n"
        "Manifest v%llu    format: %s %s\n"
        "Total rows:      %" PRIu64 "\n"
        "Fragments:       %zu\n"
        "Schema fields:   %zu\n"
        "\n",
        argv[1],
        static_cast<unsigned long long>(meta.manifest_version),
        meta.file_format  ? meta.file_format  : "?",
        meta.format_version ? meta.format_version : "?",
        static_cast<uint64_t>(meta.total_physical_rows),
        meta.fragments_len,
        meta.fields_len);

    // Per-fragment table.
    std::vector<uint64_t> row_counts;
    row_counts.reserve(meta.fragments_len);

    const size_t print_limit = 200;  // don't flood terminal for huge datasets
    if (meta.fragments_len > print_limit)
        std::fprintf(stdout, "(showing first %zu of %zu fragments)\n\n", print_limit, meta.fragments_len);

    std::fprintf(stdout, "%-8s  %-12s  %s\n", "Frag ID", "Rows", "Data files");
    std::fprintf(stdout, "%-8s  %-12s  %s\n", "-------", "----", "----------");

    for (size_t fi = 0; fi < meta.fragments_len; ++fi) {
        const NanoLanceReaderFragment& frag = meta.fragments[fi];
        row_counts.push_back(frag.physical_rows);

        if (fi >= print_limit) continue;

        // Collect total data size across all data files in this fragment.
        uint64_t total_bytes = 0;
        for (size_t di = 0; di < frag.files_len; ++di)
            total_bytes += frag.files[di].file_size_bytes;

        char hs[32];
        std::fprintf(stdout, "%-8" PRIu64 "  %-12" PRIu64 "  %s",
                     static_cast<uint64_t>(frag.id),
                     static_cast<uint64_t>(frag.physical_rows),
                     human_size(total_bytes, hs, sizeof(hs)));

        if (frag.files_len > 1) {
            // Show per-file breakdown when there are multiple data files.
            std::fprintf(stdout, " (");
            for (size_t di = 0; di < frag.files_len; ++di) {
                if (di) std::fprintf(stdout, " + ");
                std::fprintf(stdout, "%s", human_size(frag.files[di].file_size_bytes, hs, sizeof(hs)));
            }
            std::fprintf(stdout, ")");
        }
        std::fprintf(stdout, "\n");
    }

    // Summary statistics.
    if (!row_counts.empty()) {
        std::sort(row_counts.begin(), row_counts.end());
        uint64_t total  = std::accumulate(row_counts.begin(), row_counts.end(), uint64_t{0});
        uint64_t minr   = row_counts.front();
        uint64_t maxr   = row_counts.back();
        uint64_t median = row_counts[row_counts.size() / 2];
        double   avg    = static_cast<double>(total) / static_cast<double>(row_counts.size());

        std::fprintf(stdout,
            "\n--- rows per fragment ---\n"
            "  min:    %" PRIu64 "\n"
            "  median: %" PRIu64 "\n"
            "  avg:    %.0f\n"
            "  max:    %" PRIu64 "\n",
            minr, median, avg, maxr);

        if (maxr > 100000)
            std::fprintf(stdout,
                "\nHint: max fragment has %" PRIu64 " rows — fuselance loads each fragment as one\n"
                "      batch; large fragments cause slow startup. Re-ingest with smaller batches\n"
                "      (e.g. arrowipc2lance --rows-per-fragment 10000) to fix this.\n", maxr);
    }

    nano_lance_dataset_metadata_free(&meta);
    return 0;
}
