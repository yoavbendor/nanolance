// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The work counters (include/nanolance/work_stats.hpp) that tests use to check HOW a result was produced:
// each counter moves by what is counted, raise_to keeps the largest, reset() clears the work counters but
// not the I/O counters (a dataset reports differences of those), a written file is counted by its size once
// it is complete, and the index cache hit/miss counters.

#include "nanolance/work_stats.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

namespace ws = nano_lance::work_stats;

}  // namespace

int main() {
    int checks = 0;

    // reset() zeroes every work counter.
    {
        auto& c = ws::counters();
        ws::add(c.data_bytes_read, 10);
        ws::add(c.data_reads, 2);
        ws::add(c.fragment_reads, 3);
        ws::add(c.indexed_fragments, 4);
        ws::reset();
        require(c.data_bytes_read.load() == 0 && c.data_reads.load() == 0 && c.largest_read.load() == 0 &&
                    c.page_windows.load() == 0 && c.read_morsels.load() == 0 && c.fragment_reads.load() == 0 &&
                    c.parallel_column_writes.load() == 0 && c.write_buffered_bytes.load() == 0 &&
                    c.buffer_pool_hits.load() == 0 && c.buffer_pool_misses.load() == 0 &&
                    c.take_cache_hits.load() == 0 && c.indexed_fragments.load() == 0,
                "reset() clears every work counter");
        checks += 1;
    }

    // add() moves a counter by exactly what it is given, and reset() brings it back to zero.
    {
        auto& c = ws::counters();
        ws::reset();
        ws::add(c.data_bytes_read, 4096);
        ws::add(c.data_bytes_read, 4096);
        require(c.data_bytes_read.load() == 8192, "add() accumulates");
        ws::add(c.page_windows, 1);
        require(c.page_windows.load() == 1 && c.data_bytes_read.load() == 8192, "add() touches only its counter");
        ws::reset();
        checks += 2;
    }

    // raise_to() keeps the largest value seen: a smaller value does not lower it.
    {
        auto& c = ws::counters();
        ws::reset();
        ws::raise_to(c.largest_read, 10);
        require(c.largest_read.load() == 10, "raise_to() sets a larger value");
        ws::raise_to(c.largest_read, 5);
        require(c.largest_read.load() == 10, "raise_to() does not lower the maximum");
        ws::raise_to(c.largest_read, 20);
        require(c.largest_read.load() == 20, "raise_to() raises the maximum");
        ws::raise_to(c.largest_read, 20);
        require(c.largest_read.load() == 20, "an equal value leaves the maximum unchanged");
        checks += 4;
    }

    // I/O counters: count_read and count_write add one operation and its bytes. They are never reset, so
    // the test reads differences.
    {
        auto& io = ws::io_counters();
        const auto r_iops = io.read_iops.load();
        const auto r_bytes = io.read_bytes.load();
        ws::count_read(100);
        ws::count_read(28);
        require(io.read_iops.load() - r_iops == 2, "two reads are two read operations");
        require(io.read_bytes.load() - r_bytes == 128, "read bytes are summed");

        const auto w_iops = io.write_iops.load();
        const auto w_bytes = io.written_bytes.load();
        ws::count_write(50);
        require(io.write_iops.load() - w_iops == 1, "one write is one write operation");
        require(io.written_bytes.load() - w_bytes == 50, "written bytes are summed");

        ws::reset();  // the work counters only
        require(io.read_bytes.load() - r_bytes == 128, "reset() leaves the I/O counters alone");
        checks += 4;
    }

    // A file is counted by its size once it is complete; a file that does not exist counts nothing.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("nano_lance_work_stats_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "written.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << std::string(1234, 'x');
    }
    {
        auto& io = ws::io_counters();
        const auto w_iops = io.write_iops.load();
        const auto w_bytes = io.written_bytes.load();
        ws::count_written_file(file);
        require(io.write_iops.load() - w_iops == 1, "a written file is one write operation");
        require(io.written_bytes.load() - w_bytes == 1234, "a written file counts its size");

        ws::count_written_file(dir / "missing.bin");
        require(io.write_iops.load() - w_iops == 1 && io.written_bytes.load() - w_bytes == 1234,
                "a missing file counts nothing");
        checks += 3;
    }

    // WrittenFileTally counts the file when it goes out of scope, by the size it has then.
    {
        auto& io = ws::io_counters();
        const auto w_iops = io.write_iops.load();
        const auto w_bytes = io.written_bytes.load();
        const auto tally_file = dir / "tally.bin";
        {
            ws::WrittenFileTally tally(tally_file);  // declared first, so destroyed (counted) last
            std::ofstream out(tally_file, std::ios::binary);
            out << std::string(77, 'y');
        }
        require(io.write_iops.load() - w_iops == 1, "the tally counts one write when it ends");
        require(io.written_bytes.load() - w_bytes == 77, "the tally counts the complete file's size");
        checks += 2;
    }

    // Index cache counters: a hit and a miss each move their own counter by one.
    {
        auto& c = ws::index_cache_counters();
        const auto hits = c.hits.load();
        const auto misses = c.misses.load();
        ws::count_index_cache(true);
        ws::count_index_cache(true);
        ws::count_index_cache(false);
        require(c.hits.load() - hits == 2, "two hits are counted as hits");
        require(c.misses.load() - misses == 1, "one miss is counted as a miss");
        checks += 2;
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "work_stats: " << checks << " checks passed\n";
    return 0;
}
