#pragma once

// Layered L2/L3 decode (the CPU reference; the same overlay+store shape a CUDA kernel would run). Given
// a packet's bytes and its link type, walk Ethernet -> VLAN* -> IPv4/IPv6 -> TCP/UDP, accumulating each
// decoded header into a per-PDU-type list keyed by the owning packet's row id. Each list becomes its own
// Lance table (one row per PDU instance). Adding a protocol = a struct in protocols.hpp + a branch here.

#include "nanotins/protocols.hpp"

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
        // A non-zero fragment offset means the remainder is fragment data, not an L4 header — report no
        // next layer (discriminator 0) so the L4 stage decodes nothing (matches walk_packet's L4 gate).
        const bool first_fragment = (ip.flags_frag.word_host() & 0x1FFFU) == 0;
        next_ip_proto = first_fragment ? ip.protocol : static_cast<std::uint8_t>(0);
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

// ---- The shared walk: one Ethernet -> VLAN* -> IPv4/IPv6 -> TCP/UDP traversal, visited by callbacks. ----
// Replays the exact same sequence as the serial decode_l2/l3/l4 chain, invoking on_<pdu>(view) for each
// emitted header in order. It is device-safe (noexcept, no allocation, no STL/globals in the body, only
// overlay() reads) and is the SINGLE source of truth for both the serial path (decode_packet) and the
// bulk count/scatter passes — so the count pass and the scatter pass can never disagree (the count is
// exactly the number of on_* calls, which is exactly the number of rows scattered).
template <class FEth, class FVlan, class FIpv4, class FIpv6, class FTcp, class FUdp>
inline void walk_packet(std::uint32_t link_type, Bytes pkt, FEth on_eth, FVlan on_vlan, FIpv4 on_ipv4,
                        FIpv6 on_ipv6, FTcp on_tcp, FUdp on_udp) noexcept {
    if (link_type != kLinkTypeEthernet) {
        return;  // non-Ethernet link types decode to nothing in step 1
    }
    Ethernet eth{};
    if (!overlay(pkt, 0, eth)) {
        return;
    }
    on_eth(eth);
    std::uint16_t ethertype = eth.ethertype.host();
    std::size_t off = sizeof(Ethernet);
    while (ethertype == kEtherTypeVlan || ethertype == kEtherTypeQinQ) {
        VlanTag tag{};
        if (!overlay(pkt, off, tag)) {
            return;
        }
        on_vlan(tag);
        ethertype = tag.inner_ethertype.host();
        off += sizeof(VlanTag);
    }
    if (off > pkt.size()) {
        return;
    }
    Bytes after_l2 = pkt.subspan(off);
    std::uint8_t ip_proto = 0;
    std::size_t l3 = 0;
    bool has_l4 = true;  // a non-zero IPv4 fragment offset means this packet carries fragment data, not an
                         // L4 header — decoding L4 there would overlay garbage (matches tshark, which shows
                         // udp/tcp only on the first fragment). IPv6 frag ext-headers are out of scope here.
    if (ethertype == kEtherTypeIpv4) {
        Ipv4 ip{};
        if (!overlay(after_l2, 0, ip)) {
            return;
        }
        on_ipv4(ip);
        const std::size_t hdr = static_cast<std::size_t>(ip.ver_ihl.word_host() & 0x0FU) * 4U;
        l3 = hdr >= sizeof(Ipv4) ? hdr : sizeof(Ipv4);  // skip IPv4 options
        ip_proto = ip.protocol;
        has_l4 = (ip.flags_frag.word_host() & 0x1FFFU) == 0;  // frag_offset == 0 (first/only fragment)
    } else if (ethertype == kEtherTypeIpv6) {
        Ipv6 ip{};
        if (!overlay(after_l2, 0, ip)) {
            return;
        }
        on_ipv6(ip);
        l3 = sizeof(Ipv6);  // step 1 ignores IPv6 extension headers
        ip_proto = ip.next_header;
    } else {
        return;
    }
    if (l3 > after_l2.size() || !has_l4) {
        return;
    }
    Bytes after_l3 = after_l2.subspan(l3);
    if (ip_proto == kIpProtoTcp) {
        Tcp tcp{};
        if (!overlay(after_l3, 0, tcp)) {
            return;
        }
        on_tcp(tcp);
    } else if (ip_proto == kIpProtoUdp) {
        Udp udp{};
        if (!overlay(after_l3, 0, udp)) {
            return;
        }
        on_udp(udp);
    }
}

