#pragma once

// Layered L2/L3 decode (the CPU reference; the same overlay+store shape a CUDA kernel would run). Given
// a packet's bytes and its link type, walk Ethernet -> VLAN* -> IPv4/IPv6 -> TCP/UDP, accumulating each
// decoded header into a per-PDU-type list keyed by the owning packet's row id. Each list becomes its own
// Lance table (one row per PDU instance). Adding a protocol = a struct in protocols.hpp + a branch here.

#include "protocols.hpp"

#include <cstdint>
#include <vector>

namespace protocols {

// One growable list per PDU type, each paired with the packet row id that produced the header.
template <class T>
struct PduColumn {
    std::vector<std::uint64_t> packet_id;
    std::vector<T> rows;
    void add(std::uint64_t pkt, const T& v) {
        packet_id.push_back(pkt);
        rows.push_back(v);
    }
    std::size_t size() const { return rows.size(); }
};

struct DecodedPdus {
    PduColumn<Ethernet> ethernet;
    PduColumn<VlanTag> vlan;
    PduColumn<Ipv4> ipv4;
    PduColumn<Ipv6> ipv6;
    PduColumn<Tcp> tcp;
    PduColumn<Udp> udp;
};

// Per-layer decode steps. Each consumes a span that STARTS at the layer's first byte (so they compose
// for staged parsing where the previous stage advanced past its header), appends the decoded header(s)
// to `out`, and reports bytes consumed + the discriminator for the next layer. Return false if the
// header doesn't fit or the layer isn't understood.

// L2 (Ethernet + 802.1Q/QinQ VLAN stack). `consumed` = total L2 length; `next_ethertype` selects L3.
inline bool decode_l2(std::uint64_t packet_id, std::uint32_t link_type, Bytes bytes, DecodedPdus& out,
                      std::size_t& consumed, std::uint16_t& next_ethertype) {
    consumed = 0;
    next_ethertype = 0;
    if (link_type != kLinkTypeEthernet) {
        return false;  // non-Ethernet link types decode to nothing in step 1
    }
    Ethernet eth{};
    if (!overlay(bytes, 0, eth)) {
        return false;
    }
    out.ethernet.add(packet_id, eth);
    std::uint16_t ethertype = eth.ethertype.host();
    std::size_t off = sizeof(Ethernet);
    while (ethertype == kEtherTypeVlan || ethertype == kEtherTypeQinQ) {
        VlanTag tag{};
        if (!overlay(bytes, off, tag)) {
            return false;
        }
        out.vlan.add(packet_id, tag);
        ethertype = tag.inner_ethertype.host();
        off += sizeof(VlanTag);
    }
    consumed = off;
    next_ethertype = ethertype;
    return true;
}

// L3 (IPv4 / IPv6). `consumed` = network-header length; `next_ip_proto` selects L4.
inline bool decode_l3(std::uint64_t packet_id, std::uint16_t ethertype, Bytes bytes, DecodedPdus& out,
                      std::size_t& consumed, std::uint8_t& next_ip_proto) {
    consumed = 0;
    next_ip_proto = 0;
    if (ethertype == kEtherTypeIpv4) {
        Ipv4 ip{};
        if (!overlay(bytes, 0, ip)) {
            return false;
        }
        out.ipv4.add(packet_id, ip);
        const std::size_t hdr = static_cast<std::size_t>(ip.ver_ihl.word_host() & 0x0FU) * 4U;
        consumed = hdr >= sizeof(Ipv4) ? hdr : sizeof(Ipv4);  // skip IPv4 options
        next_ip_proto = ip.protocol;
        return true;
    }
    if (ethertype == kEtherTypeIpv6) {
        Ipv6 ip{};
        if (!overlay(bytes, 0, ip)) {
            return false;
        }
        out.ipv6.add(packet_id, ip);
        consumed = sizeof(Ipv6);   // step 1 ignores IPv6 extension headers
        next_ip_proto = ip.next_header;
        return true;
    }
    return false;
}

// L4 (TCP / UDP). `consumed` = transport-header length; the rest is the application payload.
inline bool decode_l4(std::uint64_t packet_id, std::uint8_t ip_proto, Bytes bytes, DecodedPdus& out,
                      std::size_t& consumed) {
    consumed = 0;
    if (ip_proto == kIpProtoTcp) {
        Tcp tcp{};
        if (!overlay(bytes, 0, tcp)) {
            return false;
        }
        out.tcp.add(packet_id, tcp);
        const std::size_t hdr = static_cast<std::size_t>((tcp.off_flags.word_host() >> 12) & 0x0FU) * 4U;
        consumed = hdr >= sizeof(Tcp) ? hdr : sizeof(Tcp);
        return true;
    }
    if (ip_proto == kIpProtoUdp) {
        Udp udp{};
        if (!overlay(bytes, 0, udp)) {
            return false;
        }
        out.udp.add(packet_id, udp);
        consumed = sizeof(Udp);
        // Hook for UDP-internal PDUs: dispatch on udp.dst_port.host() to a registry of inner parsers.
        return true;
    }
    return false;
}

// One-shot decode of all layers from a packet's first byte (the --decode-l2l3 path). Stops at the first
// layer that doesn't fit/parse; layers already added are kept.
inline void decode_packet(std::uint64_t packet_id, std::uint32_t link_type, Bytes pkt, DecodedPdus& out) {
    std::size_t l2 = 0;
    std::uint16_t ethertype = 0;
    if (!decode_l2(packet_id, link_type, pkt, out, l2, ethertype) || l2 > pkt.size()) {
        return;
    }
    Bytes after_l2 = pkt.subspan(l2);
    std::size_t l3 = 0;
    std::uint8_t ip_proto = 0;
    if (!decode_l3(packet_id, ethertype, after_l2, out, l3, ip_proto) || l3 > after_l2.size()) {
        return;
    }
    std::size_t l4 = 0;
    decode_l4(packet_id, ip_proto, after_l2.subspan(l3), out, l4);
}

}  // namespace protocols
