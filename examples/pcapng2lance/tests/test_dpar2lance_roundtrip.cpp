// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// DPAR -> Lance roundtrip: run the DPAR engine to fill a someip_tlv rule table, write it to a real Lance
// dataset via DparTableAppender (the nano_lance_writer C API), read it back with lance_table_read_dataset,
// and assert the columns (packet_id, rule_id discriminator, data_id, wire_type, length) survive the trip.
// Exercises nanotins DPAR reflection -> Arrow -> Lance write -> Lance read end to end.

#include "dpar_table_writer.hpp"

#include "nanotins/dpar_palette.hpp"

#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace dpar = nanotins::dpar;

namespace {

void put8(std::vector<std::uint8_t>& b, std::size_t off, std::uint8_t v) { b[off] = v; }
void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v);
}
void put_tag(std::vector<std::uint8_t>& b, std::uint8_t wt, std::uint16_t id) {
    b.push_back(static_cast<std::uint8_t>((wt << 4) | ((id >> 8) & 0x0F)));
    b.push_back(static_cast<std::uint8_t>(id & 0xFF));
}
std::vector<std::uint8_t> build_udp(std::uint16_t src, std::uint16_t dst,
                                    const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> b(42, 0);
    put16(b, 12, 0x0800);
    put8(b, 14, 0x45);
    put8(b, 23, 17);
    put16(b, 34, src);
    put16(b, 36, dst);
    put16(b, 38, static_cast<std::uint16_t>(8 + payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

// Find the column index whose schema name matches `name` (the reader preserves struct field names).
int column_index(const ArrowSchema& schema, const char* name) {
    for (int i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name && std::string(schema.children[i]->name) == name) {
            return i;
        }
    }
    return -1;
}

template <class T>
const T* read_col(const ArrowArray& batch, const ArrowSchema& schema, const char* name) {
    const int i = column_index(schema, name);
    CHECK(i >= 0);
    return static_cast<const T*>(batch.children[i]->buffers[1]);
}

}  // namespace

int main() {
    using G = nanotins::L2L3Graph;

    // Fill a someip_tlv table: two rules over a 2-member payload -> 4 rows (rule_id 0,0,1,1).
    dpar::dpar_engine<G, dpar::DefaultPalette> engine;
    CHECK(engine
              .load_rules(
                  "udp.dst_port == 9999 => someip_tlv udp_payload A\n"
                  "udp.dst_port == 9999 => someip_tlv udp_payload B\n")
              .ok);
    std::vector<std::uint8_t> payload;
    put_tag(payload, 0, 0x011);
    payload.push_back(0x42);
    put_tag(payload, 5, 0x022);
    payload.push_back(0x01);
    payload.push_back(0x99);
    auto pkt = build_udp(0, 9999, payload);
    engine.run(pkt.data(), pkt.size(), /*pid=*/5);

    const auto& rows = std::get<0>(engine.palette.tables);
    CHECK(rows.size() == 4);

    // Write to a temp Lance dataset.
    const std::filesystem::path ds =
        std::filesystem::temp_directory_path() / "dpar2lance_roundtrip_someip_tlv.lance";
    std::string err;
    CHECK(dpar_io::write_dpar_table<dpar::SomeipTlvRow>(ds, rows, /*compress=*/false, err));
    CHECK(err.empty());

    // Read it back.
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    CHECK(nano_lance::lance_table_read_dataset(ds, schema, batches, err));
    CHECK(!batches.empty());

    // Total rows across batches == 4, and the columns round-trip in order.
    std::int64_t total = 0;
    for (const ArrowArray& b : batches) {
        total += b.length;
    }
    CHECK(total == 4);

    // Single fragment here, so batch 0 holds all rows.
    const ArrowArray& b0 = batches[0];
    CHECK(b0.length == 4);
    const std::uint64_t* packet_id = read_col<std::uint64_t>(b0, schema, "packet_id");
    const std::uint32_t* rule_id = read_col<std::uint32_t>(b0, schema, "rule_id");
    const std::uint16_t* data_id = read_col<std::uint16_t>(b0, schema, "data_id");
    const std::uint8_t* wire_type = read_col<std::uint8_t>(b0, schema, "wire_type");
    const std::uint32_t* length = read_col<std::uint32_t>(b0, schema, "length");

    for (int i = 0; i < 4; ++i) {
        CHECK(packet_id[i] == 5);
    }
    CHECK(rule_id[0] == 0 && rule_id[1] == 0 && rule_id[2] == 1 && rule_id[3] == 1);  // discriminator
    CHECK(data_id[0] == 0x011 && data_id[1] == 0x022 && data_id[2] == 0x011 && data_id[3] == 0x022);
    CHECK(wire_type[0] == 0 && wire_type[1] == 5);
    CHECK(length[0] == 1 && length[1] == 1);

    for (ArrowArray& b : batches) {
        if (b.release) {
            b.release(&b);
        }
    }
    if (schema.release) {
        schema.release(&schema);
    }
    std::error_code ec;
    std::filesystem::remove_all(ds, ec);

    std::printf("dpar2lance_roundtrip: ok (someip_tlv rule table -> Lance -> read-back, columns + rule_id)\n");
    return 0;
}
