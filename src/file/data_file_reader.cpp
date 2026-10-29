// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_reader.hpp"
#include "nanolance/byte_sink.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/byte_source.hpp"
#include "nanolance/work_stats.hpp"

#include "nanolance/read_safety.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <list>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

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
/// A local file's modification time is only as fine as its filesystem's clock: some network and FAT
/// filesystems stamp to one or two seconds, so a rewrite that keeps the size can keep the timestamp.
/// A cached entry whose file was modified within this window of when the entry was filled cannot be
/// trusted to match the file, so it is reopened (git's "racy timestamp" rule).
constexpr std::int64_t kRacyWindowNs = 2'000'000'000LL;

std::int64_t now_unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// A local version token (io::stat_local) as nanoseconds since the Unix epoch. POSIX tokens already are;
/// Windows tokens are FILETIME (100 ns ticks since 1601).
std::int64_t version_unix_ns(std::int64_t version) {
#if defined(_WIN32)
    constexpr std::int64_t kUnixEpochTicks = 116444736000000000LL;
    return (version - kUnixEpochTicks) * 100;
#else
    return version;
#endif
}

struct OpenDataFile {
    std::unique_ptr<io::ByteSource> source;
    std::uint64_t file_size = 0;
    /// Read scope this entry was last validated in; 0 means "not validated in any scope".
    std::uint64_t checked_epoch = 0;
    /// When the entry was filled (before its stat, so no write before the stat can hide in the gap).
    std::int64_t filled_unix_ns = 0;
    /// The file's last bytes, read once by the footer reader: the footer, the file descriptor and,
    /// in most files, every column's metadata live there, so those reads become copies from memory
    /// (and, from a remote source, one request instead of one per column).
    std::vector<std::uint8_t> tail;
    std::uint64_t tail_start = std::numeric_limits<std::uint64_t>::max();
    /// The last block a small read came from: small reads next to each other (a page's buffers, many
    /// columns' small pages) are copies from it, as they were from the old stream buffer.
    std::vector<std::uint8_t> block;
    std::uint64_t block_start = std::numeric_limits<std::uint64_t>::max();
};
constexpr std::size_t kMaxOpenDataFiles = 4;

/// One thread's open files. Its lock is held by its own thread for the length of each public read
/// below (uncontended: a few nanoseconds next to a pread), and by close_data_files_under() from the
/// thread that deleted files, which so never closes a handle another thread is reading through. Every
/// thread's cache is registered so that a delete can reach threads that are idle: a deleted file held
/// open stays on disk (on NFS as a .nfsXXXX file) until the handle closes.
struct ThreadOpenFiles {
    std::recursive_mutex mutex;
    std::list<std::pair<std::filesystem::path, OpenDataFile>> files;
    ThreadOpenFiles();
    ~ThreadOpenFiles();
};

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<ThreadOpenFiles*>& registry() {
    static std::vector<ThreadOpenFiles*> r;
    return r;
}

/// Whether this thread's g_thread_files has been constructed (and not destroyed): the fork handler below
/// must not touch it otherwise, since naming a thread_local constructs it.
thread_local bool t_thread_files_live = false;

ThreadOpenFiles::ThreadOpenFiles() {
    const std::lock_guard<std::mutex> lock(registry_mutex());
    registry().push_back(this);
    t_thread_files_live = true;
}

ThreadOpenFiles::~ThreadOpenFiles() {
    const std::lock_guard<std::mutex> lock(registry_mutex());
    auto& r = registry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
    t_thread_files_live = false;
}

thread_local ThreadOpenFiles g_thread_files;

#if defined(__unix__) || defined(__APPLE__)
// A forked child has only the thread that called fork, but inherits the registry's pointers to every
// other thread's cache. Those threads' stacks and thread-local storage are reused by the child's next
// threads (a pool of its own, parallel.cpp), so the pointers dangle: close_data_files_under then locked
// a "mutex" made of whatever the new thread wrote there, and the child hung, aborted in glibc or crashed
// (tests/test_concurrent_writers.py::test_processes, forking writers). The registry lock is taken across
// fork so the copy is consistent; the child keeps only the forking thread's cache. The other threads'
// caches are dropped without closing their handles: their memory is no longer theirs to read.
void before_fork() { registry_mutex().lock(); }
void after_fork_in_parent() { registry_mutex().unlock(); }
void after_fork_in_child() {
    registry().clear();
    if (t_thread_files_live) {
        registry().push_back(&g_thread_files);
    }
    registry_mutex().unlock();
}
[[maybe_unused]] const bool g_fork_handlers_set =
    (pthread_atfork(&before_fork, &after_fork_in_parent, &after_fork_in_child), true);
