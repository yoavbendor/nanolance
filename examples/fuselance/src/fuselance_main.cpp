// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// fuselance — mount a Lance table with blob.v2 entries as a read-only FUSE filesystem.
//
// Usage:
//   fuselance <lance_table_path> --filename-col <col>
//             [--blob-col <col>]
//             [--join-blob-from <blob_table_path>]
//             [--sort-col <col>]
//             [--frame-col <col>]
//
// Each DISTINCT value of --filename-col becomes a file. The file's content is the concatenation
// of all blob.v2 payloads for rows sharing that value, in row order (or --sort-col order within
// the group).
//
// --frame-col <col> (numerical) adds a second dimension:
//   Root contains the flat files as usual (all rows), PLUS subdirectories named frame_<N> for
//   each distinct value of <col>. Each frame_<N>/ directory contains the same set of files but
//   filtered to rows where <col> == N.
//
// --join-blob-from lets the filename column and the blob column live in different tables:
//   • The positional table is the "name table" (provides --filename-col and optionally --sort-col,
//     plus a packet_id join key).
//   • --join-blob-from <path> is the "blob table" (provides payload_ref and packet_id).
//   Both are joined on packet_id. Use this with pcapng2lance --decode-l2l3 output:
//     fuselance _ipv4.lance --filename-col src --join-blob-from packets.lance
//
// Without --join-blob-from the blob column is auto-detected in the same table as --filename-col
// (or overridden with --blob-col).
//
// fixed_size_binary:4  columns are rendered as dotted-quad IPv4 (e.g. "192.168.1.1").
// fixed_size_binary:16 columns are rendered as colon-hex IPv6 (e.g. "fe80::1").
//
// Unmount with:  fusermount3 -u /tmp/fuse_<basename>   or  Ctrl-C.

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_reader.h"
#include "fuselance_version.h"

#include <nanoarrow/nanoarrow.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Verbosity / logging
// ---------------------------------------------------------------------------
// Levels: 0=errors only, 1=info (default), 2=verbose, 3=debug
static int g_verbosity = 1;

#define LOG_ERR(fmt, ...)  std::fprintf(stderr, "fuselance [ERR]: "  fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) do { if (g_verbosity >= 1) std::fprintf(stderr, "fuselance: "       fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_VERB(fmt, ...) do { if (g_verbosity >= 2) std::fprintf(stderr, "fuselance [V]: "   fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DBG(fmt, ...)  do { if (g_verbosity >= 3) std::fprintf(stderr, "fuselance [DBG]: " fmt "\n", ##__VA_ARGS__); } while(0)

// ---------------------------------------------------------------------------
// Performance counters
// ---------------------------------------------------------------------------
static bool g_perf = false;  // enabled by --perf flag

struct PerfCounters {
    // Startup phase (measured in main before fuse_main).
    int64_t  startup_ms = 0;      // total time from argv parse to fuse_main

    // FUSE op counters (updated atomically from FUSE threads).
    std::atomic<uint64_t> open_ok{0};
    std::atomic<uint64_t> open_err{0};
    std::atomic<uint64_t> read_calls{0};
    std::atomic<uint64_t> read_bytes{0};
    std::atomic<uint64_t> read_fetch_calls{0};
    std::atomic<uint64_t> read_fetch_us{0};   // microseconds spent in nano_lance_fetch_external_blob
    std::atomic<uint64_t> read_err{0};
    std::atomic<uint64_t> getattr_calls{0};
    std::atomic<uint64_t> readdir_calls{0};
};

static PerfCounters g_perf_ctr;

