// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlbench: time nanolance's native full-scan read of a dataset, or its write of an Arrow IPC stream.
// Usage: nlbench <dataset.lance> [iters] [--trusted]
//   -> prints rows, avg/best/median read milliseconds and peak_rss_mb (JSON): the most memory one run
//      added over what the process held before it, the returned table included. --trusted reads with trusted_input=true
//      (skips the untrusted-input DoS/OOM budget checks; bounds checks are unconditional and always run)
//      -- used to produce the safety-vs-trusted parity table in docs/SAFETY.md (bench/read_parity_bench.sh).
// Usage: nlbench --write <in.arrow> <out.lance> [iters] [--budget BYTES]
//   -> loads the IPC stream's batches into memory once, then writes them to a fresh dataset `iters`
//      times in this process (write_batch per batch + commit), and prints the same JSON. Timing the
//      writes in one process, like the reads, keeps process start-up and a cold heap out of the number.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// A field of /proc/self/status in KiB (0 where there is none: not Linux).
std::uint64_t status_kb(const char* field) {
    std::ifstream status("/proc/self/status");
    std::string line;
    const std::string key = field;
    while (std::getline(status, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            std::istringstream fields(line.substr(key.size()));
            std::uint64_t kb = 0;
            fields >> kb;
            return kb;
        }
    }
    return 0;
}

/// Reset the kernel's high-water mark (VmHWM) to the current RSS -- Linux: 5 to clear_refs -- and
/// return that baseline in KiB. peak_added_kb() after a run is then the most it added.
std::uint64_t reset_peak() {
    std::ofstream("/proc/self/clear_refs") << "5";
    return status_kb("VmRSS:");
}

std::uint64_t peak_added_kb(std::uint64_t baseline) {
    const auto hwm = status_kb("VmHWM:");
    return hwm > baseline ? hwm - baseline : 0;
}

std::uint64_t g_peak_kb = 0;  // the largest peak_added_kb() of any run, printed as peak_rss_mb

void print_times(std::int64_t rows, bool trusted_input, std::vector<double> all_ms) {
    // warm_median_ms leaves the first run out: it is the warm-up (cold page cache, cold heap).
    std::vector<double> warm(all_ms.size() > 1U ? all_ms.begin() + 1 : all_ms.begin(), all_ms.end());
    std::sort(warm.begin(), warm.end());
    const double warm_median_ms = warm.empty() ? 0.0 : warm[warm.size() / 2];
    std::sort(all_ms.begin(), all_ms.end());
    double total_ms = 0.0;
    for (const double ms : all_ms) {
        total_ms += ms;
    }
    const double best_ms = all_ms.empty() ? 0.0 : all_ms.front();
    const double median_ms = all_ms.empty() ? 0.0 : all_ms[all_ms.size() / 2];
    const double avg_ms = all_ms.empty() ? 0.0 : total_ms / static_cast<double>(all_ms.size());
    std::cout << "{\"rows\": " << rows << ", \"trusted_input\": " << (trusted_input ? "true" : "false")
              << ", \"best_ms\": " << best_ms << ", \"avg_ms\": " << avg_ms << ", \"median_ms\": " << median_ms
              << ", \"warm_median_ms\": " << warm_median_ms
              << ", \"peak_rss_mb\": " << static_cast<double>(g_peak_kb) / 1024.0 << "}\n";
}

