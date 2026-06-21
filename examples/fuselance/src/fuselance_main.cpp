// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// fuselance — mount a Lance table with blob.v2 entries as a read-only FUSE filesystem.
//
// Usage:
//   fuselance <lance_table_path> --filename-col <col> [--blob-col <col>] [--sort-col <col>]
//
// Each row in the table is exposed as a file under /tmp/fuse_<basename>/. The file name comes
// from --filename-col. File contents are the blob.v2 payload, fetched on demand from the URI
// stored in --blob-col (auto-detected if omitted: first struct child with uri/position/size).
// Rows are ordered by --sort-col (string or integer, lexicographic / numeric ascending), or by
// original row order when omitted. Unmount with fusermount3 -u <mountpoint> or Ctrl-C.

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_reader.h"

#include <nanoarrow/nanoarrow.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

struct VirtualFile {
    std::string name;
    std::string uri;
    uint64_t position = 0;
    uint64_t size = 0;
};

struct FuseLanceState {
    std::vector<VirtualFile> files;
};

static FuseLanceState* g_state = nullptr;

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

// Detect the blob.v2 column: the first top-level struct child that has uri/position/size children.
static int detect_blob_col(const ArrowSchema& schema) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const ArrowSchema& c = *schema.children[i];
        // Must be a struct (format starts with '+s')
        if (!c.format || c.format[0] != '+' || c.format[1] != 's') continue;
        const bool has_uri = child_index(c, "uri") >= 0 || child_index(c, "blob_uri") >= 0;
        if (has_uri && child_index(c, "position") >= 0 && child_index(c, "size") >= 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Render a column cell as a display string: string columns yield their value, integer columns their
// decimal text. Used for the filename column so an integer id column (e.g. packet_id) works too.
// Returns false for unsupported column types.
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
        default:
            return false;
    }
}

// Return a sort key string for a row in the given column view (string → value, integer → zero-padded).
// We key off ArrowArrayView::storage_type so string columns sort lexicographically and integer
// columns sort numerically (zero-padded to 20 digits so a plain string compare == numeric ascending).
static std::string sort_key(const ArrowArrayView* col, int64_t row) {
    switch (col->storage_type) {
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            ArrowStringView sv = ArrowArrayViewGetStringUnsafe(col, row);
            return std::string(sv.data, static_cast<size_t>(sv.size_bytes));
        }
        default: {
            char buf[32];
            uint64_t v = ArrowArrayViewGetUIntUnsafe(col, row);
            std::snprintf(buf, sizeof(buf), "%020llu", static_cast<unsigned long long>(v));
            return buf;
        }
    }
}

// Make a name unique within the accumulated set by appending _N suffixes.
static std::string unique_name(const std::string& base, std::vector<VirtualFile>& files) {
    // Check if base is already taken
    bool taken = false;
    for (auto& f : files) {
        if (f.name == base) { taken = true; break; }
    }
    if (!taken) return base;
    for (int n = 1; ; ++n) {
        std::string candidate = base + "_" + std::to_string(n);
        bool dup = false;
        for (auto& f : files) {
            if (f.name == candidate) { dup = true; break; }
        }
        if (!dup) return candidate;
    }
}

// ---------------------------------------------------------------------------
// FUSE operations
// ---------------------------------------------------------------------------

static int fl_getattr(const char* path, struct stat* st, struct fuse_file_info* /*fi*/) {
    std::memset(st, 0, sizeof(*st));
    if (std::strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2 + static_cast<nlink_t>(g_state->files.size());
        return 0;
    }
    const char* name = path + 1;  // skip leading '/'
    for (auto& f : g_state->files) {
        if (f.name == name) {
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = static_cast<off_t>(f.size);
            return 0;
        }
    }
    return -ENOENT;
}

