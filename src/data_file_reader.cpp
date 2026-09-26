// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_reader.hpp"
#include "nanolance/work_stats.hpp"

#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <list>
#include <utility>
#include <vector>

namespace nano_lance {
namespace {

// Small per-thread LRU of open (path -> stream, file_size, mtime), so a column's many page-buffer reads
// reuse one handle instead of re-running std::ifstream's open() (its own stat + buffer/locale setup, far
// heavier than a plain stat) on every single call — mirrors the LRU pattern already used for external
// blob fetches (nano_lance_external_blob.cpp). thread_local, so concurrent reads on different threads
// never share (or contend over) a stream.
//
// A Lance data file is immutable once *committed*, but the exact same path can legitimately be reused by
// a *later, unrelated* file: fragment file names are assigned deterministically from the lowest unused
// suffix (data_file_writer.cpp), so wiping a dataset directory and rewriting it reproduces the very same
// "fragment-0.lance" path with different bytes (this is exactly what nanolance's own test suite does
// across sequential test cases sharing one temp directory). So a lookup re-stats size + mtime and
// reopens if either changed, trading back part of the stat-avoidance win for correctness against file
// replacement, while still skipping the much heavier open() when unchanged.
//
// That re-stat used to run on EVERY lookup, which meant two `stat` syscalls per page-buffer read:
// 19,065 of them (48% of all syscall time) for three reads of a 1M-row, three-column dataset. A
// `DataFileReadScope` marks one read operation, and within it a file is validated on its first
// lookup only — the file cannot become a different file partway through a single read in any way the
// reader could act on. Outside a scope the epoch is 0, which never matches a stamped entry, so every
// lookup validates exactly as it did before.
struct OpenDataFile {
    std::ifstream stream;
    std::uint64_t file_size = 0;
    std::filesystem::file_time_type mtime{};
    /// Read scope this entry was last validated in; 0 means "not validated in any scope".
    std::uint64_t checked_epoch = 0;
    /// Where `stream`'s get pointer is, so a page read that continues where the last one stopped --
    /// which is the common case, pages of a column are laid out in order -- can skip the seek.
    std::uint64_t position = 0;
};
constexpr std::size_t kMaxOpenDataFiles = 4;
thread_local std::list<std::pair<std::filesystem::path, OpenDataFile>> g_open_data_files;
thread_local std::uint64_t g_read_epoch = 0;
thread_local std::uint64_t g_next_read_epoch = 1;

OpenDataFile* find_or_open_data_file(const std::filesystem::path& path, std::string& error) {
    for (auto it = g_open_data_files.begin(); it != g_open_data_files.end(); ++it) {
        if (it->first == path && g_read_epoch != 0 && it->second.checked_epoch == g_read_epoch) {
            g_open_data_files.splice(g_open_data_files.begin(), g_open_data_files, it);
            return &g_open_data_files.front().second;
        }
    }

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "failed to stat data file: " + ec.message();
        return nullptr;
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        error = "failed to stat data file: " + ec.message();
        return nullptr;
    }

    for (auto it = g_open_data_files.begin(); it != g_open_data_files.end(); ++it) {
        if (it->first == path) {
            if (it->second.file_size == static_cast<std::uint64_t>(file_size) && it->second.mtime == mtime) {
                it->second.checked_epoch = g_read_epoch;
                g_open_data_files.splice(g_open_data_files.begin(), g_open_data_files, it);
                return &g_open_data_files.front().second;
            }
            // Stale — same path, different underlying file. Drop it rather than leaving a shadowed,
            // still-open duplicate entry sitting in the LRU until it ages out.
            g_open_data_files.erase(it);
            break;
        }
    }
    OpenDataFile fresh;
    fresh.stream.open(path, std::ios::binary);
    if (!fresh.stream) {
        error = "failed to open data file for read";
        return nullptr;
    }
    fresh.file_size = static_cast<std::uint64_t>(file_size);
    fresh.mtime = mtime;
    fresh.checked_epoch = g_read_epoch;
    g_open_data_files.emplace_front(path, std::move(fresh));
    if (g_open_data_files.size() > kMaxOpenDataFiles) {
        g_open_data_files.pop_back();
    }
    return &g_open_data_files.front().second;
}

bool read_le16(const unsigned char* p, std::uint16_t& v) {
    v = static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8U));
    return true;
}

