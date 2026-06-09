#pragma once

// Write one decoded-PDU list as its own Lance table: a `packet_id` column (link back to the packets
// table) followed by the reflected header columns. Reuses the nanotins arrow glue, so adding a protocol
// needs no table-writing code. MAC / IPv4 / IPv6 address columns are fixed-size-binary.

#include "nanotins/arrow_glue.hpp"
#include "protocol_decode.hpp"

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace pdu_io {

template <class T>
bool write_pdu_table(const std::filesystem::path& path, const protocols::PduColumn<T>& col, bool compress,
                     std::string& error) {
    if (col.size() == 0) {
        return true;  // no instances of this PDU in the capture -> no table
    }

    nanotins::soa<T> soa;
    soa.resize(col.size());
    for (std::size_t i = 0; i < col.size(); ++i) {
        soa.store(i, col.rows[i]);
    }

    constexpr std::size_t kCols = nanotins::column_count<T>;
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<int64_t>(kCols + 1)) != NANOARROW_OK) {
        error = "pdu schema alloc failed";
        return false;
    }
    if (!nanotins::nt_set_column_schema(schema.children[0], nanotins::arrow_kind::u64, 0, "packet_id", error) ||
        !nanotins::nt_fill_struct_schema<T>(&schema, 1, error)) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;

    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        error = "pdu array init failed";
        ArrowSchemaRelease(&schema);
        return false;
    }
    for (std::size_t i = 0; i < col.size(); ++i) {
        if (ArrowArrayAppendUInt(batch.children[0], col.packet_id[i]) != NANOARROW_OK ||
            !nanotins::nt_append_scalar_row<T>(&batch, 1, soa, i) ||
            ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
            error = "pdu row append failed";
            batch.release(&batch);
            schema.release(&schema);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = "pdu array finalize failed";
        batch.release(&batch);
        schema.release(&schema);
        return false;
    }

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    NanoLanceWriter writer{};
    bool ok = nano_lance_writer_init(&writer, path.string().c_str(), 3) == NANO_LANCE_OK;
    if (ok) {
        nano_lance_writer_set_ignore_nullability(&writer, true);
        nano_lance_writer_set_compression(&writer, compress);
        ok = nano_lance_write_batch(&writer, &batch, &schema) == NANO_LANCE_OK &&
             nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK;
    }
    if (!ok) {
        error = nano_lance_writer_last_error(&writer);
    }
    nano_lance_writer_close(&writer);
    batch.release(&batch);
    schema.release(&schema);
    return ok;
}

}  // namespace pdu_io