static int fl_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                      off_t /*offset*/, struct fuse_file_info* /*fi*/,
                      enum fuse_readdir_flags /*flags*/) {
    if (std::strcmp(path, "/") != 0) return -ENOENT;
    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    for (auto& f : g_state->files) {
        struct stat st{};
        st.st_mode = S_IFREG | 0444;
        st.st_size = static_cast<off_t>(f.size);
        filler(buf, f.name.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
    }
    return 0;
}

static int fl_open(const char* path, struct fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    const char* name = path + 1;
    for (size_t i = 0; i < g_state->files.size(); ++i) {
        if (g_state->files[i].name == name) {
            fi->fh = static_cast<uint64_t>(i);
            return 0;
        }
    }
    return -ENOENT;
}

static int fl_read(const char* /*path*/, char* buf, size_t buf_size,
                   off_t offset, struct fuse_file_info* fi) {
    const VirtualFile& f = g_state->files[fi->fh];
    if (offset < 0 || static_cast<uint64_t>(offset) >= f.size) return 0;
    uint64_t remaining = f.size - static_cast<uint64_t>(offset);
    uint64_t to_read = remaining < buf_size ? remaining : static_cast<uint64_t>(buf_size);
    size_t got = 0;
    char err[512];
    int rc = nano_lance_fetch_external_blob(
        f.uri.c_str(),
        f.position + static_cast<uint64_t>(offset),
        to_read,
        reinterpret_cast<uint8_t*>(buf),
        static_cast<size_t>(to_read),
        &got, err, sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        std::fprintf(stderr, "fuselance: fetch %s @%llu+%llu: %s\n",
                     f.name.c_str(),
                     static_cast<unsigned long long>(f.position + static_cast<uint64_t>(offset)),
                     static_cast<unsigned long long>(to_read), err);
        return -EIO;
    }
    return static_cast<int>(got);
}

static const fuse_operations fl_ops = [] {
    fuse_operations ops{};
    ops.getattr = fl_getattr;
    ops.readdir = fl_readdir;
    ops.open    = fl_open;
    ops.read    = fl_read;
    return ops;
}();

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <lance_table_path> --filename-col <col> [--blob-col <col>] [--sort-col <col>]\n"
        "\n"
        "Mounts the Lance table as a read-only FUSE filesystem at /tmp/fuse_<basename>.\n"
        "Each row appears as a file named by --filename-col whose content is the blob.v2\n"
        "payload from --blob-col (auto-detected if omitted). Rows are ordered by --sort-col\n"
        "(ascending) or by original row order when omitted.\n"
        "\n"
        "Unmount with:  fusermount3 -u /tmp/fuse_<basename>\n",
        prog);
}