bool read_le32(const unsigned char* p, std::uint32_t& v) {
    v = static_cast<std::uint32_t>(static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8U) |
                                   (static_cast<unsigned>(p[2]) << 16U) | (static_cast<unsigned>(p[3]) << 24U));
    return true;
}

bool read_le64(const unsigned char* p, std::uint64_t& v) {
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return true;
}

}  // namespace

DataFileReadScope::DataFileReadScope() : saved_(g_read_epoch) { g_read_epoch = g_next_read_epoch++; }

DataFileReadScope::~DataFileReadScope() { g_read_epoch = saved_; }


bool read_lance_data_file_footer_and_descriptor(const std::filesystem::path& path, pb::FileDescriptor& descriptor,
                                                LanceDataFileFooterLayout& layout, std::string& error) {
    descriptor = pb::FileDescriptor{};
    layout = LanceDataFileFooterLayout{};
    error.clear();

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < 64U) {
        error = "data file is too small or unreadable";
        return false;
    }

    // std::min<std::uintmax_t>, explicitly: std::filesystem::file_size returns uintmax_t, and on
    // macOS libc++ that is `unsigned long` while std::uint64_t is `unsigned long long` -- two
    // distinct types, so template argument deduction fails. On Linux/glibc they are the same type
    // and the bare call compiles by coincidence, which is why this only showed up the first time the
    // macOS CI job actually ran.
    constexpr std::uintmax_t kTailBytes = 512;
    const auto tail_len = static_cast<std::streamsize>(std::min<std::uintmax_t>(kTailBytes, file_size));
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open data file";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(file_size - static_cast<std::uintmax_t>(tail_len)));
    std::vector<unsigned char> tail(static_cast<std::size_t>(tail_len));
    in.read(reinterpret_cast<char*>(tail.data()), tail_len);
    if (!in || in.gcount() != tail_len) {
        error = "failed to read data file footer tail";
        return false;
    }

    constexpr unsigned char kMagic[] = {'L', 'A', 'N', 'C'};
    std::size_t magic_idx = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i + 4U <= tail.size(); ++i) {
        if (std::memcmp(tail.data() + i, kMagic, 4) == 0) {
            magic_idx = i;
        }
    }
    if (magic_idx == static_cast<std::size_t>(-1) || magic_idx < 52U) {
        error = "LANC magic not found in data file tail";
        return false;
    }

    std::uint16_t minor = 0;
    std::uint16_t major = 0;
    if (!read_le16(tail.data() + magic_idx - 4U, minor) || !read_le16(tail.data() + magic_idx - 2U, major)) {
        error = "failed to read data file version";
        return false;
    }
    if (minor != 2U || major != 2U) {
        error = "unsupported Lance data file version (expected 2.2)";
        return false;
    }

    std::uint32_t num_columns = 0;
    std::uint32_t const1 = 0;
    if (!read_le32(tail.data() + magic_idx - 8U, num_columns) || !read_le32(tail.data() + magic_idx - 12U, const1)) {
        error = "failed to read data file column counts";
        return false;
    }
    if (const1 != 1U) {
        error = "unexpected Lance data file footer constant";
        return false;
    }

    const std::size_t u64_block = magic_idx - 52U;
    std::uint64_t global_buffer_offset = 0;
    std::uint64_t descriptor_size = 0;
    std::uint64_t column_metadata_start = 0;
    std::uint64_t column_offsets_start = 0;
    std::uint64_t global_offsets_start = 0;
    if (!read_le64(tail.data() + u64_block + 0U, global_buffer_offset) ||
        !read_le64(tail.data() + u64_block + 8U, descriptor_size) ||
        !read_le64(tail.data() + u64_block + 16U, column_metadata_start) ||
        !read_le64(tail.data() + u64_block + 24U, column_offsets_start) ||
        !read_le64(tail.data() + u64_block + 32U, global_offsets_start)) {
        error = "failed to read data file footer offset block";
        return false;
    }

    const std::uint64_t tail_base = file_size - static_cast<std::uint64_t>(tail_len);
    const std::uint64_t abs_u64_block = tail_base + u64_block;
    if (global_offsets_start != abs_u64_block) {
        error = "data file footer global_offsets_start mismatch";
        return false;
    }
    if (!fits_size_t(descriptor_size) || !range_in_bounds(global_buffer_offset, descriptor_size, file_size)) {
        error = "invalid data file descriptor bounds";
        return false;
    }
    if (num_columns > default_read_limits().max_columns) {
        error = "data file column count exceeds safety limit";
        return false;
    }

    in.clear();
    in.seekg(static_cast<std::streamoff>(global_buffer_offset));
    std::vector<std::uint8_t> desc_bytes(static_cast<std::size_t>(descriptor_size));
    in.read(reinterpret_cast<char*>(desc_bytes.data()), static_cast<std::streamsize>(descriptor_size));
    if (!in || static_cast<std::size_t>(in.gcount()) != static_cast<std::size_t>(descriptor_size)) {
        error = "failed to read data file descriptor bytes";
        return false;
    }
    if (!pb::decode_file_descriptor(desc_bytes, descriptor)) {
        error = "failed to decode data file protobuf descriptor";
        return false;
    }

    layout.global_buffer_offset = global_buffer_offset;
    layout.descriptor_size = descriptor_size;
    layout.column_metadata_start = column_metadata_start;
    layout.column_offsets_start = column_offsets_start;
    layout.global_offsets_start = global_offsets_start;
    layout.num_columns = num_columns;
    return true;
}

