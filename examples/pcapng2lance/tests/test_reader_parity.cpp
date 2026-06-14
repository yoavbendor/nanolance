// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Write/read parity gate: nanolance must read back EVERY fixed-width type it can write, including when
// a lance.blob.v2 column is present (which forces the reader's per-row decode path — the path that
// previously only handled u8/u32/u64/i64). Covers signed/unsigned ints 8..64, float, double, bool, and
// fixed-size-binary. The blob column is what makes this exercise the previously-broken path.

#include "soatins/arrow_glue.hpp"

#include "nanolance/blob_builder.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <boost/describe.hpp>
#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "parity test failed: %s\n", msg);
        std::exit(1);
    }
}

struct AllTypes {
    std::int8_t i8;
    std::uint8_t u8;
    std::int16_t i16;
    std::uint16_t u16;
    std::int32_t i32;
    std::uint32_t u32;
    std::int64_t i64;
    std::uint64_t u64;
    float f32;
    double f64;
    std::array<std::uint8_t, 6> mac;  // fixed_size_binary(6)
};
BOOST_DESCRIBE_STRUCT(AllTypes, (), (i8, u8, i16, u16, i32, u32, i64, u64, f32, f64, mac))

int child_by_name(const ArrowSchema& s, const char* name) {
    for (int64_t i = 0; i < s.n_children; ++i) {
        if (s.children[i]->name && std::strcmp(s.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "nanotins_reader_parity";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    const std::array<AllTypes, 2> rows = {
        AllTypes{-8, 200, -300, 60000, -70000, 4000000000U, -5000000000LL, 18000000000000000000ULL, 1.5f,
                 2.5, {1, 2, 3, 4, 5, 6}},
        AllTypes{7, 8, 9, 10, 11, 12, 13, 14, -3.25f, 9.75, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}}};

    soatins::soa<AllTypes> s;
    s.resize(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        s.store(i, rows[i]);
    }

    // Combined batch: AllTypes scalar columns + a lance.blob.v2 payload_ref (forces per-row decode).
    constexpr std::size_t kScalarCols = soatins::column_count<AllTypes>;
    std::string err;
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    require(ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(kScalarCols + 1)) == NANOARROW_OK, "alloc schema");
    require(soatins::nt_fill_struct_schema<AllTypes>(&schema, 0, err), err.c_str());
    {
        ArrowSchema blob{};
        require(nano_lance::build_blob_v2_payload_schema(blob, err), err.c_str());
        ArrowSchemaRelease(schema.children[kScalarCols]);
        std::memcpy(schema.children[kScalarCols], &blob, sizeof(ArrowSchema));
        blob.release = nullptr;
    }
    schema.flags = 0;

    ArrowArray batch{};
    require(ArrowArrayInitFromSchema(&batch, &schema, nullptr) == NANOARROW_OK, "alloc array");
    require(ArrowArrayStartAppending(&batch) == NANOARROW_OK, "start appending");
    ArrowArray* payload = batch.children[kScalarCols];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        require(soatins::nt_append_scalar_row<AllTypes>(&batch, 0, s, i), "append scalars");
        const std::string uri = "file:///dev/null";
        ArrowStringView uri_view{uri.data(), static_cast<int64_t>(uri.size())};
        require(ArrowArrayAppendNull(payload->children[0], 1) == NANOARROW_OK, "blob data");
        require(ArrowArrayAppendString(payload->children[1], uri_view) == NANOARROW_OK, "blob uri");
        require(ArrowArrayAppendUInt(payload->children[2], i + 1) == NANOARROW_OK, "blob pos");
        require(ArrowArrayAppendUInt(payload->children[3], i + 1) == NANOARROW_OK, "blob size");
        require(ArrowArrayFinishElement(payload) == NANOARROW_OK, "finish payload");
        require(ArrowArrayFinishElement(&batch) == NANOARROW_OK, "finish row");
    }
    require(ArrowArrayFinishBuildingDefault(&batch, nullptr) == NANOARROW_OK, "finalize");

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, dir.string().c_str(), 0) == NANO_LANCE_OK, "init");
    nano_lance_writer_set_ignore_nullability(&writer, true);
    require(nano_lance_write_batch(&writer, &batch, &schema) == NANO_LANCE_OK, nano_lance_writer_last_error(&writer));
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, nano_lance_writer_last_error(&writer));
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    batch.release(&batch);
    schema.release(&schema);

    // Read back via the nanolance reader (the blob column forces the per-row decode path).
    ArrowSchema rschema{};
    std::vector<ArrowArray> batches;
    require(nano_lance::lance_table_read_dataset(dir, rschema, batches, err), ("read: " + err).c_str());
    require(batches.size() == 1, "one batch");
    ArrowError ae;
    ArrowArrayView view{};
    require(ArrowArrayViewInitFromSchema(&view, &rschema, &ae) == NANOARROW_OK, "view init");
    require(ArrowArrayViewSetArray(&view, &batches[0], &ae) == NANOARROW_OK, "view set");

    const int ci8 = child_by_name(rschema, "i8"), cu8 = child_by_name(rschema, "u8");
    const int ci16 = child_by_name(rschema, "i16"), cu16 = child_by_name(rschema, "u16");
    const int ci32 = child_by_name(rschema, "i32"), cu32 = child_by_name(rschema, "u32");
    const int ci64 = child_by_name(rschema, "i64"), cu64 = child_by_name(rschema, "u64");
    const int cf32 = child_by_name(rschema, "f32"), cf64 = child_by_name(rschema, "f64");
    const int cmac = child_by_name(rschema, "mac");
    require(ci8 >= 0 && cu8 >= 0 && ci16 >= 0 && cu16 >= 0 && ci32 >= 0 && cu32 >= 0 && ci64 >= 0 &&
                cu64 >= 0 && cf32 >= 0 && cf64 >= 0 && cmac >= 0,
            "all columns present");

    for (int i = 0; i < 2; ++i) {
        const auto& r = rows[i];
        require(ArrowArrayViewGetIntUnsafe(view.children[ci8], i) == r.i8, "i8");
        require(ArrowArrayViewGetUIntUnsafe(view.children[cu8], i) == r.u8, "u8");
        require(ArrowArrayViewGetIntUnsafe(view.children[ci16], i) == r.i16, "i16");
        require(ArrowArrayViewGetUIntUnsafe(view.children[cu16], i) == r.u16, "u16");
        require(ArrowArrayViewGetIntUnsafe(view.children[ci32], i) == r.i32, "i32");
        require(ArrowArrayViewGetUIntUnsafe(view.children[cu32], i) == r.u32, "u32");
        require(ArrowArrayViewGetIntUnsafe(view.children[ci64], i) == r.i64, "i64");
        require(ArrowArrayViewGetUIntUnsafe(view.children[cu64], i) == r.u64, "u64");
        require(std::fabs(ArrowArrayViewGetDoubleUnsafe(view.children[cf32], i) - r.f32) < 1e-6, "f32");
        require(std::fabs(ArrowArrayViewGetDoubleUnsafe(view.children[cf64], i) - r.f64) < 1e-12, "f64");
        ArrowBufferView mac = ArrowArrayViewGetBytesUnsafe(view.children[cmac], i);
        require(mac.size_bytes == 6 && std::memcmp(mac.data.data, r.mac.data(), 6) == 0, "mac");
    }

    ArrowArrayViewReset(&view);
    ArrowSchemaRelease(&rschema);
    for (auto& b : batches) {
        if (b.release) {
            b.release(&b);
        }
    }
    std::puts("nanolance reader parity ok (all fixed-width types read back with a blob column present)");
    return 0;
}