int main(int argc, char** argv) {
    const char* table_path = nullptr;
    const char* filename_col = nullptr;
    const char* blob_col_name = nullptr;  // nullptr = auto-detect
    const char* sort_col_name = nullptr;  // nullptr = row order
    std::string err;

    // Parse argv manually to avoid mixing FUSE arg passing with a library.
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto need_val = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "fuselance: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--filename-col") { filename_col = need_val("--filename-col"); if (!filename_col) return 2; }
        else if (a == "--blob-col") { blob_col_name = need_val("--blob-col"); if (!blob_col_name) return 2; }
        else if (a == "--sort-col") { sort_col_name = need_val("--sort-col"); if (!sort_col_name) return 2; }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else if (a[0] != '-') {
            if (table_path) { std::fprintf(stderr, "fuselance: unexpected argument: %s\n", argv[i]); return 2; }
            table_path = argv[i];
        } else {
            std::fprintf(stderr, "fuselance: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (!table_path) { usage(argv[0]); return 2; }
    if (!filename_col) {
        std::fprintf(stderr, "fuselance: --filename-col is required\n");
        usage(argv[0]);
        return 2;
    }

    // Derive the mountpoint from the table basename (strip .lance suffix).
    std::filesystem::path tpath(table_path);
    std::string basename = tpath.stem().string();
    if (basename.empty()) basename = tpath.filename().string();
    const std::string mountpoint = "/tmp/fuse_" + basename;

    // ---- Read the Lance table -----------------------------------------------
    std::fprintf(stderr, "fuselance: reading %s ...\n", table_path);
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_table_read_dataset(tpath, schema, batches, err)) {
        std::fprintf(stderr, "fuselance: read failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "fuselance: loaded %zu batch(es)\n", batches.size());

    // ---- Locate columns -----------------------------------------------------
    const int fname_idx = child_index(schema, filename_col);
    if (fname_idx < 0) {
        std::fprintf(stderr, "fuselance: column '%s' not found in schema\n", filename_col);
        return 1;
    }

    int blob_idx = blob_col_name ? child_index(schema, blob_col_name) : detect_blob_col(schema);
    if (blob_idx < 0) {
        if (blob_col_name)
            std::fprintf(stderr, "fuselance: blob column '%s' not found\n", blob_col_name);
        else
            std::fprintf(stderr, "fuselance: no blob.v2 column detected (use --blob-col)\n");
        return 1;
    }
    const ArrowSchema& blob_schema = *schema.children[blob_idx];
    const int c_uri  = child_index(blob_schema, "uri") >= 0 ? child_index(blob_schema, "uri") : child_index(blob_schema, "blob_uri");
    const int c_pos  = child_index(blob_schema, "position");
    const int c_size = child_index(blob_schema, "size");
    if (c_uri < 0 || c_pos < 0 || c_size < 0) {
        std::fprintf(stderr, "fuselance: blob column '%s' missing uri/position/size children\n",
                     blob_schema.name ? blob_schema.name : "?");
        return 1;
    }

    int sort_idx = -1;
    if (sort_col_name) {
        sort_idx = child_index(schema, sort_col_name);
        if (sort_idx < 0) {
            std::fprintf(stderr, "fuselance: sort column '%s' not found\n", sort_col_name);
            return 1;
        }
    }

    // ---- Build virtual file list -------------------------------------------
    struct FileEntry {
        std::string sort_key_str;
        VirtualFile file;
    };
    std::vector<FileEntry> entries;

    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError ae{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            std::fprintf(stderr, "fuselance: view init: %s\n", ArrowErrorMessage(&ae));
            ArrowArrayViewReset(&view);
            return 1;
        }

        const ArrowArrayView* fname_view = view.children[fname_idx];
        const ArrowArrayView* blob_view  = view.children[blob_idx];
        const ArrowArrayView* sort_view  = sort_idx >= 0 ? view.children[sort_idx] : nullptr;

        // Validate the filename column type once (string or integer).
        std::string probe;
        if (view.length > 0 && !cell_to_string(fname_view, 0, probe)) {
            std::fprintf(stderr, "fuselance: --filename-col '%s' must be a string or integer column\n",
                         filename_col);
            ArrowArrayViewReset(&view);
            return 1;
        }

        for (int64_t r = 0; r < view.length; ++r) {
            // Filename
            std::string name;
            cell_to_string(fname_view, r, name);

            // Blob ref
            ArrowStringView uri_sv = ArrowArrayViewGetStringUnsafe(blob_view->children[c_uri], r);
            std::string uri(uri_sv.data, static_cast<size_t>(uri_sv.size_bytes));
            uint64_t pos  = ArrowArrayViewGetUIntUnsafe(blob_view->children[c_pos], r);
            uint64_t sz   = ArrowArrayViewGetUIntUnsafe(blob_view->children[c_size], r);

            // Sort key
            std::string sk;
            if (sort_view) {
                sk = sort_key(sort_view, r);
            } else {
                // Default: preserve insertion order via zero-padded index.
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%020zu", entries.size());
                sk = buf;
            }

            entries.push_back({std::move(sk), {std::move(name), std::move(uri), pos, sz}});
        }
        ArrowArrayViewReset(&view);
    }

    // Sort
    std::stable_sort(entries.begin(), entries.end(),
                     [](const FileEntry& a, const FileEntry& b) { return a.sort_key_str < b.sort_key_str; });

    // Deduplicate names and populate global state.
    FuseLanceState state;
    state.files.reserve(entries.size());
    for (auto& e : entries) {
        e.file.name = unique_name(e.file.name, state.files);
        state.files.push_back(std::move(e.file));
    }
    g_state = &state;

    // The virtual file list owns string copies of name/uri, so the Arrow buffers (potentially large)
    // need not survive the mount — release them now, before fuse_main blocks.
    if (schema.release) schema.release(&schema);
    for (auto& b : batches) { if (b.release) b.release(&b); }
    batches.clear();

    std::fprintf(stderr, "fuselance: %zu file(s) in virtual filesystem\n", state.files.size());

    // ---- Create mountpoint and launch FUSE ----------------------------------
    std::error_code ec;
    std::filesystem::create_directories(mountpoint, ec);
    if (ec) {
        std::fprintf(stderr, "fuselance: cannot create mountpoint %s: %s\n",
                     mountpoint.c_str(), ec.message().c_str());
        return 1;
    }

    std::fprintf(stderr, "fuselance: mounting at %s (Ctrl-C or fusermount3 -u to unmount)\n",
                 mountpoint.c_str());

    // Build FUSE argv: run in foreground (-f) so the process owns the mount lifetime.
    const char* fuse_argv[] = {argv[0], "-f", mountpoint.c_str(), nullptr};
    int fuse_argc = 3;

    int ret = fuse_main(fuse_argc, const_cast<char**>(fuse_argv), &fl_ops, nullptr);
    return ret;
}
