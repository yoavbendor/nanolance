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

// IPv6 extension-header rows (--decode-l2l3 over SRv6 / ext-header traffic). Schemas match the nanotins
// converter's ipv6_hopbyhop / ipv6_destopt / ipv6_routing / ipv6_srh_segment / ipv6_option tables.
struct Ipv6ExtOptRow {  // one shape for both Hop-by-Hop and Destination Options (routed to their tables)
    u64 packet_id;
    u8 next_header, hdr_ext_len;
};
struct Ipv6RoutingRow {  // Routing header / SRv6 SRH fixed part
    u64 packet_id;
    u8 next_header, hdr_ext_len, routing_type, segments_left, last_entry, flags;
    u16 tag;
};
struct Ipv6SrhSegmentRow {  // one row per SRv6 segment (address -> Arrow fixed_size_binary(16))
    u64 packet_id;
    u8 srh_order, segment_index;
    std::array<u8, 16> address;
};
struct Ipv6OptionRow {  // one row per IPv6/SRH TLV option
    u64 packet_id;
    u8 container_type, opt_type, opt_len;
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
NANOM_DESCRIBE(p2l_nanom::Ipv6ExtOptRow, packet_id, next_header, hdr_ext_len);
NANOM_DESCRIBE(p2l_nanom::Ipv6RoutingRow, packet_id, next_header, hdr_ext_len, routing_type, segments_left,
               last_entry, flags, tag);
NANOM_DESCRIBE(p2l_nanom::Ipv6SrhSegmentRow, packet_id, srh_order, segment_index, address);
NANOM_DESCRIBE(p2l_nanom::Ipv6OptionRow, packet_id, container_type, opt_type, opt_len);

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
inline Ipv6ExtOptRow make_ext_opt(u64 pid, const nmproto::Ipv6ExtOpt& h) {
    return Ipv6ExtOptRow{pid, h.next_header, h.hdr_ext_len};
}
inline Ipv6RoutingRow make_ipv6_routing(u64 pid, const nmproto::Ipv6Srh& s) {
    return Ipv6RoutingRow{pid,           s.next_header, s.hdr_ext_len, s.routing_type,
                          s.segments_left, s.last_entry,   s.flags,       u16(s.tag)};
}

}  // namespace p2l_nanom
