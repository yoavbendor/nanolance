// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Tests for nano_lance_writer_set_column_encoding (declared encodings, skipping detection scans) and
// nano_lance_writer_set_borrow_buffers (zero-copy fixed-width ingest). The core guarantees under test:
// (1) borrow mode produces BYTE-IDENTICAL .lance data files to copy mode, (2) declared encodings
// round-trip values exactly and reject type-incompatible declarations at commit, (3) a borrowed column
// receiving a second batch silently falls back to copying and still round-trips.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& msg) {
    require(ok, msg.c_str());
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_writer_options_" + std::string(suffix));
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

struct Batch {
    ArrowSchema c0{};
    ArrowSchema c1{};
    ArrowSchema* kids[2]{};
    ArrowSchema root{};
    const void* b0[2]{};
    const void* b1[2]{};
    ArrowArray a0{};
    ArrowArray a1{};
    ArrowArray* akids[2]{};
    const void* rb[1]{};
    ArrowArray aroot{};

    Batch(const std::vector<std::uint64_t>& ts, const std::vector<std::uint32_t>& caplen) {
        c0.format = "L";
        c0.name = "ts";
        c1.format = "I";
        c1.name = "caplen";
        kids[0] = &c0;
        kids[1] = &c1;
        root.format = "+s";
        root.name = "";
        root.n_children = 2;
        root.children = kids;
        b0[0] = nullptr;
        b0[1] = ts.data();
        b1[0] = nullptr;
        b1[1] = caplen.data();
        a0.length = static_cast<std::int64_t>(ts.size());
        a0.n_buffers = 2;
        a0.buffers = b0;
        a1.length = static_cast<std::int64_t>(caplen.size());
        a1.n_buffers = 2;
        a1.buffers = b1;
        akids[0] = &a0;
        akids[1] = &a1;
        rb[0] = nullptr;
        aroot.length = static_cast<std::int64_t>(ts.size());
        aroot.n_buffers = 1;
        aroot.buffers = rb;
        aroot.n_children = 2;
        aroot.children = akids;
    }
};

void write_two_col(const std::filesystem::path& ds, const std::vector<std::uint64_t>& ts,
                   const std::vector<std::uint32_t>& caplen, bool borrow, bool hints) {
    Batch batch(ts, caplen);
    NanoLanceWriter w{};
    require(nano_lance_writer_init(&w, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_ignore_nullability(&w, true) == NANO_LANCE_OK, "nullability");
    if (borrow) {
        require(nano_lance_writer_set_borrow_buffers(&w, true) == NANO_LANCE_OK, "borrow");
    }
    if (hints) {
        require(nano_lance_writer_set_column_encoding(&w, "ts", "bitpack") == NANO_LANCE_OK, "hint ts");
        require(nano_lance_writer_set_column_encoding(&w, "caplen", "bitpack") == NANO_LANCE_OK, "hint caplen");
    }
    require(nano_lance_write_batch(&w, &batch.aroot, &batch.root) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
}

void verify_roundtrip(const std::filesystem::path& ds, const std::vector<std::uint64_t>& ts,
                      const std::vector<std::uint32_t>& caplen) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error);
    require(batches.size() == 1U, "one batch");
    require(batches[0].n_children == 2, "two columns");
    const auto* got_ts = static_cast<const std::uint64_t*>(batches[0].children[0]->buffers[1]);
    const auto* got_caplen = static_cast<const std::uint32_t*>(batches[0].children[1]->buffers[1]);
    require(static_cast<std::size_t>(batches[0].children[0]->length) == ts.size(), "ts length");
    require(std::memcmp(got_ts, ts.data(), ts.size() * 8U) == 0, "ts values");
    require(std::memcmp(got_caplen, caplen.data(), caplen.size() * 4U) == 0, "caplen values");
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
}

}  // namespace