// One-shot decode of all layers from a packet's first byte (the --decode-l2l3 path). Stops at the first
// layer that doesn't fit/parse; layers already added are kept. Implemented on the shared walk.
inline void decode_packet(std::uint64_t packet_id, std::uint32_t link_type, Bytes pkt, DecodedPdus& out) {
    walk_packet(
        link_type, pkt, [&](const Ethernet& v) { out.ethernet.add(packet_id, v); },
        [&](const VlanTag& v) { out.vlan.add(packet_id, v); }, [&](const Ipv4& v) { out.ipv4.add(packet_id, v); },
        [&](const Ipv6& v) { out.ipv6.add(packet_id, v); }, [&](const Tcp& v) { out.tcp.add(packet_id, v); },
        [&](const Udp& v) { out.udp.add(packet_id, v); });
}

// ---- Bulk (device-safe) decode primitives: count then scatter, both over walk_packet. ----

// Per-packet PDU counts (count pass output) and, after an exclusive prefix-sum, per-packet write bases.
struct PduCounts {
    std::uint32_t eth = 0;
    std::uint32_t vlan = 0;
    std::uint32_t ipv4 = 0;
    std::uint32_t ipv6 = 0;
    std::uint32_t tcp = 0;
    std::uint32_t udp = 0;
};

// Count how many PDUs of each type a packet emits — no output writes, no allocation (device-safe).
inline PduCounts count_packet(std::uint32_t link_type, Bytes pkt) noexcept {
    PduCounts c{};
    walk_packet(
        link_type, pkt, [&](const Ethernet&) { ++c.eth; }, [&](const VlanTag&) { ++c.vlan; },
        [&](const Ipv4&) { ++c.ipv4; }, [&](const Ipv6&) { ++c.ipv6; }, [&](const Tcp&) { ++c.tcp; },
        [&](const Udp&) { ++c.udp; });
    return c;
}

// Raw column pointers (parallel packet_id + row arrays) for the scatter pass — the SoA a CUDA kernel writes.
struct PduSink {
    std::uint64_t* eth_pid;
    Ethernet* eth_rows;
    std::uint64_t* vlan_pid;
    VlanTag* vlan_rows;
    std::uint64_t* ipv4_pid;
    Ipv4* ipv4_rows;
    std::uint64_t* ipv6_pid;
    Ipv6* ipv6_rows;
    std::uint64_t* tcp_pid;
    Tcp* tcp_rows;
    std::uint64_t* udp_pid;
    Udp* udp_rows;
};

// Re-walk a packet and write each emitted PDU to its prefix-summed slot. `base` = the index in each
// column where this packet's first PDU of that type goes (from the exclusive scan). Device-safe: every
// write index is derived solely from this packet's `base` + its own emit order, so no two packets touch
// the same slot — exactly the data-parallel write pattern a GPU thread runs.
inline void scatter_packet(std::uint64_t packet_id, std::uint32_t link_type, Bytes pkt, const PduCounts& base,
                           const PduSink& sink) noexcept {
    std::uint32_t e = base.eth, v = base.vlan, i4 = base.ipv4, i6 = base.ipv6, t = base.tcp, u = base.udp;
    walk_packet(
        link_type, pkt,
        [&](const Ethernet& x) {
            sink.eth_pid[e] = packet_id;
            sink.eth_rows[e] = x;
            ++e;
        },
        [&](const VlanTag& x) {
            sink.vlan_pid[v] = packet_id;
            sink.vlan_rows[v] = x;
            ++v;
        },
        [&](const Ipv4& x) {
            sink.ipv4_pid[i4] = packet_id;
            sink.ipv4_rows[i4] = x;
            ++i4;
        },
        [&](const Ipv6& x) {
            sink.ipv6_pid[i6] = packet_id;
            sink.ipv6_rows[i6] = x;
            ++i6;
        },
        [&](const Tcp& x) {
            sink.tcp_pid[t] = packet_id;
            sink.tcp_rows[t] = x;
            ++t;
        },
        [&](const Udp& x) {
            sink.udp_pid[u] = packet_id;
            sink.udp_rows[u] = x;
            ++u;
        });
}

}  // namespace protocols
