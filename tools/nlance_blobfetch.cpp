// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlance_blobfetch — the nanolance side of the blob-fetch benchmark. Reads a lance.blob.v2 dataset, and
// for each row (or a chosen subset) calls nano_lance_fetch_external_blob(uri, position, size) — the
// nimble AWS-C++-SDK path on S3, plain file IO on file:// — then MD5s the bytes and prints one line per
// blob: `index <TAB> md5 <TAB> fetch_nanos`. Only the fetch call is timed (not MD5/IO setup), so the
// number is comparable to pylance's take_blobs().read(). The bench script diffs the md5 column against
// pylance and aggregates the timings.

#include "md5_util.hpp"

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_reader.h"
#include "nanolance/version.hpp"

#include <CLI/CLI.hpp>
#include <nanoarrow/nanoarrow.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

int child_by_name(const ArrowSchema& s, const char* name) {
    for (std::int64_t i = 0; i < s.n_children; ++i) {
        if (s.children[i]->name && std::strcmp(s.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Fetch external blob.v2 payloads via nanolance and MD5 each (benchmark fetcher)"};
    std::string dataset;
    std::string indices_csv;  // empty = all rows
    app.add_option("dataset", dataset, "Input .lance dataset")->required();
    app.add_option("--indices", indices_csv, "Comma-separated row indices to fetch (default: all)");
    app.set_version_flag("--version", std::string(nanolance::library_version()));
    CLI11_PARSE(app, argc, argv);

    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string err;
    if (!nano_lance::lance_table_read_dataset(dataset, schema, batches, err)) {
        std::fprintf(stderr, "nlance_blobfetch: read failed: %s\n", err.c_str());
        return 1;
    }

    const int blob_col = child_by_name(schema, "payload_ref");
    if (blob_col < 0) {
        std::fprintf(stderr, "nlance_blobfetch: no payload_ref column\n");
        return 1;
    }
    const ArrowSchema& blob_schema = *schema.children[blob_col];
    const int c_pos = child_by_name(blob_schema, "position");
    const int c_size = child_by_name(blob_schema, "size");
    // nanolance's reader exposes the ingest shape (`uri`); pylance's physical layout calls it `blob_uri`.
    int c_uri = child_by_name(blob_schema, "uri");
    if (c_uri < 0) {
        c_uri = child_by_name(blob_schema, "blob_uri");
    }
    if (c_pos < 0 || c_size < 0 || c_uri < 0) {
        std::fprintf(stderr, "nlance_blobfetch: payload_ref missing position/size/uri\n");
        return 1;
    }

    // Flatten all batches into one logical row sequence (uri, position, size).
    struct Ref {
        std::string uri;
        std::uint64_t position;
        std::uint64_t size;
    };
    std::vector<Ref> refs;
    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError ae{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            std::fprintf(stderr, "nlance_blobfetch: view: %s\n", ArrowErrorMessage(&ae));
            ArrowArrayViewReset(&view);
            return 1;
        }
        const ArrowArrayView* blob = view.children[blob_col];
        for (std::int64_t r = 0; r < view.length; ++r) {
            ArrowStringView u = ArrowArrayViewGetStringUnsafe(blob->children[c_uri], r);
            refs.push_back(Ref{std::string(u.data, static_cast<std::size_t>(u.size_bytes)),
                               static_cast<std::uint64_t>(ArrowArrayViewGetUIntUnsafe(blob->children[c_pos], r)),
                               static_cast<std::uint64_t>(ArrowArrayViewGetUIntUnsafe(blob->children[c_size], r))});
        }
        ArrowArrayViewReset(&view);
    }

    // Which indices to fetch.
    std::vector<std::size_t> want;
    if (indices_csv.empty()) {
        want.resize(refs.size());
        for (std::size_t i = 0; i < refs.size(); ++i) {
            want[i] = i;
        }
    } else {
        std::stringstream ss(indices_csv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                want.push_back(static_cast<std::size_t>(std::strtoull(item.c_str(), nullptr, 10)));
            }
        }
    }

    std::vector<std::uint8_t> buf;
    char ferr[512];
    for (std::size_t idx : want) {
        if (idx >= refs.size()) {
            std::fprintf(stderr, "nlance_blobfetch: index %zu out of range (%zu rows)\n", idx, refs.size());
            return 1;
        }
        const Ref& ref = refs[idx];
        buf.resize(ref.size);
        std::size_t got = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = nano_lance_fetch_external_blob(ref.uri.c_str(), ref.position, ref.size, buf.data(),
                                                      buf.size(), &got, ferr, sizeof ferr);
        const auto t1 = std::chrono::steady_clock::now();
        if (rc != NANO_LANCE_READER_OK) {
            std::fprintf(stderr, "nlance_blobfetch: fetch idx %zu (%s @%llu+%llu) failed: %s\n", idx,
                         ref.uri.c_str(), static_cast<unsigned long long>(ref.position),
                         static_cast<unsigned long long>(ref.size), ferr);
            return 1;
        }
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const std::string md5 = md5util::md5_hex(buf.data(), got);
        std::printf("%zu\t%s\t%lld\n", idx, md5.c_str(), static_cast<long long>(nanos));
    }

    if (schema.release) {
        schema.release(&schema);
    }
    for (auto& b : batches) {
        if (b.release) {
            b.release(&b);
        }
    }
    return 0;
}