static void perf_dump() {
    if (!g_perf) return;
    uint64_t fetch_calls = g_perf_ctr.read_fetch_calls.load();
    uint64_t fetch_us    = g_perf_ctr.read_fetch_us.load();
    double   fetch_avg_ms = fetch_calls ? static_cast<double>(fetch_us) / fetch_calls / 1000.0 : 0.0;
    uint64_t read_bytes  = g_perf_ctr.read_bytes.load();
    double   read_mib    = static_cast<double>(read_bytes) / (1024.0 * 1024.0);

    std::fprintf(stderr,
        "\n--- fuselance perf counters ---\n"
        "  startup:       %lld ms\n"
        "  getattr calls: %llu\n"
        "  readdir calls: %llu\n"
        "  open  ok/err:  %llu / %llu\n"
        "  read  calls:   %llu  (%.2f MiB total)\n"
        "  fetch calls:   %llu  (avg %.2f ms each, %llu us total)\n"
        "  read  errors:  %llu\n"
        "-------------------------------\n",
        static_cast<long long>(g_perf_ctr.startup_ms),
        static_cast<unsigned long long>(g_perf_ctr.getattr_calls.load()),
        static_cast<unsigned long long>(g_perf_ctr.readdir_calls.load()),
        static_cast<unsigned long long>(g_perf_ctr.open_ok.load()),
        static_cast<unsigned long long>(g_perf_ctr.open_err.load()),
        static_cast<unsigned long long>(g_perf_ctr.read_calls.load()),
        read_mib,
        static_cast<unsigned long long>(fetch_calls),
        fetch_avg_ms,
        static_cast<unsigned long long>(fetch_us),
        static_cast<unsigned long long>(g_perf_ctr.read_err.load()));
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

// One contiguous blob segment: a range within one external URI.
struct BlobSeg {
    std::string uri;
    uint64_t position = 0;
    uint64_t size = 0;
};

// One virtual file: a name and an ordered list of blob segments (one per matching row).
// Total file size = sum of segment sizes. Reads span segments transparently.
struct VirtualFile {
    std::string name;
    uint64_t total_size = 0;
    std::vector<BlobSeg> segs;
};

// One frame directory: an integer label and the files visible inside it.
struct FrameDir {
    std::string label;           // "frame_<N>" — the directory name shown in FUSE
    std::vector<VirtualFile> files;
};

struct FuseLanceState {
    std::vector<VirtualFile> files;   // root-level flat files (all rows)
    std::vector<FrameDir>    frames;  // frame_<N> subdirectories (only when --frame-col given)
};

static FuseLanceState* g_state = nullptr;

// ---------------------------------------------------------------------------
// File-handle encoding
//
// fh bit layout (64-bit):
//   root files:   0x0000_0000_XXXX_XXXX   (top 32 bits = 0, bottom = file index)
//   framed files: 0x8000_FFFF_XXXX_XXXX   (bit 63 set; bits [47:32] = frame index; [31:0] = file index)
//
// fh layout for framed entries: bit 63 = framed flag | bits [47:32] = file index (uint16, up to 65535 files)
// | bits [31:0] = frame index (uint32, up to ~4B frames). Root fh = plain file_idx (bit 63 clear).
// ---------------------------------------------------------------------------

static constexpr uint64_t kFramedBit = (uint64_t{1} << 63);

static uint64_t encode_root_fh(size_t file_idx) {
    return static_cast<uint64_t>(file_idx);
}
static uint64_t encode_frame_fh(size_t frame_idx, size_t file_idx) {
    return kFramedBit | (static_cast<uint64_t>(file_idx) << 32) | static_cast<uint64_t>(frame_idx);
}
static bool   is_framed_fh(uint64_t fh)           { return (fh & kFramedBit) != 0; }
static size_t frame_idx_from_fh(uint64_t fh)       { return static_cast<size_t>(fh & 0xFFFF'FFFF); }
static size_t framed_file_idx_from_fh(uint64_t fh) { return static_cast<size_t>((fh >> 32) & 0xFFFF); }
static size_t root_file_idx_from_fh(uint64_t fh)   { return static_cast<size_t>(fh & 0xFFFFFFFFULL); }

// Resolve fh → VirtualFile&.
static const VirtualFile* vfile_from_fh(uint64_t fh) {
    if (is_framed_fh(fh)) {
        size_t fi = frame_idx_from_fh(fh);
        size_t vi = framed_file_idx_from_fh(fh);
        if (fi >= g_state->frames.size() || vi >= g_state->frames[fi].files.size()) return nullptr;
        return &g_state->frames[fi].files[vi];
    }
    size_t vi = root_file_idx_from_fh(fh);
    if (vi >= g_state->files.size()) return nullptr;
    return &g_state->files[vi];
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int child_index(const ArrowSchema& schema, const char* name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name && std::strcmp(schema.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Detect the blob.v2 column: first top-level struct child with uri/position/size children.
static int detect_blob_col(const ArrowSchema& schema) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const ArrowSchema& c = *schema.children[i];
        if (!c.format || c.format[0] != '+' || c.format[1] != 's') continue;
        const bool has_uri = child_index(c, "uri") >= 0 || child_index(c, "blob_uri") >= 0;
        if (has_uri && child_index(c, "position") >= 0 && child_index(c, "size") >= 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Format a fixed-size binary cell as an IP address string (IPv4: 4 bytes, IPv6: 16 bytes).
// Returns true if the width is 4 or 16 and formatting succeeded, false otherwise.
static bool format_ip(const uint8_t* bytes, int width, std::string& out) {
    if (width == 4) {
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, bytes, buf, sizeof(buf))) { out = buf; return true; }
    } else if (width == 16) {
        char buf[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, bytes, buf, sizeof(buf))) { out = buf; return true; }
    }
    return false;
}

// Strip a single leading '/' so "/proc/info/foo.log" → "proc/info/foo.log".
// Column values without a leading slash (e.g. "std.err") are kept as-is.
static std::string strip_leading_slash(const std::string& s) {
    if (!s.empty() && s[0] == '/') return s.substr(1);
    return s;
}

// Render a column cell as a filename-safe display string.
// Supports: string, integer, and fixed_size_binary:4/16 (rendered as IP addresses).
// Returns false for unsupported types.
static bool cell_to_string(const ArrowArrayView* col, int64_t row, std::string& out) {
    switch (col->storage_type) {
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            ArrowStringView sv = ArrowArrayViewGetStringUnsafe(col, row);
            out.assign(sv.data, static_cast<size_t>(sv.size_bytes));
            return true;
        }
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
            out = std::to_string(ArrowArrayViewGetIntUnsafe(col, row));
            return true;
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            out = std::to_string(ArrowArrayViewGetUIntUnsafe(col, row));
            return true;
        case NANOARROW_TYPE_FIXED_SIZE_BINARY: {
            // element_size_bits is in the layout (set by ArrowArrayViewInitFromSchema).
            const int w = static_cast<int>(col->layout.element_size_bits[1] / 8);
            ArrowBufferView bv = ArrowArrayViewGetBytesUnsafe(col, row);
            if (format_ip(reinterpret_cast<const uint8_t*>(bv.data.as_char), w, out)) return true;
            // Fall back: hex dump for other binary widths.
            out.clear();
            static const char hex[] = "0123456789abcdef";
            for (int b = 0; b < w; ++b) {
                uint8_t byte = reinterpret_cast<const uint8_t*>(bv.data.as_char)[b];
                out += hex[byte >> 4];
                out += hex[byte & 0xf];
            }
            return true;
        }
        default:
            return false;
    }
}

// Sort key: string → lexicographic value; integer → zero-padded decimal; binary → same hex as above.
static std::string sort_key(const ArrowArrayView* col, int64_t row) {
    std::string s;
    if (cell_to_string(col, row, s)) return s;
    // Fallback: zero-padded row index (shouldn't happen for sane sort cols).
    char buf[24]; std::snprintf(buf, sizeof(buf), "%020lld", static_cast<long long>(row));
    return buf;
}

// Read frame value as a uint64 (sign-extended int columns mapped via two's complement).
static uint64_t frame_value(const ArrowArrayView* col, int64_t row) {
    switch (col->storage_type) {
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
            return static_cast<uint64_t>(ArrowArrayViewGetIntUnsafe(col, row));
        default:
            return ArrowArrayViewGetUIntUnsafe(col, row);
    }
}

// ---------------------------------------------------------------------------
// Path helpers — support multi-level virtual paths from column values
// ---------------------------------------------------------------------------

// Convert FUSE absolute path "/a/b/c" to relative "a/b/c" (strips leading '/').
static std::string_view path_rel(const char* fuse_path) {
    return (fuse_path[0] == '/' && fuse_path[1] != '\0')
        ? std::string_view(fuse_path + 1)
        : std::string_view("");
}

// True if `file_rel` ("a/b/c") lives under directory `dir_rel` ("a/b"), i.e.
// file_rel == dir_rel + "/" + something.
static bool under_dir(std::string_view dir_rel, const std::string& file_rel) {
    if (dir_rel.empty()) return true;  // everything is under root
    if (file_rel.size() <= dir_rel.size()) return false;
    if (file_rel.compare(0, dir_rel.size(), dir_rel) != 0) return false;
    return file_rel[dir_rel.size()] == '/';
}

// Given a file at `file_rel` that lives under `dir_rel`, return its direct child
// name (the next path component), and whether that child is itself a directory.
// e.g. dir="proc", file="proc/info/foo.log" → child="info", is_dir=true
//      dir="proc/info", file="proc/info/foo.log" → child="foo.log", is_dir=false
static std::string direct_child(std::string_view dir_rel, const std::string& file_rel,
                                bool& out_is_dir) {
    size_t start = dir_rel.empty() ? 0 : dir_rel.size() + 1;
    size_t slash = file_rel.find('/', start);
    out_is_dir = (slash != std::string::npos);
    return file_rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
}

// Emit all direct children of `dir_rel` from `files` into the FUSE filler.
// Deduplicates subdirectory names. `file_total_size` is used for regular file stat.
static void fill_dir_children(const std::vector<VirtualFile>& files,
                               std::string_view dir_rel,
                               void* buf, fuse_fill_dir_t filler) {
    std::set<std::string> seen_dirs;
    for (const auto& f : files) {
        if (!under_dir(dir_rel, f.name)) continue;
        bool is_dir;
        std::string child = direct_child(dir_rel, f.name, is_dir);
        struct stat st{};
        if (is_dir) {
            if (!seen_dirs.insert(child).second) continue;  // already emitted
            st.st_mode = S_IFDIR | 0555;
        } else {
            st.st_mode = S_IFREG | 0444;
            st.st_size = static_cast<off_t>(f.total_size);
        }
        filler(buf, child.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
    }
}

// For getattr: find a VirtualFile whose relative path exactly matches `rel`,
// or return nullptr. Also checks if `rel` is a valid intermediate directory.
// Sets `out_is_dir=true` if rel names a directory prefix.
static const VirtualFile* find_vfile(const std::vector<VirtualFile>& files,
                                     std::string_view rel, bool& out_is_dir) {
    out_is_dir = false;
    std::string prefix(rel);
    prefix += '/';
    for (const auto& f : files) {
        if (f.name == rel)  return &f;
        if (f.name.size() > rel.size() &&
            f.name.compare(0, prefix.size(), prefix) == 0) {
            out_is_dir = true;  // keep looking for exact match
        }
    }
    return nullptr;
}

// If `fuse_path` starts with a known frame label ("/frame_N/…"), return the frame index
// and the remainder relative path within the frame ("proc/info/foo.log").
// Returns -1 if path doesn't start with a frame label.
static int split_frame_path(const char* fuse_path, std::string_view& out_rest) {
    if (fuse_path[0] != '/') return -1;
    const char* p = fuse_path + 1;
    const char* slash = std::strchr(p, '/');
    std::string label = slash ? std::string(p, slash - p) : std::string(p);
    for (size_t i = 0; i < g_state->frames.size(); ++i) {
        if (g_state->frames[i].label == label) {
            out_rest = slash ? std::string_view(slash + 1) : std::string_view("");
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// FUSE operations
// ---------------------------------------------------------------------------

static int fl_getattr(const char* path, struct stat* st, struct fuse_file_info* /*fi*/) {
    if (g_perf) ++g_perf_ctr.getattr_calls;
    std::memset(st, 0, sizeof(*st));

    // Root directory.
    if (std::strcmp(path, "/") == 0) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }

    // Check if path is inside (or is) a frame directory: "/frame_N" or "/frame_N/…"
    std::string_view frame_rest;
    int fdi = split_frame_path(path, frame_rest);
    if (fdi >= 0) {
        const auto& ffiles = g_state->frames[static_cast<size_t>(fdi)].files;
        if (frame_rest.empty()) {
            // "/frame_N" itself
            st->st_mode  = S_IFDIR | 0555;
            st->st_nlink = 2;
            return 0;
        }
        bool is_dir = false;
        const VirtualFile* vf = find_vfile(ffiles, frame_rest, is_dir);
        if (vf) {
            st->st_mode  = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size  = static_cast<off_t>(vf->total_size);
            return 0;
        }
        if (is_dir) { st->st_mode = S_IFDIR | 0555; st->st_nlink = 2; return 0; }
        LOG_DBG("getattr ENOENT: %s", path);
        return -ENOENT;
    }

    // Root path: "/a" or "/a/b/c"
    std::string_view rel = path_rel(path);
    bool is_dir = false;
    const VirtualFile* vf = find_vfile(g_state->files, rel, is_dir);
    if (vf) {
        st->st_mode  = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size  = static_cast<off_t>(vf->total_size);
        return 0;
    }
    if (is_dir) { st->st_mode = S_IFDIR | 0555; st->st_nlink = 2; return 0; }
    LOG_DBG("getattr ENOENT: %s", path);
    return -ENOENT;
}

static int fl_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                      off_t /*offset*/, struct fuse_file_info* /*fi*/,
                      enum fuse_readdir_flags /*flags*/) {
    if (g_perf) ++g_perf_ctr.readdir_calls;
    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));

    // Check if inside a frame directory.
    std::string_view frame_rest;
    int fdi = split_frame_path(path, frame_rest);
    if (fdi >= 0) {
        fill_dir_children(g_state->frames[static_cast<size_t>(fdi)].files,
                          frame_rest, buf, filler);
        return 0;
    }

    std::string_view rel = (std::strcmp(path, "/") == 0) ? "" : path_rel(path);

    // Emit frame subdirs at root level.
    if (rel.empty()) {
        for (const auto& fr : g_state->frames) {
            struct stat st{};
            st.st_mode = S_IFDIR | 0555;
            filler(buf, fr.label.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
        }
    }

    fill_dir_children(g_state->files, rel, buf, filler);
    return 0;
}

static int fl_open(const char* path, struct fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        LOG_ERR("open rejected (not O_RDONLY): %s", path);
        if (g_perf) ++g_perf_ctr.open_err;
        return -EACCES;
    }

    // Framed file: "/frame_N/…/file"
    std::string_view frame_rest;
    int fdi = split_frame_path(path, frame_rest);
    if (fdi >= 0 && !frame_rest.empty()) {
        const auto& ffiles = g_state->frames[static_cast<size_t>(fdi)].files;
        for (size_t vi = 0; vi < ffiles.size(); ++vi) {
            if (ffiles[vi].name == frame_rest) {
                fi->fh = encode_frame_fh(static_cast<size_t>(fdi), vi);
                LOG_VERB("open framed %s -> fh=%llx (%zu segs, %llu bytes)",
                         path, (unsigned long long)fi->fh,
                         ffiles[vi].segs.size(),
                         (unsigned long long)ffiles[vi].total_size);
                if (g_perf) ++g_perf_ctr.open_ok;
                return 0;
            }
        }
        LOG_ERR("open ENOENT (framed): %s", path);
        if (g_perf) ++g_perf_ctr.open_err;
        return -ENOENT;
    }

    // Root file: "/a/b/c"
    std::string_view rel = path_rel(path);
    for (size_t i = 0; i < g_state->files.size(); ++i) {
        if (g_state->files[i].name == rel) {
            fi->fh = encode_root_fh(i);
            LOG_VERB("open %s -> fh=%llx (%zu segs, %llu bytes)",
                     path, (unsigned long long)fi->fh,
                     g_state->files[i].segs.size(),
                     (unsigned long long)g_state->files[i].total_size);
            if (g_perf) ++g_perf_ctr.open_ok;
            return 0;
        }
    }
    LOG_ERR("open ENOENT: %s", path);
    if (g_perf) ++g_perf_ctr.open_err;
    return -ENOENT;
}

// Read spanning multiple blob segments transparently.
static int fl_read(const char* /*path*/, char* buf, size_t buf_size,
                   off_t offset, struct fuse_file_info* fi) {
    const VirtualFile* vf = vfile_from_fh(fi->fh);
    if (!vf) return -EBADF;
    if (offset < 0 || static_cast<uint64_t>(offset) >= vf->total_size) return 0;

    if (g_perf) ++g_perf_ctr.read_calls;

    uint64_t file_off = static_cast<uint64_t>(offset);
    size_t total_written = 0;
    // Target: accumulate at least 64KB per read to stream sequential readers (cat)
    // smoothly while keeping random/partial readers (head) fast.
    const size_t target_size = 64 * 1024;

    for (const BlobSeg& seg : vf->segs) {
        if (total_written >= buf_size) break;
        if (file_off >= seg.size) { file_off -= seg.size; continue; }  // skip segments before offset

        uint64_t seg_off = file_off;
        uint64_t seg_avail = seg.size - seg_off;
        size_t want = std::min(static_cast<uint64_t>(buf_size - total_written), seg_avail);

        size_t got = 0;
        char ferr[512];

        auto t0 = g_perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        int rc = nano_lance_fetch_external_blob(
            seg.uri.c_str(), seg.position + seg_off, want,
            reinterpret_cast<uint8_t*>(buf + total_written), want,
            &got, ferr, sizeof(ferr));
        if (g_perf) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            ++g_perf_ctr.read_fetch_calls;
            g_perf_ctr.read_fetch_us += static_cast<uint64_t>(us);
        }

        if (rc != NANO_LANCE_READER_OK) {
            LOG_ERR("fetch '%s' uri=%s @%llu+%llu: %s",
                    vf->name.c_str(), seg.uri.c_str(),
                    static_cast<unsigned long long>(seg.position + seg_off),
                    static_cast<unsigned long long>(want), ferr);
            if (g_perf) ++g_perf_ctr.read_err;
            return total_written > 0 ? static_cast<int>(total_written) : -EIO;
        }
        LOG_DBG("read '%s' @%llu+%llu => %zu bytes",
                vf->name.c_str(),
                static_cast<unsigned long long>(seg.position + seg_off),
                static_cast<unsigned long long>(want), got);
        total_written += got;
        file_off = 0;  // consumed offset; subsequent segs start at 0
        // Return once we've accumulated target_size data (if available). This allows
        // sequential readers (cat) to buffer ~64KB per read while partial readers
        // (head) return quickly with just the first few segments.
        if (total_written >= target_size) break;
    }
    if (g_perf) g_perf_ctr.read_bytes += total_written;
    return static_cast<int>(total_written);
}

static void fl_destroy(void* /*private_data*/) {
    perf_dump();
}

static const fuse_operations fl_ops = [] {
    fuse_operations ops{};
    ops.getattr = fl_getattr;
    ops.readdir = fl_readdir;
    ops.open    = fl_open;
    ops.read    = fl_read;
    ops.destroy = fl_destroy;
    return ops;
}();

// ---------------------------------------------------------------------------
// Schema / batch helpers
// ---------------------------------------------------------------------------

struct BlobColIndices {
    int blob_idx = -1;  // index in parent schema
    int c_uri    = -1;  // child index for uri/blob_uri
    int c_pos    = -1;  // child index for position
    int c_size   = -1;  // child index for size
};

static bool resolve_blob_col(const ArrowSchema& schema, const char* blob_col_name,
                             BlobColIndices& out, std::string& err) {
    out.blob_idx = blob_col_name ? child_index(schema, blob_col_name) : detect_blob_col(schema);
    if (out.blob_idx < 0) {
        err = blob_col_name ? std::string("blob column '") + blob_col_name + "' not found"
                            : "no blob.v2 column detected (use --blob-col)";
        return false;
    }
    const ArrowSchema& bs = *schema.children[out.blob_idx];
    out.c_uri  = child_index(bs, "uri") >= 0 ? child_index(bs, "uri") : child_index(bs, "blob_uri");
    out.c_pos  = child_index(bs, "position");
    out.c_size = child_index(bs, "size");
    if (out.c_uri < 0 || out.c_pos < 0 || out.c_size < 0) {
        err = std::string("blob column '") + (bs.name ? bs.name : "?") + "' missing uri/position/size children";
        return false;
    }
    return true;
}

// Read (packet_id → BlobSeg) from a blob-only table; used for --join-blob-from.
static bool read_blob_table(const std::filesystem::path& path,
                            const char* blob_col_name,
                            std::map<uint64_t, BlobSeg>& out,
                            std::string& err) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_table_read_dataset(path, schema, batches, err)) return false;

    const int pid_idx = child_index(schema, "packet_id");
    if (pid_idx < 0) { err = "blob table missing packet_id join key"; return false; }

    BlobColIndices bi;
    if (!resolve_blob_col(schema, blob_col_name, bi, err)) return false;

    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError ae{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            err = std::string("blob table view: ") + ArrowErrorMessage(&ae);
            ArrowArrayViewReset(&view);
            return false;
        }
        const ArrowArrayView* blob_view = view.children[bi.blob_idx];
        for (int64_t r = 0; r < view.length; ++r) {
            uint64_t pid = ArrowArrayViewGetUIntUnsafe(view.children[pid_idx], r);
            ArrowStringView uv = ArrowArrayViewGetStringUnsafe(blob_view->children[bi.c_uri], r);
            out[pid] = {std::string(uv.data, static_cast<size_t>(uv.size_bytes)),
                        ArrowArrayViewGetUIntUnsafe(blob_view->children[bi.c_pos], r),
                        ArrowArrayViewGetUIntUnsafe(blob_view->children[bi.c_size], r)};
        }
        ArrowArrayViewReset(&view);
    }
    if (schema.release) schema.release(&schema);
    for (auto& b : batches) { if (b.release) b.release(&b); }
    return true;
}

