// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Byte-identity test for the typed facade's lance.blob.v2 column (blob_ref_column): the same
// packet_id + external payload_ref data is written once through the typed writer and once through
// the established C path (build_epb_table_schema/build_epb_table_array + nano_lance_write_batch),
// and the .lance data files must compare equal byte-for-byte. That proves the facade's hand-built
// Arrow shape (struct child, extension metadata, all-empty non-null `data` child) hits the exact
// same ingest as the nanoarrow-built batch.
#include "nanolance/blob_builder.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/typed_writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_typed_blob_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::filesystem::path first_data_file(const std::filesystem::path& ds) {
    for (const auto& e : std::filesystem::directory_iterator(ds / "data")) {
        if (e.path().extension() == ".lance") {
            return e.path();
        }
    }
    require(false, "no data file written");
    return {};
}

}  // namespace

int main() {
    namespace nt = nano_lance::typed;
    std::string error;

    const std::size_t n = 4096;
    const std::string shared_uri = "file:///captures/SRL_front_left_51.pcapng";
    std::vector<std::uint64_t> packet_ids(n);
    std::vector<nt::blob_ref> refs(n);
    std::vector<nano_lance::BlobV2Row> rows(n);
    std::uint64_t position = 128;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t size = 64U + (i * 37U) % 1400U;
        packet_ids[i] = i + 1U;
        refs[i] = nt::blob_ref{shared_uri, position, size};
        rows[i] = nano_lance::BlobV2Row{std::nullopt, shared_uri, position, size};
        position += size + 32U;
    }

    // Reference dataset via the C path (nanoarrow-built batch).
    const auto ds_c = temp_dataset("c_api");
    {
        ArrowSchema schema{};
        require(nano_lance::build_epb_table_schema(schema, error), error.c_str());
        ArrowArray batch{};
        require(nano_lance::build_epb_table_array(packet_ids, rows, batch, error), error.c_str());
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, ds_c.string().c_str(), 3) == NANO_LANCE_OK, "c init");
        require(nano_lance_writer_set_ignore_nullability(&w, true) == NANO_LANCE_OK, "c nullability");
        require(nano_lance_write_batch(&w, &batch, &schema) == NANO_LANCE_OK, nano_lance_writer_last_error(&w));
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, nano_lance_writer_last_error(&w));
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "c close");
        batch.release(&batch);
        ArrowSchemaRelease(&schema);
    }

    // Same data via the typed facade.
    const auto ds_typed = temp_dataset("typed");
    {
        using EpbSchema = nt::schema<nt::column<std::uint64_t, "packet_id">, nt::blob_ref_column<"payload_ref">>;
        nt::writer<EpbSchema> w(ds_typed.string().c_str(), {});
        require(w.ok(), "typed writer construction");
        require(w.write_batch(std::span<const std::uint64_t>(packet_ids.data(), n),
                              std::span<const nt::blob_ref>(refs.data(), n)),
                w.last_error());
        require(w.commit(), w.last_error());
        require(w.close(), "typed close");
    }

    require(read_file_bytes(first_data_file(ds_c)) == read_file_bytes(first_data_file(ds_typed)),
            "typed blob column must produce byte-identical data files to the C path");

    std::error_code ec;
    std::filesystem::remove_all(ds_c, ec);
    std::filesystem::remove_all(ds_typed, ec);
    std::cerr << "typed blob writer: blob_ref_column byte-identical to the C blob path\n";
    return 0;
}
