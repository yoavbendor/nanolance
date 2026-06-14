// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlance_blobgen — build a Lance dataset of external lance.blob.v2 references that tiles each input
// object into consecutive, packet-sized random chunks. Models the real workload: N external objects (e.g.
// one pcapng per hour) each sliced front-to-back into rows, so a single Lance file spans many hours of
// externally-stored payload. Two columns: `packet_id` (running global index) + `payload_ref` (the
// blob.v2 external descriptor: uri, position, size). Validity is structural — chunks never run past EOF.
//
// Sizes are passed in (the bench script reads them via stat / `aws s3api head-object`), so this tool needs
// no S3 build. Chunking per file: pos=0; len = min + rand()%span (last chunk clamped to size-pos); repeat
// until pos==size; then reset for the next uri.
//
//   nlance_blobgen --out out.lance --uris s3://b/h00,s3://b/h01 --sizes 12000000,11500000 [--seed N]

#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/version.hpp"

#include <CLI/CLI.hpp>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Generate a Lance dataset of external blob.v2 refs tiling each object into random chunks"};
    std::string out_path, uris_csv, sizes_csv;
    std::uint64_t seed = 1;
    std::uint64_t min_len = 64, span = 9000;  // len in [min_len, min_len+span)
    bool compress = true;
    app.add_option("--out", out_path, "Output .lance dataset path")->required();
    app.add_option("--uris", uris_csv, "Comma-separated external object URIs (file:// or s3://)")->required();
    app.add_option("--sizes", sizes_csv, "Comma-separated byte sizes, parallel to --uris")->required();
    app.add_option("--seed", seed, "RNG seed (reproducible chunking)")->default_val(1);
    app.add_option("--min", min_len, "Minimum chunk length")->default_val(64);
    app.add_option("--span", span, "Chunk length is min + rand()%span")->default_val(9000);
    app.add_flag("!--no-compress", compress, "Disable zstd compression");
    app.set_version_flag("--version", std::string(nanolance::library_version()));
    CLI11_PARSE(app, argc, argv);

    const std::vector<std::string> uris = split_csv(uris_csv);
    const std::vector<std::string> sizes_str = split_csv(sizes_csv);
    if (uris.size() != sizes_str.size() || uris.empty()) {
        std::fprintf(stderr, "nlance_blobgen: --uris and --sizes must be non-empty and equal length (%zu vs %zu)\n",
                     uris.size(), sizes_str.size());
        return 2;
    }
    if (span == 0) {
        std::fprintf(stderr, "nlance_blobgen: --span must be > 0\n");
        return 2;
    }

    // Tile each object into consecutive random chunks -> parallel (packet_id, BlobV2Row) arrays.
    std::vector<std::uint64_t> ids;
    std::vector<nano_lance::BlobV2Row> rows;
    std::mt19937_64 rng(seed);
    std::uint64_t index = 0;
    for (std::size_t f = 0; f < uris.size(); ++f) {
        const std::uint64_t size = std::strtoull(sizes_str[f].c_str(), nullptr, 10);
        if (size == 0) {
            std::fprintf(stderr, "nlance_blobgen: object %s has size 0 (skipped)\n", uris[f].c_str());
            continue;
        }
        std::uint64_t pos = 0;
        std::uint64_t file_chunks = 0;
        while (pos < size) {
            std::uint64_t len = min_len + (rng() % span);
            if (pos + len > size) {
                len = size - pos;  // clamp the final chunk to land exactly on EOF
            }
            nano_lance::BlobV2Row row;
            row.uri = uris[f];
            row.position = pos;
            row.size = len;
            rows.push_back(std::move(row));
            ids.push_back(index++);
            pos += len;
            ++file_chunks;
        }
        std::fprintf(stderr, "nlance_blobgen: %s -> %llu chunks (%llu bytes)\n", uris[f].c_str(),
                     static_cast<unsigned long long>(file_chunks), static_cast<unsigned long long>(size));
    }
    if (rows.empty()) {
        std::fprintf(stderr, "nlance_blobgen: no rows generated\n");
        return 1;
    }

    // Build the packet_id + payload_ref record batch and write it as a single Lance fragment.
    ArrowSchema schema{};
    ArrowArray batch{};
    std::string err;
    if (!nano_lance::build_epb_table_schema(schema, err)) {
        std::fprintf(stderr, "nlance_blobgen: schema: %s\n", err.c_str());
        return 1;
    }
    if (!nano_lance::build_epb_table_array(ids, rows, batch, err)) {
        std::fprintf(stderr, "nlance_blobgen: array: %s\n", err.c_str());
        schema.release(&schema);
        return 1;
    }

    NanoLanceWriter writer{};
    auto fail = [&](const std::string& msg) {
        std::fprintf(stderr, "nlance_blobgen: %s: %s\n", msg.c_str(), nano_lance_writer_last_error(&writer));
        return 1;
    };
    if (nano_lance_writer_init(&writer, out_path.c_str(), 0) != NANO_LANCE_OK) {
        return fail("writer init");
    }
    nano_lance_writer_set_ignore_nullability(&writer, true);
    nano_lance_writer_set_compression(&writer, compress);
    int rc = 0;
    if (nano_lance_write_batch(&writer, &batch, &schema) != NANO_LANCE_OK) {
        rc = fail("write_batch");
    } else if (nano_lance_writer_commit(&writer, /*is_append=*/false) != NANO_LANCE_OK) {
        rc = fail("commit");
    } else if (nano_lance_writer_close(&writer) != NANO_LANCE_OK) {
        rc = fail("close");
    }
    batch.release(&batch);
    schema.release(&schema);
    if (rc == 0) {
        std::fprintf(stderr, "nlance_blobgen: wrote %zu rows across %zu objects -> %s\n", rows.size(),
                     uris.size(), out_path.c_str());
    }
    return rc;
}
