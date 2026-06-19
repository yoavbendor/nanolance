// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// lldp2lance -> Lance roundtrip: run the DPAR engine with the user-written LldpPalette over a synthetic
// eth/0x88CC frame, write the resulting LLDP TLV table to a real Lance dataset via DparTableAppender, read
// it back with lance_table_read_dataset, and assert the rows survive — including the example's DECODED
// typed columns (ttl_seconds, caps_*, mgmt_*) AND the 32-byte fixed-size-binary value_head snapshot. The
// dpar2lance roundtrip covers a scalar-only someip_tlv table; this one adds the fixed-binary column path
// (value_head), proving the generic DPAR->Arrow->Lance sink carries a user Kind's array column unchanged.

#include "dpar_table_writer.hpp"

#include "lldp.hpp"  // the user-written LLDP parser + LldpPalette (vendored example)

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

namespace {

// Append one LLDP TLV: 2-byte header [type:7][length:9] (big-endian) then the value bytes.
void put_tlv(std::vector<std::uint8_t>& b, std::uint16_t type, const std::vector<std::uint8_t>& value) {
    const std::uint16_t hdr = static_cast<std::uint16_t>((type << 9) | (value.size() & 0x01FF));
    b.push_back(static_cast<std::uint8_t>(hdr >> 8));
    b.push_back(static_cast<std::uint8_t>(hdr & 0xFF));
    b.insert(b.end(), value.begin(), value.end());
}
std::vector<std::uint8_t> bytes_of(const char* s) {
    return std::vector<std::uint8_t>(s, s + std::char_traits<char>::length(s));
}

// A rich LLDPDU: Chassis ID (MAC), Port ID ("Gi0/1"), TTL=120, System Name ("switch01"), System
// Capabilities (Bridge|Router supported, Bridge enabled), Management Address (IPv4 192.168.1.1, ifIndex 3).
std::vector<std::uint8_t> build_lldpdu_rich() {
    std::vector<std::uint8_t> b;
    put_tlv(b, lldp_example::kLldpChassisId, {4, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55});  // subtype 4 (MAC)
    {
        std::vector<std::uint8_t> v = {5};  // subtype 5 (interface name)
        auto name = bytes_of("Gi0/1");
        v.insert(v.end(), name.begin(), name.end());
        put_tlv(b, lldp_example::kLldpPortId, v);
    }
    put_tlv(b, lldp_example::kLldpTtl, {0x00, 0x78});                  // 120 seconds
    put_tlv(b, lldp_example::kLldpSysName, bytes_of("switch01"));
    put_tlv(b, lldp_example::kLldpSysCaps, {0x00, 0x14, 0x00, 0x04});  // supported Bridge|Router, enabled Bridge
    put_tlv(b, lldp_example::kLldpMgmtAddr,
            {5, 1, 192, 168, 1, 1, 2, 0x00, 0x00, 0x00, 0x03, 0});     // IPv4 192.168.1.1, ifIndex 3
    put_tlv(b, lldp_example::kLldpEnd, {});                            // End of LLDPDU
    return b;
}

// Wrap an LLDPDU as the payload of an Ethernet frame (EtherType 0x88CC, 14-byte header).
std::vector<std::uint8_t> build_eth_lldp(const std::vector<std::uint8_t>& lldpdu) {
    std::vector<std::uint8_t> f(14, 0);
    const std::uint8_t dst[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};  // LLDP multicast
    for (int i = 0; i < 6; ++i) {
        f[i] = dst[i];
    }
    f[12] = static_cast<std::uint8_t>(lldp_example::kLldpEtherType >> 8);
    f[13] = static_cast<std::uint8_t>(lldp_example::kLldpEtherType & 0xFF);
    f.insert(f.end(), lldpdu.begin(), lldpdu.end());
    return f;
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

    // Run the user-defined LldpPalette over one eth/0x88CC frame -> 6 TLV rows.
    nanotins::dpar::dpar_engine<G, lldp_example::LldpPalette> engine;
    CHECK(engine.load_rules("eth.ethertype == 0x88CC => lldp eth_payload \"lldp\"\n").ok);
    const auto frame = build_eth_lldp(build_lldpdu_rich());
    engine.run(frame.data(), frame.size(), /*pid=*/7);

    const auto& rows = std::get<0>(engine.palette.tables);
    CHECK(rows.size() == 6);  // chassis, port, ttl, sysname, syscaps, mgmt (End is not emitted)

    // Write to a temp Lance dataset.
    const std::filesystem::path ds =
        std::filesystem::temp_directory_path() / "lldp2lance_roundtrip_lldp.lance";
    std::string err;
    CHECK(dpar_io::write_dpar_table<lldp_example::LldpTlvRow>(ds, rows, /*compress=*/false, err));
    CHECK(err.empty());

    // Read it back.
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    CHECK(nano_lance::lance_table_read_dataset(ds, schema, batches, err));
    CHECK(!batches.empty());

    std::int64_t total = 0;
    for (const ArrowArray& b : batches) {
        total += b.length;
    }
    CHECK(total == 6);

    // Single fragment, so batch 0 holds all rows.
    const ArrowArray& b0 = batches[0];
    CHECK(b0.length == 6);

    const std::uint64_t* packet_id = read_col<std::uint64_t>(b0, schema, "packet_id");
    const std::uint32_t* rule_id = read_col<std::uint32_t>(b0, schema, "rule_id");
    const std::uint16_t* tlv_type = read_col<std::uint16_t>(b0, schema, "tlv_type");
    const std::uint16_t* tlv_length = read_col<std::uint16_t>(b0, schema, "tlv_length");
    const std::uint8_t* subtype = read_col<std::uint8_t>(b0, schema, "subtype");
    const std::uint16_t* ttl_seconds = read_col<std::uint16_t>(b0, schema, "ttl_seconds");
    const std::uint16_t* caps_supported = read_col<std::uint16_t>(b0, schema, "caps_supported");
    const std::uint16_t* caps_enabled = read_col<std::uint16_t>(b0, schema, "caps_enabled");
    const std::uint8_t* mgmt_addr_subtype = read_col<std::uint8_t>(b0, schema, "mgmt_addr_subtype");
    const std::uint8_t* mgmt_iface_subtype = read_col<std::uint8_t>(b0, schema, "mgmt_iface_subtype");
    const std::uint32_t* mgmt_iface_number = read_col<std::uint32_t>(b0, schema, "mgmt_iface_number");

    // Every row carries the packet + rule discriminator.
    for (int i = 0; i < 6; ++i) {
        CHECK(packet_id[i] == 7);
        CHECK(rule_id[i] == 0);
    }

    // TLV types in order, and the chassis/port subtypes.
    CHECK(tlv_type[0] == lldp_example::kLldpChassisId && subtype[0] == 4);
    CHECK(tlv_type[1] == lldp_example::kLldpPortId && subtype[1] == 5);
    CHECK(tlv_type[2] == lldp_example::kLldpTtl);
    CHECK(tlv_type[3] == lldp_example::kLldpSysName && tlv_length[3] == 8);
    CHECK(tlv_type[4] == lldp_example::kLldpSysCaps);
    CHECK(tlv_type[5] == lldp_example::kLldpMgmtAddr);

    // The DECODED typed columns survive the trip (the part beyond a plain scalar passthrough).
    CHECK(ttl_seconds[2] == 120);
    CHECK(caps_supported[4] == 0x14 && caps_enabled[4] == 0x04);
    CHECK(mgmt_addr_subtype[5] == 1 && mgmt_iface_subtype[5] == 2 && mgmt_iface_number[5] == 3);

    // The fixed-size-binary value_head column (32 bytes/row) round-trips: row 3's value is "switch01".
    const int vh = column_index(schema, "value_head");
    CHECK(vh >= 0);
    const auto* vh_bytes = static_cast<const std::uint8_t*>(b0.children[vh]->buffers[1]);
    CHECK(vh_bytes != nullptr);
    const std::uint8_t* sysname = vh_bytes + 3 * 32;  // row 3, 32 bytes/element
    const char expect[] = "switch01";
    for (int i = 0; i < 8; ++i) {
        CHECK(sysname[i] == static_cast<std::uint8_t>(expect[i]));
    }
    // The Port ID value_head holds [subtype=5]['G''i''0''/''1'] (row 1).
    const std::uint8_t* portid = vh_bytes + 1 * 32;
    CHECK(portid[0] == 5 && portid[1] == 'G' && portid[2] == 'i' && portid[5] == '1');

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

    std::printf(
        "lldp2lance_roundtrip: ok (LldpPalette rule table -> Lance -> read-back: scalars, decoded "
        "ttl/caps/mgmt columns, fixed-binary value_head)\n");
    return 0;
}
