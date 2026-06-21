// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// fuselance — mount a Lance table with blob.v2 entries as a read-only FUSE filesystem.
//
// Usage:
//   fuselance <lance_table_path> --filename-col <col>
//             [--blob-col <col>]
//             [--join-blob-from <blob_table_path>]
//             [--sort-col <col>]
//
// Each DISTINCT value of --filename-col becomes a file. The file's content is the concatenation
// of all blob.v2 payloads for rows sharing that value, in row order (or --sort-col order within
// the group).
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

#include <nanoarrow/nanoarrow.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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
    const char* name = path + 1;
    for (auto& f : g_state->files) {
        if (f.name == name) {
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = static_cast<off_t>(f.total_size);
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
        st.st_size = static_cast<off_t>(f.total_size);
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

// Read spanning multiple blob segments transparently.
static int fl_read(const char* /*path*/, char* buf, size_t buf_size,
                   off_t offset, struct fuse_file_info* fi) {
    const VirtualFile& f = g_state->files[fi->fh];
    if (offset < 0 || static_cast<uint64_t>(offset) >= f.total_size) return 0;

    uint64_t file_off = static_cast<uint64_t>(offset);
    size_t total_written = 0;

    for (const BlobSeg& seg : f.segs) {
        if (total_written >= buf_size) break;
        if (file_off >= seg.size) { file_off -= seg.size; continue; }  // skip segments before offset

        uint64_t seg_off = file_off;
        uint64_t seg_avail = seg.size - seg_off;
        size_t want = std::min(static_cast<uint64_t>(buf_size - total_written), seg_avail);

        size_t got = 0;
        char ferr[512];
        int rc = nano_lance_fetch_external_blob(
            seg.uri.c_str(), seg.position + seg_off, want,
            reinterpret_cast<uint8_t*>(buf + total_written), want,
            &got, ferr, sizeof(ferr));
        if (rc != NANO_LANCE_READER_OK) {
            std::fprintf(stderr, "fuselance: fetch %s seg @%llu+%llu: %s\n",
                         f.name.c_str(),
                         static_cast<unsigned long long>(seg.position + seg_off),
                         static_cast<unsigned long long>(want), ferr);
            return total_written > 0 ? static_cast<int>(total_written) : -EIO;
        }
        total_written += got;
        file_off = 0;  // consumed the within-segment offset; subsequent segs start at 0
    }
    return static_cast<int>(total_written);
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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <lance_table_path> --filename-col <col>\n"
        "          [--blob-col <col>] [--join-blob-from <blob_table>] [--sort-col <col>]\n"
        "\n"
        "Mounts the Lance table as a read-only FUSE filesystem at /tmp/fuse_<basename>.\n"
        "Each distinct value of --filename-col becomes one file whose content is the\n"
        "concatenation of all matching rows' blob.v2 payloads.\n"
        "\n"
        "  --blob-col <col>          blob.v2 struct column (auto-detected if omitted)\n"
        "  --join-blob-from <path>   read blob refs from a companion table joined on packet_id\n"
        "                            (use when --filename-col and the blob live in different tables)\n"
        "  --sort-col <col>          order blobs within each file by this column (ascending)\n"
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
    const char* table_path      = nullptr;
    const char* filename_col    = nullptr;
    const char* blob_col_name   = nullptr;   // nullptr = auto-detect in same table
    const char* join_blob_path  = nullptr;   // nullptr = no join; blobs in same table
    const char* sort_col_name   = nullptr;   // nullptr = row order
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
        // List column names from the manifest so the user can pick one.
        NanoLanceDatasetMetadata meta{};
        char merr[512];
        if (nano_lance_dataset_read_latest(table_path, &meta, merr, sizeof(merr)) != NANO_LANCE_READER_OK) {
            std::fprintf(stderr, "fuselance: --filename-col is required\n");
            std::fprintf(stderr, "fuselance: (also failed to read schema from '%s': %s)\n", table_path, merr);
            usage(argv[0]);
            return 2;
        }
        std::fprintf(stderr, "fuselance: --filename-col is required\n\nColumns in '%s':\n", table_path);
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
    std::filesystem::path tpath(table_path);
    std::string basename = tpath.stem().string();
    if (basename.empty()) basename = tpath.filename().string();
    const std::string mountpoint = "/tmp/fuse_" + basename;

    // ---- Optional: load blob table for join ---------------------------------
    // Maps packet_id → BlobSeg; used when --join-blob-from is specified.
    std::map<uint64_t, BlobSeg> join_blobs;
    if (join_blob_path) {
        std::fprintf(stderr, "fuselance: loading blob table %s ...\n", join_blob_path);
        if (!read_blob_table(std::filesystem::path(join_blob_path), blob_col_name, join_blobs, err)) {
            std::fprintf(stderr, "fuselance: blob table error: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "fuselance: joined %zu blob refs\n", join_blobs.size());
    }

    // ---- Read the name table ------------------------------------------------
    std::fprintf(stderr, "fuselance: reading %s ...\n", table_path);
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_table_read_dataset(tpath, schema, batches, err)) {
        std::fprintf(stderr, "fuselance: read failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "fuselance: loaded %zu batch(es)\n", batches.size());

    // ---- Locate columns in name table ---------------------------------------
    const int fname_idx = child_index(schema, filename_col);
    if (fname_idx < 0) {
        std::fprintf(stderr, "fuselance: column '%s' not found in schema\n", filename_col);
        return 1;
    }

    // Blob indices — only needed when NOT using a join table.
    BlobColIndices bi;
    if (!join_blob_path) {
        if (!resolve_blob_col(schema, blob_col_name, bi, err)) {
            std::fprintf(stderr, "fuselance: %s\n", err.c_str());
            return 1;
        }
    }

    // For join mode we need packet_id from the name table.
    const int pid_idx = join_blob_path ? child_index(schema, "packet_id") : -1;
    if (join_blob_path && pid_idx < 0) {
        std::fprintf(stderr, "fuselance: name table missing packet_id join key\n");
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

    // ---- Build grouped virtual file map ------------------------------------
    // We collect all entries into a stable map keyed by the filename value.
    // Each entry records an optional sort key and a BlobSeg (to be appended in order).
    struct RowEntry {
        std::string sk;
        BlobSeg seg;
    };
    std::map<std::string, std::vector<RowEntry>> groups;  // key = filename value

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
        const ArrowArrayView* blob_av    = !join_blob_path ? view.children[bi.blob_idx] : nullptr;
        const ArrowArrayView* sort_view  = sort_idx >= 0 ? view.children[sort_idx] : nullptr;
        const ArrowArrayView* pid_view   = pid_idx >= 0  ? view.children[pid_idx]  : nullptr;

        // Validate filename column type on first non-empty batch.
        if (view.length > 0) {
            std::string probe;
            if (!cell_to_string(fname_view, 0, probe)) {
                std::fprintf(stderr, "fuselance: --filename-col '%s' has an unsupported type\n", filename_col);
                ArrowArrayViewReset(&view);
                return 1;
            }
        }

        for (int64_t r = 0; r < view.length; ++r) {
            std::string fname;
            cell_to_string(fname_view, r, fname);

            BlobSeg seg;
            if (join_blob_path) {
                // Join on packet_id.
                uint64_t pid = ArrowArrayViewGetUIntUnsafe(pid_view, r);
                auto it = join_blobs.find(pid);
                if (it == join_blobs.end()) continue;  // no blob for this row — skip
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
                // Stable insertion order: zero-padded global index across all batches.
                char buf[24];
                size_t global = 0;
                for (auto& [k, v] : groups) global += v.size();
                std::snprintf(buf, sizeof(buf), "%020zu", global);
                sk = buf;
            }

            groups[fname].push_back({std::move(sk), std::move(seg)});
        }
        ArrowArrayViewReset(&view);
    }

    // ---- Sort within each group and build VirtualFile list -----------------
    FuseLanceState state;
    state.files.reserve(groups.size());

    for (auto& [name, entries] : groups) {
        std::stable_sort(entries.begin(), entries.end(),
                         [](const RowEntry& a, const RowEntry& b) { return a.sk < b.sk; });
        VirtualFile vf;
        vf.name = name;
        vf.segs.reserve(entries.size());
        for (auto& e : entries) {
            vf.total_size += e.seg.size;
            vf.segs.push_back(std::move(e.seg));
        }
        state.files.push_back(std::move(vf));
    }
    // Sort files by name so readdir order is deterministic.
    std::sort(state.files.begin(), state.files.end(),
              [](const VirtualFile& a, const VirtualFile& b) { return a.name < b.name; });

    g_state = &state;

    // Release Arrow buffers before blocking in fuse_main.
    if (schema.release) schema.release(&schema);
    for (auto& b : batches) { if (b.release) b.release(&b); }
    batches.clear();

    std::fprintf(stderr, "fuselance: %zu distinct file(s) in virtual filesystem\n", state.files.size());
    if (state.files.size() <= 20) {
        for (auto& f : state.files)
            std::fprintf(stderr, "  %-40s  %llu byte(s) across %zu segment(s)\n",
                         f.name.c_str(), static_cast<unsigned long long>(f.total_size), f.segs.size());
    }

    // ---- Create mountpoint and launch FUSE ---------------------------------
    std::error_code ec;
    std::filesystem::create_directories(mountpoint, ec);
    if (ec) {
        std::fprintf(stderr, "fuselance: cannot create mountpoint %s: %s\n",
                     mountpoint.c_str(), ec.message().c_str());
        return 1;
    }
    std::fprintf(stderr, "fuselance: mounting at %s (Ctrl-C or fusermount3 -u to unmount)\n", mountpoint.c_str());

    const char* fuse_argv[] = {argv[0], "-f", mountpoint.c_str(), nullptr};
    int fuse_argc = 3;
    return fuse_main(fuse_argc, const_cast<char**>(fuse_argv), &fl_ops, nullptr);
}
