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

// Decode one packet's headers into `out`. Pure function of (link_type, pkt bytes); silently stops at the
// first header that doesn't fit (truncated capture) — partial layers already added are kept.
inline void decode_packet(std::uint64_t packet_id, std::uint32_t link_type, Bytes pkt, DecodedPdus& out) {
    if (link_type != kLinkTypeEthernet) {
        return;  // step 1 only decodes Ethernet link layer; other link types pass through as payload-only
    }
    Ethernet eth{};
    if (!overlay(pkt, 0, eth)) {
        return;
    }
    out.ethernet.add(packet_id, eth);
    std::uint16_t ethertype = eth.ethertype.host();
    std::size_t off = sizeof(Ethernet);

    // VLAN / QinQ stack: each tag is 4 bytes carrying the next ethertype.
    while (ethertype == kEtherTypeVlan || ethertype == kEtherTypeQinQ) {
        VlanTag tag{};
        if (!overlay(pkt, off, tag)) {
            return;
        }
        out.vlan.add(packet_id, tag);
        ethertype = tag.inner_ethertype.host();
        off += sizeof(VlanTag);
    }

    std::uint8_t ip_proto = 0;
    std::size_t l4_off = 0;
    if (ethertype == kEtherTypeIpv4) {
        Ipv4 ip{};
        if (!overlay(pkt, off, ip)) {
            return;
        }
        out.ipv4.add(packet_id, ip);
        const std::size_t ihl_words = static_cast<std::size_t>(ip.ver_ihl.word_host() & 0x0FU);
        const std::size_t hdr_len = ihl_words * 4U;
        ip_proto = ip.protocol;
        l4_off = off + (hdr_len >= sizeof(Ipv4) ? hdr_len : sizeof(Ipv4));  // skip IPv4 options
    } else if (ethertype == kEtherTypeIpv6) {
        Ipv6 ip{};
        if (!overlay(pkt, off, ip)) {
            return;
        }
        out.ipv6.add(packet_id, ip);
        ip_proto = ip.next_header;  // step 1 ignores IPv6 extension headers
        l4_off = off + sizeof(Ipv6);
    } else {
        return;
    }

    if (ip_proto == kIpProtoTcp) {
        Tcp tcp{};
        if (overlay(pkt, l4_off, tcp)) {
            out.tcp.add(packet_id, tcp);
        }
    } else if (ip_proto == kIpProtoUdp) {
        Udp udp{};
        if (overlay(pkt, l4_off, udp)) {
            out.udp.add(packet_id, udp);
            // Hook for UDP-internal PDUs: dispatch on udp.dst_port.host() to a registry of inner
            // parsers (e.g. tunnelled / app protocols). Left as an extension point in step 1.
        }
    }
}

}  // namespace protocols