// Build a sorted VirtualFile list from a groups map.
static std::vector<VirtualFile> build_virtual_files(
        std::map<std::string, std::vector<std::pair<std::string, BlobSeg>>>& groups) {
    std::vector<VirtualFile> files;
    files.reserve(groups.size());
    for (auto& [name, entries] : groups) {
        std::stable_sort(entries.begin(), entries.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        VirtualFile vf;
        vf.name = name;
        vf.segs.reserve(entries.size());
        for (auto& [sk, seg] : entries) {
            vf.total_size += seg.size;
            vf.segs.push_back(std::move(seg));
        }
        files.push_back(std::move(vf));
    }
    std::sort(files.begin(), files.end(),
              [](const VirtualFile& a, const VirtualFile& b) { return a.name < b.name; });
    return files;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <lance_table_path> --filename-col <col>\n"
        "          [--blob-col <col>] [--join-blob-from <blob_table>] [--sort-col <col>]\n"
        "          [--frame-col <col>] [-v|-vv|-vvv]\n"
        "\n"
        "Mounts the Lance table as a read-only FUSE filesystem at /tmp/fuse_<basename>.\n"
        "Each distinct value of --filename-col becomes one file whose content is the\n"
        "concatenation of all matching rows' blob.v2 payloads.\n"
        "\n"
        "  --blob-col <col>          blob.v2 struct column (auto-detected if omitted)\n"
        "  --join-blob-from <path>   read blob refs from a companion table joined on packet_id\n"
        "                            (use when --filename-col and the blob live in different tables)\n"
        "  --sort-col <col>          order blobs within each file by this column (ascending)\n"
        "  --frame-col <col>         numerical column; adds frame_<N>/ subdirectories to root,\n"
        "                            each containing the same files filtered to rows where <col>==N\n"
        "  -v / -vv / -vvv           verbosity: info(default=1) / verbose(2) / debug(3)\n"
        "                            use -q to suppress all but errors (level 0)\n"
        "  --perf                    print performance counters on unmount (startup ms,\n"
        "                            open/read/fetch counts, avg fetch latency, total bytes)\n"
        "\n"
        "fixed_size_binary:4  columns are rendered as dotted-quad IPv4 addresses.\n"
        "fixed_size_binary:16 columns are rendered as colon-hex IPv6 addresses.\n"
        "\n"
        "Example (pcapng2lance --decode-l2l3 output):\n"
        "  %s packets_ipv4.lance --filename-col src --join-blob-from packets.lance\n"
        "\n"
        "Unmount with:  fusermount3 -u /tmp/fuse_<basename>\n",
        prog, prog);
}

