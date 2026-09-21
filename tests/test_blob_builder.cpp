// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/blob_builder.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& message) {
    require(ok, message.c_str());
}

}  // namespace

int main() {
    std::string error;
    ArrowSchema schema;
    require(nano_lance::build_epb_table_schema(schema, error), error);
    require(schema.n_children == 2, "epb schema child count mismatch");

    nano_lance::LanceSchemaMapping mapping;
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error);
    const nano_lance::LanceField* payload = nullptr;
    for (const auto& field : mapping.fields) {
        if (field.name == "payload_ref") {
            payload = &field;
        }
    }
    require(payload != nullptr, "payload_ref missing from mapping");
    require(payload->extension_name == nano_lance::kBlobV2ExtensionName, "payload_ref extension mismatch");
    require(nano_lance::lance_physical_fields(mapping).size() == 5, "epb physical field count mismatch");
    ArrowSchemaRelease(&schema);

    ArrowArray batch{};
    std::vector<std::uint64_t> packet_ids{1, 2};
    std::vector<nano_lance::BlobV2Row> rows;
    rows.push_back({std::nullopt, std::string("file:///tmp/capture_001.pcapng"), 100, 64});
    rows.push_back({std::vector<std::uint8_t>{'i', 'n', 'l', 'i', 'n', 'e'}, std::nullopt, 0, 0});
    require(nano_lance::build_epb_table_array(packet_ids, rows, batch, error), error);
    require(batch.length == 2, "epb batch length mismatch");

    ArrowArrayRelease(&batch);
    return 0;
}
