#include "nano_lance_writer/blob_builder.hpp"
#include "nano_lance_writer/schema_mapper.hpp"

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

}  // namespace

int main() {
    std::string error;
    ArrowSchema schema;
    require(nano_lance::build_epb_table_schema(schema, error), error.c_str());
    require(schema.n_children == 2, "epb schema child count mismatch");

    nano_lance::LanceSchemaMapping mapping;
    require(nano_lance::map_arrow_schema(schema, mapping, error, true), error.c_str());
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
    require(nano_lance::build_epb_table_array(packet_ids, rows, batch, error), error.c_str());
    require(batch.length == 2, "epb batch length mismatch");

    ArrowArrayRelease(&batch);
    return 0;
}