int bench_write(const std::string& in_path, const std::string& out_path, int iters, std::uint64_t budget) {
    FILE* file = std::fopen(in_path.c_str(), "rb");
    if (file == nullptr) {
        std::cerr << "cannot open " << in_path << '\n';
        return 1;
    }
    ArrowIpcInputStream input{};
    if (ArrowIpcInputStreamInitFile(&input, file, 1) != NANOARROW_OK) {
        std::fclose(file);
        std::cerr << "cannot read " << in_path << '\n';
        return 1;
    }
    ArrowArrayStream stream{};
    if (ArrowIpcArrayStreamReaderInit(&stream, &input, nullptr) != NANOARROW_OK) {
        input.release(&input);
        std::cerr << "not an Arrow IPC stream: " << in_path << '\n';
        return 1;
    }
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::int64_t rows = 0;
    bool ok = stream.get_schema(&stream, &schema) == NANOARROW_OK;
    while (ok) {
        ArrowArray batch{};
        ok = stream.get_next(&stream, &batch) == NANOARROW_OK;
        if (!ok || batch.release == nullptr) {
            break;
        }
        rows += batch.length;
        batches.push_back(batch);
    }
    stream.release(&stream);
    std::vector<double> all_ms;
    int status = ok ? 0 : 1;
    if (!ok) {
        std::cerr << "IPC read failed: " << in_path << '\n';
    }
    for (int it = 0; status == 0 && it < iters; ++it) {
        std::filesystem::remove_all(out_path);
        const auto baseline = reset_peak();
        const auto t0 = std::chrono::steady_clock::now();
        NanoLanceWriter writer{};
        int rc = nano_lance_writer_init(&writer, out_path.c_str(), 3);
        if (rc == NANO_LANCE_OK && budget != 0U) {
            rc = nano_lance_writer_set_max_pending_bytes(&writer, budget);
        }
        for (std::size_t b = 0; rc == NANO_LANCE_OK && b < batches.size(); ++b) {
            rc = nano_lance_write_batch(&writer, &batches[b], &schema);
        }
        if (rc == NANO_LANCE_OK) {
            rc = nano_lance_writer_commit(&writer, false);
        }
        if (rc != NANO_LANCE_OK) {
            std::cerr << "write failed: " << nano_lance_writer_last_error(&writer) << '\n';
            status = 1;
        }
        nano_lance_writer_close(&writer);
        const auto t1 = std::chrono::steady_clock::now();
        all_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        g_peak_kb = std::max(g_peak_kb, peak_added_kb(baseline));
    }
    for (auto& b : batches) {
        ArrowArrayRelease(&b);
    }
    if (schema.release != nullptr) {
        ArrowSchemaRelease(&schema);
    }
    if (status == 0) {
        print_times(rows, false, all_ms);
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: nlbench <dataset.lance> [iters] [--trusted]\n"
                     "       nlbench --write <in.arrow> <out.lance> [iters] [--budget BYTES]\n";
        return 2;
    }
    if (std::strcmp(argv[1], "--write") == 0) {
        if (argc < 4) {
            std::cerr << "usage: nlbench --write <in.arrow> <out.lance> [iters] [--budget BYTES]\n";
            return 2;
        }
        int iters = 5;
        std::uint64_t budget = 0;
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
                budget = std::strtoull(argv[++i], nullptr, 10);
            } else {
                iters = std::atoi(argv[i]);
            }
        }
        return bench_write(argv[2], argv[3], iters, budget);
    }
    const std::string path = argv[1];
    int iters = 5;
    bool trusted_input = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trusted") == 0) {
            trusted_input = true;
        } else {
            iters = std::atoi(argv[i]);
        }
    }

    std::int64_t rows = 0;
    std::vector<double> all_ms;
    for (int it = 0; it < iters; ++it) {
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        const auto baseline = reset_peak();
        const auto t0 = std::chrono::steady_clock::now();
        if (!nano_lance::lance_table_read_dataset(path, schema, batches, error, trusted_input)) {
            std::cerr << "read failed: " << error << '\n';
            return 1;
        }
        const auto t1 = std::chrono::steady_clock::now();
        rows = 0;
        for (auto& b : batches) {
            rows += b.length;
        }
        all_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        g_peak_kb = std::max(g_peak_kb, peak_added_kb(baseline));  // the returned table still held
        ArrowSchemaRelease(&schema);
        for (auto& b : batches) {
            ArrowArrayRelease(&b);
        }
    }
    print_times(rows, trusted_input, all_ms);
    return 0;
}
