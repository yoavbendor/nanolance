// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/array_accessor.hpp"
#include "nanolance/blob_builder.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

const nano_lance::LanceField* find_field_by_name(const nano_lance::LanceSchemaMapping& mapping, const std::string& name) {
    for (const auto& field : mapping.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

void require_descriptor_matches(const nano_lance::BlobV2ExternalDescriptor& descriptor,
                              std::uint8_t kind,
                              std::uint64_t position,
                              std::uint64_t size,
                              const std::string& uri) {
    require(descriptor.kind == kind, "descriptor kind mismatch");
    require(descriptor.position == position, "descriptor position mismatch");
    require(descriptor.size == size, "descriptor size mismatch");
    require(descriptor.blob_id == 0U, "descriptor blob_id must be zero for external rows");
    require(descriptor.blob_uri == uri, "descriptor uri mismatch");
}

}  // namespace

int main() {
    std::string error;

    {
        nano_lance::BlobV2ExternalDescriptor source{};
        source.kind = 3;
        source.position = 200;
        source.size = 128;
        source.blob_id = 0;
        source.blob_uri = "s3://bucket/capture_001.pcapng";

        const auto packed = nano_lance::blob_v2_pack_descriptor_row(source);
        nano_lance::BlobV2ExternalDescriptor roundtrip{};
        require(nano_lance::blob_v2_unpack_descriptor_row(packed, roundtrip, error), error.c_str());
        require_descriptor_matches(roundtrip, 3, 200, 128, source.blob_uri);
    }

    ArrowSchema schema;
    require(nano_lance::build_epb_table_schema(schema, error), error.c_str());
    nano_lance::LanceSchemaMapping mapping;
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error.c_str());
    const auto* payload = find_field_by_name(mapping, "payload_ref");
    require(payload != nullptr, "payload_ref missing from mapping");

    std::vector<std::uint64_t> packet_ids{1, 2};
    std::vector<nano_lance::BlobV2Row> rows;
    rows.push_back({std::nullopt, std::string("file:///tmp/capture_001.pcapng"), 100, 64});
    rows.push_back({std::nullopt, std::string("s3://bucket/capture_001.pcapng"), 200, 128});

    ArrowArray batch{};
    require(nano_lance::build_epb_table_array(packet_ids, rows, batch, error), error.c_str());
    require(batch.length == 2, "batch length mismatch");

    const auto* struct_array = nano_lance::resolve_field_array(batch, mapping, *payload);
    require(struct_array != nullptr, "payload_ref struct array missing");

    const auto* data_f = find_field_by_name(mapping, "data");
    const auto* uri_f = find_field_by_name(mapping, "uri");
    const auto* pos_f = find_field_by_name(mapping, "position");
    const auto* size_f = find_field_by_name(mapping, "size");
    require(data_f != nullptr && uri_f != nullptr && pos_f != nullptr && size_f != nullptr,
            "write-side blob children missing from mapping");

    const auto* data_a = nano_lance::resolve_field_array(batch, mapping, *data_f);
    const auto* uri_a = nano_lance::resolve_field_array(batch, mapping, *uri_f);
    const auto* pos_a = nano_lance::resolve_field_array(batch, mapping, *pos_f);
    const auto* size_a = nano_lance::resolve_field_array(batch, mapping, *size_f);
    require(data_a != nullptr && uri_a != nullptr && pos_a != nullptr && size_a != nullptr,
            "write-side blob child arrays missing");

    {
        nano_lance::BlobV2ExternalDescriptor row0{};
        require(nano_lance::preprocess_blob_v2_external_row(*data_a, *uri_a, *pos_a, *size_a, 0, row0, error),
                error.c_str());
        require_descriptor_matches(row0, 3, 100, 64, "file:///tmp/capture_001.pcapng");

        nano_lance::BlobV2ExternalDescriptor row1{};
        require(nano_lance::preprocess_blob_v2_external_row(*data_a, *uri_a, *pos_a, *size_a, 1, row1, error),
                error.c_str());
        require_descriptor_matches(row1, 3, 200, 128, "s3://bucket/capture_001.pcapng");
    }

    {
        std::vector<nano_lance::BlobV2Row> inline_rows;
        inline_rows.push_back({std::vector<std::uint8_t>{'i', 'n', 'l', 'i', 'n', 'e'}, std::nullopt, 0, 0});
        ArrowArray inline_batch{};
        require(nano_lance::build_epb_table_array(std::vector<std::uint64_t>{1}, inline_rows, inline_batch, error),
                error.c_str());
        const auto* inline_data = nano_lance::resolve_field_array(inline_batch, mapping, *data_f);
        const auto* inline_uri = nano_lance::resolve_field_array(inline_batch, mapping, *uri_f);
        const auto* inline_pos = nano_lance::resolve_field_array(inline_batch, mapping, *pos_f);
        const auto* inline_size = nano_lance::resolve_field_array(inline_batch, mapping, *size_f);
        nano_lance::BlobV2ExternalDescriptor rejected{};
        require(!nano_lance::preprocess_blob_v2_external_row(*inline_data, *inline_uri, *inline_pos, *inline_size, 0,
                                                               rejected, error),
                "inline blob row must be rejected");
        require(error.find("external references") != std::string::npos, "inline rejection message mismatch");
        ArrowArrayRelease(&inline_batch);
    }

    {
        nano_lance::ColumnValues column_values;
        require(nano_lance::append_blob_v2_batch_column_values(batch, mapping, *payload, /*dictionary_mode=*/false,
                                                               column_values, error),
                error.c_str());
        require(column_values.kind == nano_lance::ColumnValues::Kind::BlobV2External, "column kind mismatch");
        require(column_values.blob_v2.row_packed_sizes.size() == 2U, "packed row count mismatch");
        require(column_values.blob_v2.packed_payload.size() > 0U, "packed payload empty");

        std::size_t offset = 0;
        for (std::size_t row = 0; row < column_values.blob_v2.row_packed_sizes.size(); ++row) {
            const auto row_size = column_values.blob_v2.row_packed_sizes[row];
            require(offset + row_size <= column_values.blob_v2.packed_payload.size(), "packed row bounds mismatch");
            const std::vector<std::uint8_t> row_bytes(
                column_values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(offset),
                column_values.blob_v2.packed_payload.begin() + static_cast<std::ptrdiff_t>(offset + row_size));
            nano_lance::BlobV2ExternalDescriptor unpacked{};
            require(nano_lance::blob_v2_unpack_descriptor_row(row_bytes, unpacked, error), error.c_str());
            if (row == 0) {
                require_descriptor_matches(unpacked, 3, 100, 64, "file:///tmp/capture_001.pcapng");
            } else {
                require_descriptor_matches(unpacked, 3, 200, 128, "s3://bucket/capture_001.pcapng");
            }
            offset += row_size;
        }

        const auto control = nano_lance::blob_v2_build_control_buffer(column_values.blob_v2.row_packed_sizes);
        require(!control.empty(), "control buffer must not be empty");
        std::vector<std::uint32_t> roundtrip_sizes;
        require(nano_lance::blob_v2_control_buffer_to_row_sizes(control, 2, roundtrip_sizes, error), error.c_str());
        require(roundtrip_sizes.size() == column_values.blob_v2.row_packed_sizes.size(), "control parse row count");
        for (std::size_t i = 0; i < roundtrip_sizes.size(); ++i) {
            require(roundtrip_sizes[i] == column_values.blob_v2.row_packed_sizes[i], "control parse row size mismatch");
        }

        {
            const std::vector<std::uint32_t> large_row_sizes{40000U, 30000U, 42U};
            const auto large_control = nano_lance::blob_v2_build_control_buffer(large_row_sizes);
            std::vector<std::uint32_t> large_roundtrip;
            require(nano_lance::blob_v2_control_buffer_to_row_sizes(large_control, large_row_sizes.size(),
                                                                    large_roundtrip, error),
                    error.c_str());
            require(large_roundtrip.size() == large_row_sizes.size(), "large control parse row count mismatch");
            for (std::size_t i = 0; i < large_roundtrip.size(); ++i) {
                require(large_roundtrip[i] == large_row_sizes[i], "large control parse row size mismatch");
            }
        }

        require(!nano_lance::blob_v2_page_layout_encoding().empty(), "page layout encoding must not be empty");
        require(nano_lance::blob_v2_column_page_encoding().size() >= 100U,
                "blob column page encoding should match Lance reference size");
        require(nano_lance::blob_v2_column_page_encoding().size() <
                    nano_lance::blob_v2_page_layout_encoding().size(),
                "column page encoding must strip outer PageLayout wrapper");
    }

    // Regression: control-buffer offset width must scale with the cumulative total. Previously wide
    // mode always wrote 16-bit cumulative offsets, so a total above 65535 wrapped and became
    // non-monotonic ("invalid wide cumulative offsets"). narrow(<256) / wide16(<=65535) / wide32 must
    // all round-trip exactly.
    {
        const auto check_roundtrip = [&](const std::vector<std::uint32_t>& sizes, const char* what) {
            const auto control = nano_lance::blob_v2_build_control_buffer(sizes);
            std::vector<std::uint32_t> back;
            std::string err;
            require(nano_lance::blob_v2_control_buffer_to_row_sizes(control, sizes.size(), back, err), err.c_str());
            require(back == sizes, what);
        };
        check_roundtrip({10, 200, 40}, "narrow control round-trip");          // total 250 (< 256)
        check_roundtrip({50000, 10000, 5000}, "wide16 control round-trip");   // total 65000 (<= 65535)
        check_roundtrip({60000, 60000, 60000}, "wide32 control round-trip");  // total 180000 (> 65535)
        check_roundtrip({4000000000U, 200000000U}, "wide32 large control round-trip");  // ~4.2e9 cumulative
    }

    {
        nano_lance::LanceSchemaMapping write_mapping = mapping;
        require(nano_lance::finalize_blob_v2_schema_for_write(write_mapping, error), error.c_str());
        const auto* finalized_parent = find_field_by_name(write_mapping, "payload_ref");
        require(finalized_parent != nullptr, "finalized payload_ref missing");
        require(finalized_parent->column_index == 1, "finalized blob column index mismatch");
        require(finalized_parent->metadata.at("lance-encoding:packed") == "true", "packed metadata missing");
        require(finalized_parent->metadata.at("lance-encoding:blob") == "true", "blob metadata missing");
        require(find_field_by_name(write_mapping, "data") == nullptr, "write-side data child must be removed");
        require(find_field_by_name(write_mapping, "uri") == nullptr, "write-side uri child must be removed");
        require(find_field_by_name(write_mapping, "kind") != nullptr, "materialized kind child missing");
        require(find_field_by_name(write_mapping, "blob_uri") != nullptr, "materialized blob_uri child missing");
        require(nano_lance::lance_physical_fields(write_mapping).size() == 2U,
                "finalized physical field count mismatch");
    }

    ArrowArrayRelease(&batch);
    ArrowSchemaRelease(&schema);
    return 0;
}
