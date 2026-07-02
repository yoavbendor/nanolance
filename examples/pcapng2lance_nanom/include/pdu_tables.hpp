// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// Per-PDU Lance row types for the --decode-l2l3 path. One flat, NANOM_DESCRIBE'd struct per PDU
// (Ethernet / VLAN / IPv4 / IPv6 / TCP / UDP): `packet_id` (the join key back to the L1 packet table)
// followed by the decoded header fields. The nanom bit fields (ubits<>) and endian wire scalars (be<>)
// from nm_protocols.hpp are decoded on access during the walk, so here they land as ordinary
// host-order integer columns; MAC / IP addresses stay as fixed-size byte arrays -> Arrow fixed-binary
// ("w:6"/"w:4"/"w:16"). soa<T> + soa_lance_writer turn each of these into a Lance table with no
// per-type writer code (see soa_lance_writer.hpp).

#include "nm_protocols.hpp"  // nmproto::Ethernet/VlanTag/Ipv4/Ipv6/Tcp/Udp + walk_packet

#include <array>
#include <cstdint>

#include <nanom/nanom.hpp>

namespace p2l_nanom {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct EthRow {
    u64 packet_id;
    std::array<u8, 6> dst, src;
    u16 ethertype;
};
struct VlanRow {
    u64 packet_id;
    u8 pcp, dei;
    u16 vid;
    u16 inner_ethertype;
};
struct Ipv4Row {
    u64 packet_id;
    u8 version, ihl, dscp, ecn;
    u16 total_length, identification;
    u8 flags;
    u16 frag_offset;
    u8 ttl, protocol;
    u16 checksum;
    std::array<u8, 4> src, dst;
};
struct Ipv6Row {
    u64 packet_id;
    u8 version, traffic_class;
    u32 flow_label;
    u16 payload_length;
    u8 next_header, hop_limit;
    std::array<u8, 16> src, dst;
};
struct TcpRow {
    u64 packet_id;
    u16 src_port, dst_port;
    u32 seq, ack;
    u8 data_offset, reserved;
    u16 flags;
    u16 window, checksum, urgent_ptr;
};
struct UdpRow {
    u64 packet_id;
    u16 src_port, dst_port, length, checksum;
};

}  // namespace p2l_nanom

NANOM_DESCRIBE(p2l_nanom::EthRow, packet_id, dst, src, ethertype);
NANOM_DESCRIBE(p2l_nanom::VlanRow, packet_id, pcp, dei, vid, inner_ethertype);
NANOM_DESCRIBE(p2l_nanom::Ipv4Row, packet_id, version, ihl, dscp, ecn, total_length, identification, flags,
               frag_offset, ttl, protocol, checksum, src, dst);
NANOM_DESCRIBE(p2l_nanom::Ipv6Row, packet_id, version, traffic_class, flow_label, payload_length,
               next_header, hop_limit, src, dst);
NANOM_DESCRIBE(p2l_nanom::TcpRow, packet_id, src_port, dst_port, seq, ack, data_offset, reserved, flags,
               window, checksum, urgent_ptr);
NANOM_DESCRIBE(p2l_nanom::UdpRow, packet_id, src_port, dst_port, length, checksum);

namespace p2l_nanom {

// Converters: decoded nanom header value (from walk_packet) -> flat Lance row for `packet_id`.
inline EthRow make_eth(u64 pid, const nmproto::Ethernet& e) {
    return EthRow{pid, e.dst, e.src, u16(e.ethertype)};
}
inline VlanRow make_vlan(u64 pid, const nmproto::VlanTag& v) {
    return VlanRow{pid, u8(v.pcp), u8(v.dei), u16(v.vid), u16(v.inner_ethertype)};
}
inline Ipv4Row make_ipv4(u64 pid, const nmproto::Ipv4& p) {
    return Ipv4Row{pid,
                   u8(p.version),
                   u8(p.ihl),
                   u8(p.dscp),
                   u8(p.ecn),
                   u16(p.total_length),
                   u16(p.identification),
                   u8(p.flags),
                   u16(p.frag_offset),
                   p.ttl,
                   p.protocol,
                   u16(p.checksum),
                   p.src,
                   p.dst};
}
inline Ipv6Row make_ipv6(u64 pid, const nmproto::Ipv6& p) {
    return Ipv6Row{pid,   u8(p.version),         u8(p.traffic_class), u32(p.flow_label),
                   u16(p.payload_length), p.next_header,       p.hop_limit,   p.src,       p.dst};
}
inline TcpRow make_tcp(u64 pid, const nmproto::Tcp& t) {
    return TcpRow{pid,           u16(t.src_port),   u16(t.dst_port), u32(t.seq),
                  u32(t.ack),    u8(t.data_offset), u8(t.reserved),  u16(t.flags),
                  u16(t.window), u16(t.checksum),   u16(t.urgent)};
}
inline UdpRow make_udp(u64 pid, const nmproto::Udp& u) {
    return UdpRow{pid, u16(u.src_port), u16(u.dst_port), u16(u.length), u16(u.checksum)};
}

}  // namespace p2l_nanom
