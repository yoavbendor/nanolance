#pragma once

// The L2/L3/L4 backbone as struct_spec wire specs — the single home for the Ethernet / VLAN / IPv4 / IPv6 /
// TCP / UDP layouts, replacing the be<>/bits<> packed structs in protocols.hpp for the spec-driven decode.
// Field names + types match the described protocols:: structs exactly (verified by test_protocol_specs vs
// the be<>/bits<> overlay), so the per-PDU Lance tables a struct_spec DAG emits are byte-identical to the
// current walk_packet tables. One spec drives host read + device read + SoA + Arrow (see struct_spec.hpp).

#include "nanotins/struct_spec.hpp"

#include <cstdint>

namespace nanotins {

using namespace literals;

// ---- Ethernet II (14 bytes) ----
using EthernetSpec = StructSpec<
    named_bytes_field<decltype("dst"_fld), 0, 6>,
    named_bytes_field<decltype("src"_fld), 6, 6>,
    named_field<decltype("ethertype"_fld), 12, std::uint16_t, wire_endian::big>>;

// ---- 802.1Q VLAN tag (4 bytes: TCI + inner ethertype) ----
using VlanTagSpec = StructSpec<
    named_bit_field<decltype("pcp"_fld), 0, std::uint16_t, 0, 3, wire_endian::big>,
    named_bit_field<decltype("dei"_fld), 0, std::uint16_t, 3, 1, wire_endian::big>,
    named_bit_field<decltype("vid"_fld), 0, std::uint16_t, 4, 12, wire_endian::big>,
    named_field<decltype("inner_ethertype"_fld), 2, std::uint16_t, wire_endian::big>>;

// ---- IPv4 (20-byte fixed header; ihl gives the true length) ----
using Ipv4Spec = StructSpec<
    named_bit_field<decltype("version"_fld), 0, std::uint8_t, 0, 4, wire_endian::big>,
    named_bit_field<decltype("ihl"_fld), 0, std::uint8_t, 4, 4, wire_endian::big>,
    named_bit_field<decltype("dscp"_fld), 1, std::uint8_t, 0, 6, wire_endian::big>,
    named_bit_field<decltype("ecn"_fld), 1, std::uint8_t, 6, 2, wire_endian::big>,
    named_field<decltype("total_length"_fld), 2, std::uint16_t, wire_endian::big>,
    named_field<decltype("identification"_fld), 4, std::uint16_t, wire_endian::big>,
    named_bit_field<decltype("flags"_fld), 6, std::uint16_t, 0, 3, wire_endian::big>,
    named_bit_field<decltype("frag_offset"_fld), 6, std::uint16_t, 3, 13, wire_endian::big>,
    named_field<decltype("ttl"_fld), 8, std::uint8_t, wire_endian::big>,
    named_field<decltype("protocol"_fld), 9, std::uint8_t, wire_endian::big>,
    named_field<decltype("checksum"_fld), 10, std::uint16_t, wire_endian::big>,
    named_bytes_field<decltype("src"_fld), 12, 4>,
    named_bytes_field<decltype("dst"_fld), 16, 4>>;

// ---- IPv6 (40-byte fixed header) ----
using Ipv6Spec = StructSpec<
    named_bit_field<decltype("version"_fld), 0, std::uint32_t, 0, 4, wire_endian::big>,
    named_bit_field<decltype("traffic_class"_fld), 0, std::uint32_t, 4, 8, wire_endian::big>,
    named_bit_field<decltype("flow_label"_fld), 0, std::uint32_t, 12, 20, wire_endian::big>,
    named_field<decltype("payload_length"_fld), 4, std::uint16_t, wire_endian::big>,
    named_field<decltype("next_header"_fld), 6, std::uint8_t, wire_endian::big>,
    named_field<decltype("hop_limit"_fld), 7, std::uint8_t, wire_endian::big>,
    named_bytes_field<decltype("src"_fld), 8, 16>,
    named_bytes_field<decltype("dst"_fld), 24, 16>>;

// ---- TCP (20-byte fixed header) ----
using TcpSpec = StructSpec<
    named_field<decltype("src_port"_fld), 0, std::uint16_t, wire_endian::big>,
    named_field<decltype("dst_port"_fld), 2, std::uint16_t, wire_endian::big>,
    named_field<decltype("seq"_fld), 4, std::uint32_t, wire_endian::big>,
    named_field<decltype("ack"_fld), 8, std::uint32_t, wire_endian::big>,
    named_bit_field<decltype("data_offset"_fld), 12, std::uint16_t, 0, 4, wire_endian::big>,
    named_bit_field<decltype("reserved"_fld), 12, std::uint16_t, 4, 3, wire_endian::big>,
    named_bit_field<decltype("flags"_fld), 12, std::uint16_t, 7, 9, wire_endian::big>,
    named_field<decltype("window"_fld), 14, std::uint16_t, wire_endian::big>,
    named_field<decltype("checksum"_fld), 16, std::uint16_t, wire_endian::big>,
    named_field<decltype("urgent_ptr"_fld), 18, std::uint16_t, wire_endian::big>>;

// ---- UDP (8 bytes) ----
using UdpSpec = StructSpec<
    named_field<decltype("src_port"_fld), 0, std::uint16_t, wire_endian::big>,
    named_field<decltype("dst_port"_fld), 2, std::uint16_t, wire_endian::big>,
    named_field<decltype("length"_fld), 4, std::uint16_t, wire_endian::big>,
    named_field<decltype("checksum"_fld), 6, std::uint16_t, wire_endian::big>>;

}  // namespace nanotins