#endif
thread_local std::list<std::pair<std::filesystem::path, OpenDataFile>>& g_open_data_files = g_thread_files.files;
thread_local std::uint64_t g_read_epoch = 0;
thread_local std::uint64_t g_next_read_epoch = 1;

OpenDataFile* find_or_open_data_file(const std::filesystem::path& path, std::string& error) {
    for (auto it = g_open_data_files.begin(); it != g_open_data_files.end(); ++it) {
        if (it->first == path && g_read_epoch != 0 && it->second.checked_epoch == g_read_epoch) {
            g_open_data_files.splice(g_open_data_files.begin(), g_open_data_files, it);
            return &g_open_data_files.front().second;
        }
    }

    // One stat of the path per read scope: a file replaced at the same path (a new inode) must not be
    // read through the old handle.
    const std::int64_t filled_at = now_unix_ns();
    std::uint64_t file_size = 0;
    std::int64_t version = 0;
    const bool local = !io::is_remote(path.string());
    if (local && !io::stat_local(path.string(), file_size, version)) {
        std::error_code ec;
        error = std::filesystem::exists(path, ec) ? "failed to stat data file " + path.string()
                                                  : "data file not found: " + path.string();
        return nullptr;
    }

    for (auto it = g_open_data_files.begin(); it != g_open_data_files.end(); ++it) {
        if (it->first == path) {
            // Trusted only if the size and timestamp match AND the entry was filled after the file's last
            // change could have been stamped (see kRacyWindowNs).
            const bool racy = local && version_unix_ns(it->second.source->version_token()) >=
                                           it->second.filled_unix_ns - kRacyWindowNs;
            if (!local || (it->second.file_size == file_size && it->second.source->version_token() == version && !racy)) {
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
    fresh.source = io::open_source(path.string(), error);
    if (fresh.source == nullptr) {
        return nullptr;
    }
    fresh.file_size = fresh.source->size();
    fresh.checked_epoch = g_read_epoch;
    fresh.filled_unix_ns = filled_at;
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

bool data_file_identity(const std::filesystem::path& path, std::uint64_t& size, std::int64_t& version) {
    const std::lock_guard<std::recursive_mutex> lock(g_thread_files.mutex);
    std::string error;
    auto* file = find_or_open_data_file(path, error);
    if (file == nullptr) {
        return false;
    }
    size = file->file_size;
    version = file->source->version_token();
    return true;
}

void close_data_files_under(const std::string& path_or_prefix) {
    const std::lock_guard<std::mutex> registry_lock(registry_mutex());
    for (ThreadOpenFiles* cache : registry()) {
        const std::lock_guard<std::recursive_mutex> lock(cache->mutex);
        cache->files.remove_if([&](const auto& entry) {
            const std::string p = entry.first.string();
            return p.size() >= path_or_prefix.size() && p.compare(0, path_or_prefix.size(), path_or_prefix) == 0;
        });
    }
}

namespace {
// remove_output (in the library below this one) closes the handles of what it removes through this hook.
[[maybe_unused]] const bool g_output_removed_hook_set = (io::set_output_removed_hook(&close_data_files_under), true);

// Pool threads run their share of a parallel read inside a read scope of their own (parallel.hpp is below
// this layer, so it is registered rather than called).
void run_in_read_scope(const std::function<void()>& work) {
    const DataFileReadScope scope;
    work();
}
[[maybe_unused]] const bool g_worker_scope_set = (parallel::set_worker_scope(&run_in_read_scope), true);
}  // namespace

DataFileReadScope::DataFileReadScope() : saved_(g_read_epoch) { g_read_epoch = g_next_read_epoch++; }

DataFileReadScope::~DataFileReadScope() { g_read_epoch = saved_; }


bool read_lance_data_file_footer_and_descriptor(const std::filesystem::path& path, pb::FileDescriptor& descriptor,
                                                LanceDataFileFooterLayout& layout, std::string& error) {
    descriptor = pb::FileDescriptor{};
    layout = LanceDataFileFooterLayout{};
    error.clear();
    const std::lock_guard<std::recursive_mutex> lock(g_thread_files.mutex);

    auto* file = find_or_open_data_file(path, error);
    if (file == nullptr) {
        // Keep why: a file that cleanup removed while it was being opened is "not found", which a reader
        // racing a cleanup expects (and pylance says the same).
        if (error.empty()) {
            error = "data file is too small or unreadable";
        }
        return false;
    }
    const std::uint64_t file_size = file->file_size;
    if (file_size < 64U) {
        error = "data file is too small or unreadable";
        return false;
    }
    // One read of the file's end (its source says how much; all of a smaller file) serves the footer,
    // the descriptor and often every column's metadata.
    if (file->tail_start == std::numeric_limits<std::uint64_t>::max()) {
        const auto len = std::min<std::uint64_t>(std::max<std::uint64_t>(file->source->footer_read_size(), 512U), file_size);
        file->tail.resize(static_cast<std::size_t>(len));
        if (!file->source->read_at(file_size - len, len, file->tail.data(), error)) {
            file->tail.clear();
            error = "failed to read data file footer tail";
            return false;
        }
        work_stats::count_read(len);
        file->tail_start = file_size - len;
    }
    const std::vector<std::uint8_t>& tail = file->tail;
    const auto tail_len = static_cast<std::uint64_t>(tail.size());

    // The footer ends the file; its magic is in the last 512 bytes, where it was looked for before.
    constexpr unsigned char kMagic[] = {'L', 'A', 'N', 'C'};
    std::size_t magic_idx = static_cast<std::size_t>(-1);
    const std::size_t search_from = tail.size() > 512U ? tail.size() - 512U : 0U;
    for (std::size_t i = search_from; i + 4U <= tail.size(); ++i) {
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
    // Major first, then minor, then the magic (lance-file's footer; its version tests spell it out).
    if (!read_le16(tail.data() + magic_idx - 4U, major) || !read_le16(tail.data() + magic_idx - 2U, minor)) {
        error = "failed to read data file version";
        return false;
    }
    // 2.1 and 2.2 share the structural page layouts; 2.2 adds encodings a 2.1 file never holds. 2.0
    // (footer 0.3: ArrayEncoding pages, lance_v20_decoder.cpp) has the same container. 2.3 and the
    // legacy v1 format are not read.
    const bool v2_0 = major == 0U && minor == 3U;
    if (!v2_0 && (major != 2U || (minor != 1U && minor != 2U))) {
        // The footer spells format 2.0 as 0.3, and the legacy (v1) format as 0.1 or 0.2.
        const std::string footer = std::to_string(major) + "." + std::to_string(minor);
        const std::string format = major == 0U && minor == 3U                  ? "2.0 (footer 0.3)"
                                   : major == 0U && (minor == 1U || minor == 2U) ? "legacy v1 (footer " + footer + ")"
                                                                               : footer;
        error = "unsupported Lance file format " + format + " (nanolance reads 2.0, 2.1 and 2.2)";
        return false;
    }

    std::uint32_t num_columns = 0;
    std::uint32_t num_global_buffers = 0;
    if (!read_le32(tail.data() + magic_idx - 8U, num_columns) ||
        !read_le32(tail.data() + magic_idx - 12U, num_global_buffers)) {
        error = "failed to read data file column counts";
        return false;
    }
    // Global buffer 0 is the schema; Lance's index files add more (a label list's null rows).
    if (num_global_buffers == 0U || num_global_buffers > 1024U) {
        error = "unexpected Lance data file global buffer count";
        return false;
    }

    std::uint64_t column_metadata_start = 0;
    std::uint64_t column_offsets_start = 0;
    std::uint64_t global_offsets_start = 0;
    if (!read_le64(tail.data() + magic_idx - 36U, column_metadata_start) ||
        !read_le64(tail.data() + magic_idx - 28U, column_offsets_start) ||
        !read_le64(tail.data() + magic_idx - 20U, global_offsets_start)) {
        error = "failed to read data file footer offset block";
        return false;
    }
    // The global buffer table sits right before those offsets, as every Lance writer lays it out.
    const std::uint64_t tail_base = file_size - tail_len;
    const std::uint64_t table_bytes = 16ULL * num_global_buffers;
    if (magic_idx < 36U + table_bytes || global_offsets_start != tail_base + magic_idx - 36U - table_bytes) {
        error = "data file footer global_offsets_start mismatch";
        return false;
    }
    const std::size_t u64_block = magic_idx - 36U - static_cast<std::size_t>(table_bytes);
    std::uint64_t global_buffer_offset = 0;
    std::uint64_t descriptor_size = 0;
    if (!read_le64(tail.data() + u64_block, global_buffer_offset) ||
        !read_le64(tail.data() + u64_block + 8U, descriptor_size)) {
        error = "failed to read data file footer offset block";
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

    std::vector<std::uint8_t> desc_bytes;
    if (!read_lance_data_file_bytes(path, global_buffer_offset, descriptor_size, desc_bytes, error)) {
        error = "failed to read data file descriptor bytes";
        return false;
    }
    if (!pb::decode_file_descriptor(desc_bytes, descriptor)) {
        error = "failed to decode data file protobuf descriptor";
        return false;
    }

    layout.major_version = major;
    layout.minor_version = minor;
    layout.global_buffer_offset = global_buffer_offset;
    layout.descriptor_size = descriptor_size;
    layout.column_metadata_start = column_metadata_start;
    layout.column_offsets_start = column_offsets_start;
    layout.global_offsets_start = global_offsets_start;
    layout.num_columns = num_columns;
    layout.num_global_buffers = num_global_buffers;
    return true;
}

bool read_lance_file_global_buffer(const std::filesystem::path& path, const LanceDataFileFooterLayout& layout,
                                   std::uint32_t index, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (index >= layout.num_global_buffers) {
        error = "the file has no global buffer " + std::to_string(index);
        return false;
    }
    std::vector<std::uint8_t> entry;
    if (!read_lance_data_file_bytes(path, layout.global_offsets_start + 16ULL * index, 16U, entry, error)) {
        return false;
    }
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    if (!read_le64(entry.data(), offset) || !read_le64(entry.data() + 8U, size)) {
        error = "failed to read a global buffer entry";
        return false;
    }
    return read_lance_data_file_bytes(path, offset, size, out, error);
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
    const std::lock_guard<std::recursive_mutex> lock(g_thread_files.mutex);
    auto* file = find_or_open_data_file(path, error);
    if (file == nullptr) {
        return false;
    }
    if (!range_in_bounds(offset, size, file->file_size)) {
        error = "read range exceeds data file size";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    {
        auto& stats = work_stats::counters();
        work_stats::add(stats.data_bytes_read, size);
        work_stats::add(stats.data_reads, 1U);
        work_stats::raise_to(stats.largest_read, size);
    }
    if (size == 0U) {
        return true;
    }
    if (offset >= file->tail_start) {  // inside the cached tail: a copy, no I/O
        std::memcpy(out.data(), file->tail.data() + (offset - file->tail_start), static_cast<std::size_t>(size));
        return true;
    }
    const std::uint64_t block = file->source->small_read_block();
    if (size < block) {
        if (file->block_start == std::numeric_limits<std::uint64_t>::max() || offset < file->block_start ||
            offset + size > file->block_start + file->block.size()) {
            // Load the aligned block holding the read (cut at the file's end; widened when the read
            // crosses a block boundary).
            const std::uint64_t start = offset - offset % block;
            const std::uint64_t end = std::min(file->file_size, std::max(start + block, offset + size));
            file->block.resize(static_cast<std::size_t>(end - start));
            if (!file->source->read_at(start, end - start, file->block.data(), error)) {
                file->block.clear();
                file->block_start = std::numeric_limits<std::uint64_t>::max();
                out.clear();
                return false;
            }
            work_stats::count_read(end - start);
            file->block_start = start;
        }
        std::memcpy(out.data(), file->block.data() + (offset - file->block_start), static_cast<std::size_t>(size));
        return true;
    }
    if (!file->source->read_at(offset, size, out.data(), error)) {
        out.clear();
        return false;
    }
    work_stats::count_read(size);
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
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges(layout.num_columns);
    std::uint64_t lo = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t hi = 0;
    std::uint64_t total = 0;
    for (std::uint32_t col = 0; col < layout.num_columns; ++col) {
        const auto base = static_cast<std::size_t>(col) * 16U;
        if (!read_le64(table.data() + base, ranges[col].first) || !read_le64(table.data() + base + 8U, ranges[col].second)) {
            error = "failed to parse column metadata offset table";
            return false;
        }
        lo = std::min(lo, ranges[col].first);
        hi = std::max(hi, ranges[col].first + ranges[col].second);
        total += ranges[col].second;
    }
    // Lance writes the column metadatas back to back: when they are (nearly) contiguous, one read of
    // the span replaces one read per column -- one request instead of hundreds from a remote source.
    std::vector<std::uint8_t> span;
    const bool one_read = layout.num_columns > 1U && hi > lo && hi - lo <= 2U * total + 4096U &&
                          hi - lo <= (std::uint64_t{64} << 20U);
    if (one_read && !read_lance_data_file_bytes(path, lo, hi - lo, span, error)) {
        return false;
    }
    std::vector<std::uint8_t> meta_bytes;
    for (std::uint32_t col = 0; col < layout.num_columns; ++col) {
        const auto [meta_offset, meta_size] = ranges[col];
        if (one_read) {
            meta_bytes.assign(span.begin() + static_cast<std::ptrdiff_t>(meta_offset - lo),
                              span.begin() + static_cast<std::ptrdiff_t>(meta_offset - lo + meta_size));
        } else if (!read_lance_data_file_bytes(path, meta_offset, meta_size, meta_bytes, error)) {
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