int main() {
    const std::size_t n = 100000;  // above the RLE sampling threshold; multi-page bitpack
    std::vector<std::uint64_t> ts(n);
    std::vector<std::uint32_t> caplen(n);
    for (std::size_t i = 0; i < n; ++i) {
        ts[i] = 1700000000000000ULL + i * 1500U + (i * 2654435761ULL) % 200U;
        caplen[i] = 60U + static_cast<std::uint32_t>((i * 40503U) % 1454U);
    }

    // 1. Borrow mode must produce byte-identical data files to copy mode (with and without hints).
    const auto ds_copy = temp_dataset("copy");
    const auto ds_borrow = temp_dataset("borrow");
    const auto ds_hint = temp_dataset("hint");
    const auto ds_borrow_hint = temp_dataset("borrow_hint");
    write_two_col(ds_copy, ts, caplen, /*borrow=*/false, /*hints=*/false);
    write_two_col(ds_borrow, ts, caplen, /*borrow=*/true, /*hints=*/false);
    write_two_col(ds_hint, ts, caplen, /*borrow=*/false, /*hints=*/true);
    write_two_col(ds_borrow_hint, ts, caplen, /*borrow=*/true, /*hints=*/true);
    require(read_file_bytes(first_data_file(ds_copy)) == read_file_bytes(first_data_file(ds_borrow)),
            "borrow mode must be byte-identical to copy mode");
    require(read_file_bytes(first_data_file(ds_hint)) == read_file_bytes(first_data_file(ds_borrow_hint)),
            "borrow+hint must be byte-identical to copy+hint");
    verify_roundtrip(ds_borrow, ts, caplen);
    verify_roundtrip(ds_borrow_hint, ts, caplen);

    // 2. Multi-batch borrow: second batch materializes the borrowed column; values still round-trip.
    {
        const auto ds = temp_dataset("borrow_multibatch");
        const std::size_t half = n / 2U;
        std::vector<std::uint64_t> ts_a(ts.begin(), ts.begin() + static_cast<std::ptrdiff_t>(half));
        std::vector<std::uint64_t> ts_b(ts.begin() + static_cast<std::ptrdiff_t>(half), ts.end());
        std::vector<std::uint32_t> cl_a(caplen.begin(), caplen.begin() + static_cast<std::ptrdiff_t>(half));
        std::vector<std::uint32_t> cl_b(caplen.begin() + static_cast<std::ptrdiff_t>(half), caplen.end());
        Batch batch_a(ts_a, cl_a);
        Batch batch_b(ts_b, cl_b);
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, ds.string().c_str(), 3) == NANO_LANCE_OK, "mb init");
        require(nano_lance_writer_set_ignore_nullability(&w, true) == NANO_LANCE_OK, "mb nullability");
        require(nano_lance_writer_set_borrow_buffers(&w, true) == NANO_LANCE_OK, "mb borrow");
        require(nano_lance_write_batch(&w, &batch_a.aroot, &batch_a.root) == NANO_LANCE_OK, "mb write a");
        require(nano_lance_write_batch(&w, &batch_b.aroot, &batch_b.root) == NANO_LANCE_OK, "mb write b");
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "mb commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "mb close");
        verify_roundtrip(ds, ts, caplen);
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }

    // 3. Declared-encoding validation: type-incompatible declarations must fail the commit loudly.
    {
        const auto ds = temp_dataset("bad_hint");
        Batch batch(ts, caplen);
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, ds.string().c_str(), 3) == NANO_LANCE_OK, "bh init");
        require(nano_lance_writer_set_ignore_nullability(&w, true) == NANO_LANCE_OK, "bh nullability");
        require(nano_lance_writer_set_column_encoding(&w, "ts", "nonsense") == NANO_LANCE_INVALID_ARGUMENT,
                "unknown encoding name must be rejected by the setter");
        require(nano_lance_writer_set_column_encoding(&w, "ts", "bss-zstd") == NANO_LANCE_OK, "bh set");
        require(nano_lance_write_batch(&w, &batch.aroot, &batch.root) == NANO_LANCE_OK, "bh write");
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_INVALID_ARGUMENT,
                "bss-zstd on an integer column must fail the commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "bh close");
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }

    // 4. "plain" declaration: values round-trip through flat pages (no structural encoding).
    {
        const auto ds = temp_dataset("plain_hint");
        Batch batch(ts, caplen);
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, ds.string().c_str(), 3) == NANO_LANCE_OK, "ph init");
        require(nano_lance_writer_set_ignore_nullability(&w, true) == NANO_LANCE_OK, "ph nullability");
        require(nano_lance_writer_set_column_encoding(&w, "ts", "plain") == NANO_LANCE_OK, "ph set");
        require(nano_lance_write_batch(&w, &batch.aroot, &batch.root) == NANO_LANCE_OK, "ph write");
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "ph commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "ph close");
        verify_roundtrip(ds, ts, caplen);
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }

    std::error_code ec;
    std::filesystem::remove_all(ds_copy, ec);
    std::filesystem::remove_all(ds_borrow, ec);
    std::filesystem::remove_all(ds_hint, ec);
    std::filesystem::remove_all(ds_borrow_hint, ec);
    std::cerr << "writer options: borrow byte-identical, hints validated, multi-batch fallback OK\n";
    return 0;
}