bool read_lance_data_file_bytes(const std::filesystem::path& path, const std::uint64_t offset,
                                const std::uint64_t size, std::vector<std::uint8_t>& out, std::string& error) {
    // Deliberately no `out.clear()` here: callers commonly reuse the same `out` vector across many
    // page reads in a loop, and `resize()` below both sets the exact final size and, when reused across
    // calls of similar size, avoids re-zeroing bytes the very next line's ifstream::read() is about to
    // overwrite anyway (clear()+resize() would force a fresh zero-fill of the whole buffer every call).
    error.clear();
    if (!fits_size_t(size)) {
        error = "read size overflow";
        return false;
    }
    auto* file = find_or_open_data_file(path, error);
    if (file == nullptr) {
        return false;
    }
    if (!range_in_bounds(offset, size, file->file_size)) {
        error = "read range exceeds data file size";
        return false;
    }
    auto& in = file->stream;
    in.clear();  // a previous read on this cached stream may have set eof/fail; clear before repositioning
    // Skip the seek when the stream is already there. libstdc++'s seekg() discards the filebuf and
    // always issues an lseek, and a column's pages are read in file order, so most of those seeks were
    // asking the kernel to move the offset to where it already was.
    if (file->position != offset) {
        in.seekg(static_cast<std::streamoff>(offset));
        file->position = offset;
    }
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    {
        auto& stats = work_stats::counters();
        work_stats::add(stats.data_bytes_read, size);
        work_stats::add(stats.data_reads, 1U);
        work_stats::raise_to(stats.largest_read, size);
    }
    if (!in || static_cast<std::uint64_t>(in.gcount()) != size) {
        error = "failed to read data file byte range";
        // The stream's position is now wherever the short read left it, and `in` is in a failed state
        // that the next call clears — forget it rather than trusting the cached value.
        file->position = std::numeric_limits<std::uint64_t>::max();
        out.clear();
        return false;
    }
    file->position = offset + size;
    return true;
}

bool read_lance_data_file_column_metadatas(const std::filesystem::path& path,
                                           const LanceDataFileFooterLayout& layout,
                                           std::vector<pb::ColumnMetadata>& columns, std::string& error) {
    error.clear();
    columns.clear();
    if (layout.num_columns == 0U) {
        return true;
    }
    const auto table_bytes = static_cast<std::uint64_t>(layout.num_columns) * 16U;
    std::vector<std::uint8_t> table;
    if (!read_lance_data_file_bytes(path, layout.column_offsets_start, table_bytes, table, error)) {
        return false;
    }
    columns.resize(layout.num_columns);
    for (std::uint32_t col = 0; col < layout.num_columns; ++col) {
        const auto base = static_cast<std::size_t>(col) * 16U;
        std::uint64_t meta_offset = 0;
        std::uint64_t meta_size = 0;
        if (!read_le64(table.data() + base, meta_offset) || !read_le64(table.data() + base + 8U, meta_size)) {
            error = "failed to parse column metadata offset table";
            return false;
        }
        std::vector<std::uint8_t> meta_bytes;
        if (!read_lance_data_file_bytes(path, meta_offset, meta_size, meta_bytes, error)) {
            return false;
        }
        if (!pb::decode_column_metadata(meta_bytes, columns[col])) {
            error = "failed to decode column metadata for column " + std::to_string(col);
            return false;
        }
    }
    return true;
}

}  // namespace nano_lance
