#pragma once

// Helpers for staged (incremental) parsing: read a Lance table's per-row external payload reference +
// discriminator, and write a "remainder" table that points at the still-unparsed bytes for the next
// stage. Each enrich stage reads the previous stage's table, fetches the referenced bytes, decodes one
// more layer, and writes new PDU tables + the advanced remainder — all keyed by packet_id, never copying
// payload bytes (they stay external in the original capture).

#include "nanolance/blob_builder.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace staged {

// One row of an external-payload table: where the (still-unparsed) bytes live, the next-layer
// discriminator, and the owning packet id.
struct PayloadRow {
    std::uint64_t packet_id = 0;
    std::uint64_t discriminator = 0;  // link_type (-> L2), ethertype (-> L3), or ip_proto (-> L4)
    std::string uri;
    std::uint64_t off = 0;
    std::uint64_t size = 0;
};

inline int nt_child_index(const ArrowSchema& s, const char* name) {
    for (std::int64_t i = 0; i < s.n_children; ++i) {
        if (s.children[i]->name != nullptr && std::strcmp(s.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Read `packet_id` + a discriminator scalar column + the `payload_ref` blob (uri/position/size) from a
// committed Lance dataset (the packets table, or a previous remainder table).
inline bool read_payload_table(const std::filesystem::path& dir, const char* disc_col,
                               std::vector<PayloadRow>& out, std::string& error) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_table_read_dataset(dir, schema, batches, error)) {
        return false;
    }
    const auto release_all = [&]() {
        for (auto& b : batches) {
            if (b.release) {
                b.release(&b);
            }
        }
        if (schema.release) {
            schema.release(&schema);
        }
    };

    const int pid_idx = nt_child_index(schema, "packet_id");
    const int disc_idx = nt_child_index(schema, disc_col);
    const int blob_idx = nt_child_index(schema, "payload_ref");
    if (pid_idx < 0 || disc_idx < 0 || blob_idx < 0) {
        error = std::string("table missing packet_id / ") + disc_col + " / payload_ref";
        release_all();
        return false;
    }
    // lance_table_read_dataset rebuilds the blob column in INGEST shape: children data/uri/position/size.
    const ArrowSchema& blob_schema = *schema.children[blob_idx];
    const int pos_g = nt_child_index(blob_schema, "position");
    const int size_g = nt_child_index(blob_schema, "size");
    const int uri_g = nt_child_index(blob_schema, "uri");
    if (pos_g < 0 || size_g < 0 || uri_g < 0) {
        error = "payload_ref struct missing position/size/uri";
        release_all();
        return false;
    }

    bool ok = true;
    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError ae;
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            error = "failed to view payload table batch";
            ArrowArrayViewReset(&view);
            ok = false;
            break;
        }
        const ArrowArrayView* blob = view.children[blob_idx];
        for (std::int64_t i = 0; i < view.length; ++i) {
            PayloadRow r;
            r.packet_id = ArrowArrayViewGetUIntUnsafe(view.children[pid_idx], i);
            r.discriminator = ArrowArrayViewGetUIntUnsafe(view.children[disc_idx], i);
            r.off = ArrowArrayViewGetUIntUnsafe(blob->children[pos_g], i);
            r.size = ArrowArrayViewGetUIntUnsafe(blob->children[size_g], i);
            const ArrowStringView sv = ArrowArrayViewGetStringUnsafe(blob->children[uri_g], i);
            r.uri.assign(sv.data, static_cast<std::size_t>(sv.size_bytes));
            out.push_back(std::move(r));
        }
        ArrowArrayViewReset(&view);
    }
    release_all();
    return ok;
}

inline bool nt_set_scalar_child(ArrowSchema* child, ArrowType type, const char* name, std::string& error) {
    if (ArrowSchemaSetType(child, type) != NANOARROW_OK || ArrowSchemaSetName(child, name) != NANOARROW_OK) {
        error = std::string("failed to set scalar child ") + name;
        return false;
    }
    child->flags &= ~static_cast<int64_t>(ARROW_FLAG_NULLABLE);
    return true;
}

// Write a remainder table: struct[ packet_id u64, <disc_col> u64, payload_ref blob.v2 external ]. The
// blob ref points at the bytes the next stage will parse (already advanced past this stage's header).
inline bool write_remainder_table(const std::filesystem::path& path, const std::vector<PayloadRow>& rows,
                                  const char* disc_col, bool compress, std::string& error) {
    if (rows.empty()) {
        return true;
    }
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, 3) != NANOARROW_OK) {
        error = "remainder schema alloc failed";
        return false;
    }
    if (!nt_set_scalar_child(schema.children[0], NANOARROW_TYPE_UINT64, "packet_id", error) ||
        !nt_set_scalar_child(schema.children[1], NANOARROW_TYPE_UINT64, disc_col, error)) {
        ArrowSchemaRelease(&schema);
        return false;
    }
    {
        ArrowSchema blob{};
        if (!nano_lance::build_blob_v2_payload_schema(blob, error)) {
            ArrowSchemaRelease(&schema);
            return false;
        }
        ArrowSchemaRelease(schema.children[2]);
        std::memcpy(schema.children[2], &blob, sizeof(ArrowSchema));
        blob.release = nullptr;
    }
    schema.flags = 0;

    ArrowArray batch{};
    if (ArrowArrayInitFromSchema(&batch, &schema, nullptr) != NANOARROW_OK ||
        ArrowArrayStartAppending(&batch) != NANOARROW_OK) {
        error = "remainder array init failed";
        ArrowSchemaRelease(&schema);
        return false;
    }
    ArrowArray* payload = batch.children[2];
    bool ok = true;
    for (const auto& r : rows) {
        ArrowStringView uri_view{r.uri.data(), static_cast<int64_t>(r.uri.size())};
        if (ArrowArrayAppendUInt(batch.children[0], r.packet_id) != NANOARROW_OK ||
            ArrowArrayAppendUInt(batch.children[1], r.discriminator) != NANOARROW_OK ||
            ArrowArrayAppendNull(payload->children[0], 1) != NANOARROW_OK ||
            ArrowArrayAppendString(payload->children[1], uri_view) != NANOARROW_OK ||
            ArrowArrayAppendUInt(payload->children[2], r.off) != NANOARROW_OK ||
            ArrowArrayAppendUInt(payload->children[3], r.size) != NANOARROW_OK ||
            ArrowArrayFinishElement(payload) != NANOARROW_OK || ArrowArrayFinishElement(&batch) != NANOARROW_OK) {
            error = "remainder row append failed";
            ok = false;
            break;
        }
    }
    if (ok && ArrowArrayFinishBuildingDefault(&batch, nullptr) != NANOARROW_OK) {
        error = "remainder array finalize failed";
        ok = false;
    }
    if (ok) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        NanoLanceWriter writer{};
        ok = nano_lance_writer_init(&writer, path.string().c_str(), 3) == NANO_LANCE_OK;
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
    }
    batch.release(&batch);
    schema.release(&schema);
    return ok;
}

}  // namespace staged
