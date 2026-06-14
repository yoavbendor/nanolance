// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ArrowArray;
struct ArrowSchema;

namespace nano_lance {

constexpr const char* kBlobV2ExtensionName = "lance.blob.v2";

struct BlobV2Row {
    std::optional<std::vector<std::uint8_t>> inline_data;
    std::optional<std::string> uri;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
};

/// Build Arrow schema for `payload_ref` write-side blob v2 storage struct.
bool build_blob_v2_payload_schema(ArrowSchema& schema, std::string& error);

/// Build a single-column record-batch schema: `packet_id` + `payload_ref` (extension struct).
bool build_epb_table_schema(ArrowSchema& schema, std::string& error);

/// Build an Arrow struct array for `payload_ref` from rows (write-side layout).
bool build_blob_v2_payload_array(const std::vector<BlobV2Row>& rows, ArrowArray& array, std::string& error);

/// Build a record-batch Arrow array matching `build_epb_table_schema`.
bool build_epb_table_array(const std::vector<std::uint64_t>& packet_ids,
                           const std::vector<BlobV2Row>& payload_rows,
                           ArrowArray& array,
                           std::string& error);

}  // namespace nano_lance