int main(int argc, char** argv) {
    const auto t_start = std::chrono::steady_clock::now();

    const char* table_path      = nullptr;
    const char* filename_col    = nullptr;
    const char* blob_col_name   = nullptr;   // nullptr = auto-detect in same table
    const char* join_blob_path  = nullptr;   // nullptr = no join; blobs in same table
    const char* sort_col_name   = nullptr;   // nullptr = row order
    const char* frame_col_name  = nullptr;   // nullptr = no frame dimension
    std::string err;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto need_val = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "fuselance: %s requires a value\n", flag); return nullptr; }
            return argv[++i];
        };
        if      (a == "--filename-col")    { filename_col   = need_val("--filename-col");    if (!filename_col)   return 2; }
        else if (a == "--blob-col")        { blob_col_name  = need_val("--blob-col");        if (!blob_col_name)  return 2; }
        else if (a == "--join-blob-from")  { join_blob_path = need_val("--join-blob-from");  if (!join_blob_path) return 2; }
        else if (a == "--sort-col")        { sort_col_name  = need_val("--sort-col");        if (!sort_col_name)  return 2; }
        else if (a == "--frame-col")       { frame_col_name = need_val("--frame-col");       if (!frame_col_name) return 2; }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a == "--perf")            { g_perf = true; }
        else if (a == "-q")                { g_verbosity = 0; }
        else if (a == "-v")                { g_verbosity = 2; }
        else if (a == "-vv")               { g_verbosity = 3; }
        else if (a == "-vvv")              { g_verbosity = 4; }
        else if (a[0] != '-') {
            if (table_path) { std::fprintf(stderr, "fuselance: unexpected argument: %s\n", argv[i]); return 2; }
            table_path = argv[i];
        } else {
            std::fprintf(stderr, "fuselance: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    // Version banner always on stderr regardless of verbosity.
    std::fprintf(stderr, "%s\n", FUSELANCE_VERSION);

    if (!table_path) { usage(argv[0]); return 2; }
    if (!filename_col) {
        // List column names from the manifest so the user can pick one.
        NanoLanceDatasetMetadata meta{};
        char merr[512];
        if (nano_lance_dataset_read_latest(table_path, &meta, merr, sizeof(merr)) != NANO_LANCE_READER_OK) {
            LOG_ERR("--filename-col is required");
            LOG_ERR("(also failed to read schema from '%s': %s)", table_path, merr);
            usage(argv[0]);
            return 2;
        }
        LOG_ERR("--filename-col is required\n\nColumns in '%s':", table_path);
        for (size_t i = 0; i < meta.fields_len; ++i) {
            const NanoLanceReaderField& f = meta.fields[i];
            if (f.parent_id == -1)
                std::fprintf(stderr, "  %-24s (%s)\n", f.name ? f.name : "?", f.logical_type ? f.logical_type : "?");
        }
        std::fprintf(stderr, "\n");
        usage(argv[0]);
        nano_lance_dataset_metadata_free(&meta);
        return 2;
    }

    // Mountpoint derived from the name table's basename.
    // Normalise trailing slashes so stem() works on "foo.lance/" too.
    std::filesystem::path tpath = std::filesystem::path(table_path).lexically_normal();
    std::string basename = tpath.stem().string();
    if (basename.empty() || basename == ".") basename = tpath.filename().string();
    if (basename.empty() || basename == ".") basename = "lance";
    const std::string mountpoint = "/tmp/fuse_" + basename;

    // ---- Optional: load blob table for join ---------------------------------
    std::map<uint64_t, BlobSeg> join_blobs;
    if (join_blob_path) {
        LOG_INFO("loading blob table %s ...", join_blob_path);
        if (!read_blob_table(std::filesystem::path(join_blob_path), blob_col_name, join_blobs, err)) {
            LOG_ERR("blob table error: %s", err.c_str());
            return 1;
        }
        LOG_INFO("joined %zu blob refs", join_blobs.size());
    }

    // ---- Read the name table ------------------------------------------------
    // Collect only the columns fuselance actually needs; skip all others to
    // avoid decompressing irrelevant column data (huge win for wide tables).
    std::vector<std::string> proj_cols;
    proj_cols.push_back(filename_col);
    if (blob_col_name)   proj_cols.push_back(blob_col_name);
    if (sort_col_name)   proj_cols.push_back(sort_col_name);
    if (frame_col_name)  proj_cols.push_back(frame_col_name);
    // packet_id join key (needed when --join-blob-from is used).
    if (join_blob_path)  proj_cols.push_back("packet_id");
    // If blob column not explicitly named, auto-detect after schema load — fall
    // back to full read so detect_blob_col can scan all columns.
    const bool can_project = (blob_col_name != nullptr) || join_blob_path;

    LOG_INFO("reading %s ... (projecting %zu column(s)%s)",
             table_path, proj_cols.size(), can_project ? "" : " — blob-col unknown, reading all");
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    bool read_ok = can_project
        ? nano_lance::lance_table_read_dataset_projected(tpath, proj_cols, schema, batches, err)
        : nano_lance::lance_table_read_dataset(tpath, schema, batches, err);
    if (!read_ok) {
        // If projected read failed (e.g. unknown join key), fall back to full read.
        if (can_project) {
            LOG_VERB("projected read failed (%s), retrying full read", err.c_str());
            err.clear();
            read_ok = nano_lance::lance_table_read_dataset(tpath, schema, batches, err);
        }
        if (!read_ok) {
            LOG_ERR("read failed: %s", err.c_str());
            return 1;
        }
    }
    LOG_INFO("loaded %zu batch(es)", batches.size());

    // ---- Locate columns in name table ---------------------------------------
    const int fname_idx = child_index(schema, filename_col);
    if (fname_idx < 0) {
        LOG_ERR("column '%s' not found in schema", filename_col);
        return 1;
    }

    BlobColIndices bi;
    if (!join_blob_path) {
        if (!resolve_blob_col(schema, blob_col_name, bi, err)) {
            LOG_ERR("%s", err.c_str());
            return 1;
        }
    }

    const int pid_idx = join_blob_path ? child_index(schema, "packet_id") : -1;
    if (join_blob_path && pid_idx < 0) {
        LOG_ERR("name table missing packet_id join key");
        return 1;
    }

    int sort_idx = -1;
    if (sort_col_name) {
        sort_idx = child_index(schema, sort_col_name);
        if (sort_idx < 0) {
            LOG_ERR("sort column '%s' not found", sort_col_name);
            return 1;
        }
    }

    int frame_idx = -1;
    if (frame_col_name) {
        frame_idx = child_index(schema, frame_col_name);
        if (frame_idx < 0) {
            LOG_ERR("frame column '%s' not found", frame_col_name);
            return 1;
        }
    }

    // ---- Build grouped virtual file maps ------------------------------------
    // groups_all: all rows (root-level flat files)
    // groups_by_frame: per-frame-value groups (frame_<N> directories)
    using GroupMap = std::map<std::string, std::vector<std::pair<std::string, BlobSeg>>>;
    GroupMap groups_all;
    std::map<uint64_t, GroupMap> groups_by_frame;  // keyed by raw frame value

    size_t global_row = 0;  // stable sort key counter across batches

    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError ae{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            LOG_ERR("view init: %s", ArrowErrorMessage(&ae));
            ArrowArrayViewReset(&view);
            return 1;
        }

        const ArrowArrayView* fname_view  = view.children[fname_idx];
        const ArrowArrayView* blob_av     = !join_blob_path ? view.children[bi.blob_idx] : nullptr;
        const ArrowArrayView* sort_view   = sort_idx  >= 0 ? view.children[sort_idx]  : nullptr;
        const ArrowArrayView* pid_view    = pid_idx   >= 0 ? view.children[pid_idx]   : nullptr;
        const ArrowArrayView* frame_view  = frame_idx >= 0 ? view.children[frame_idx] : nullptr;

        if (view.length > 0) {
            std::string probe;
            if (!cell_to_string(fname_view, 0, probe)) {
                LOG_ERR("--filename-col '%s' has an unsupported type", filename_col);
                ArrowArrayViewReset(&view);
                return 1;
            }
        }

        for (int64_t r = 0; r < view.length; ++r) {
            std::string fname;
            cell_to_string(fname_view, r, fname);
            fname = strip_leading_slash(fname);

            BlobSeg seg;
            if (join_blob_path) {
                uint64_t pid = ArrowArrayViewGetUIntUnsafe(pid_view, r);
                auto it = join_blobs.find(pid);
                if (it == join_blobs.end()) {
                    LOG_VERB("row %lld: packet_id %llu has no blob match, skipping",
                             static_cast<long long>(r), static_cast<unsigned long long>(pid));
                    continue;
                }
                seg = it->second;
            } else {
                ArrowStringView uv = ArrowArrayViewGetStringUnsafe(blob_av->children[bi.c_uri], r);
                seg.uri      = std::string(uv.data, static_cast<size_t>(uv.size_bytes));
                seg.position = ArrowArrayViewGetUIntUnsafe(blob_av->children[bi.c_pos], r);
                seg.size     = ArrowArrayViewGetUIntUnsafe(blob_av->children[bi.c_size], r);
            }

            std::string sk;
            if (sort_view) {
                sk = sort_key(sort_view, r);
            } else {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%020zu", global_row);
                sk = buf;
            }

            groups_all[fname].emplace_back(sk, seg);

            if (frame_view) {
                uint64_t fv = frame_value(frame_view, r);
                groups_by_frame[fv][fname].emplace_back(sk, seg);
            }

            ++global_row;
        }
        ArrowArrayViewReset(&view);
    }

    // ---- Build FuseLanceState -----------------------------------------------
    FuseLanceState state;
    state.files = build_virtual_files(groups_all);

    // Sort frame values numerically and build FrameDir list.
    for (auto& [fv, gmap] : groups_by_frame) {
        FrameDir fd;
        fd.label = "frame_" + std::to_string(fv);
        fd.files = build_virtual_files(gmap);
        state.frames.push_back(std::move(fd));
    }

    g_state = &state;

    // Release Arrow buffers before blocking in fuse_main.
    if (schema.release) schema.release(&schema);
    for (auto& b : batches) { if (b.release) b.release(&b); }
    batches.clear();

    LOG_INFO("%zu distinct file(s)%s in virtual filesystem",
             state.files.size(),
             state.frames.empty() ? "" :
                 (", " + std::to_string(state.frames.size()) + " frame dir(s)").c_str());
    if (state.files.size() <= 20) {
        for (auto& f : state.files)
            LOG_INFO("  %-40s  %llu byte(s) across %zu segment(s)",
                     f.name.c_str(), static_cast<unsigned long long>(f.total_size), f.segs.size());
    }
    if (!state.frames.empty() && state.frames.size() <= 10) {
        for (auto& fr : state.frames)
            LOG_INFO("  [dir] %s/  (%zu file(s))", fr.label.c_str(), fr.files.size());
    }

    // ---- Create mountpoint and launch FUSE ---------------------------------
    std::error_code ec;
    std::filesystem::create_directories(mountpoint, ec);
    if (ec) {
        LOG_ERR("cannot create mountpoint %s: %s", mountpoint.c_str(), ec.message().c_str());
        return 1;
    }
    LOG_INFO("mounting at %s (Ctrl-C or fusermount3 -u to unmount)", mountpoint.c_str());

    g_perf_ctr.startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();
    if (g_perf)
        std::fprintf(stderr, "fuselance [perf]: startup took %lld ms\n",
                     static_cast<long long>(g_perf_ctr.startup_ms));

    const char* fuse_argv[] = {argv[0], "-f", mountpoint.c_str(), nullptr};
    int fuse_argc = 3;
    return fuse_main(fuse_argc, const_cast<char**>(fuse_argv), &fl_ops, nullptr);
}
