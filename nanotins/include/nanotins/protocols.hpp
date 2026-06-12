#pragma once

// L2/L3 protocol headers as packed, standard-layout wire structs (the M3 payoff of the nanotins
// reflection core). Each is overlay-safe on packet bytes: be<>/bits<> store raw wire bytes (1-aligned,
// no padding), endianness and bit layout are declared on the field types, and one BOOST_DESCRIBE_STRUCT
// line gives each struct a SoA, an Arrow schema, and a Lance table for free.
//
// Bitfields straddling bytes (IPv4 flags/frag over a BE u16, IPv6 version/tc/flow over a BE u32) are
// handled by byteswapping the whole word first, then shift+mask — see nanotins/bits.hpp.

#include "soatins/bits.hpp"
#include "soatins/endian.hpp"
#include "soatins/fixed_string.hpp"

#include <boost/describe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace protocols {

using soatins::be;
using soatins::bits;
using soatins::field;
// POD byte span (ptr + size) that stays device-safe under clang-cuda/libstdc++ (std::span in this toolchain
// pulls host-only assertion hooks in device code paths).
struct Bytes {
    const std::uint8_t* ptr = nullptr;
    std::size_t len = 0;

    constexpr Bytes() = default;
    constexpr Bytes(const std::uint8_t* data, std::size_t size) : ptr(data), len(size) {}
    constexpr Bytes(std::span<const std::uint8_t> s) : ptr(s.data()), len(s.size()) {}

    constexpr const std::uint8_t* data() const { return ptr; }
    constexpr std::size_t size() const { return len; }
    constexpr bool empty() const { return len == 0; }
    constexpr const std::uint8_t& operator[](std::size_t i) const { return ptr[i]; }

    constexpr Bytes subspan(std::size_t off) const {
        return off <= len ? Bytes(ptr + off, len - off) : Bytes{};
    }
    constexpr Bytes subspan(std::size_t off, std::size_t count) const {
        if (off > len) return {};
        const std::size_t n = count > (len - off) ? (len - off) : count;
        return Bytes(ptr + off, n);
    }
};

// ---- Ethernet II (14 bytes) ----
struct Ethernet {
    std::array<std::uint8_t, 6> dst;
    std::array<std::uint8_t, 6> src;
    be<std::uint16_t> ethertype;
};
BOOST_DESCRIBE_STRUCT(Ethernet, (), (dst, src, ethertype))
static_assert(std::is_standard_layout_v<Ethernet> && sizeof(Ethernet) == 14 && alignof(Ethernet) == 1);

// ---- 802.1Q VLAN tag: the 4 bytes following an 0x8100 ethertype (TCI + inner ethertype) ----
struct VlanTag {
    bits<be<std::uint16_t>, field<"pcp", 3>, field<"dei", 1>, field<"vid", 12>> tci;
    be<std::uint16_t> inner_ethertype;
};
BOOST_DESCRIBE_STRUCT(VlanTag, (), (tci, inner_ethertype))
static_assert(std::is_standard_layout_v<VlanTag> && sizeof(VlanTag) == 4 && alignof(VlanTag) == 1);

// ---- IPv4 (20-byte fixed header; options excluded, ihl gives the true length) ----
struct Ipv4 {
    bits<std::uint8_t, field<"version", 4>, field<"ihl", 4>> ver_ihl;          // 1 byte -> no swap
    bits<std::uint8_t, field<"dscp", 6>, field<"ecn", 2>> dscp_ecn;
    be<std::uint16_t> total_length;
    be<std::uint16_t> identification;
    bits<be<std::uint16_t>, field<"flags", 3>, field<"frag_offset", 13>> flags_frag;  // straddles 2 BE bytes
    std::uint8_t ttl;
    std::uint8_t protocol;
    be<std::uint16_t> checksum;
    std::array<std::uint8_t, 4> src;
    std::array<std::uint8_t, 4> dst;
};
BOOST_DESCRIBE_STRUCT(Ipv4, (),
                      (ver_ihl, dscp_ecn, total_length, identification, flags_frag, ttl, protocol, checksum, src, dst))
static_assert(std::is_standard_layout_v<Ipv4> && sizeof(Ipv4) == 20 && alignof(Ipv4) == 1);

// ---- IPv6 (40-byte fixed header) ----
struct Ipv6 {
    bits<be<std::uint32_t>, field<"version", 4>, field<"traffic_class", 8>, field<"flow_label", 20>> vtf;
    be<std::uint16_t> payload_length;
    std::uint8_t next_header;
    std::uint8_t hop_limit;
    std::array<std::uint8_t, 16> src;
    std::array<std::uint8_t, 16> dst;
};
BOOST_DESCRIBE_STRUCT(Ipv6, (), (vtf, payload_length, next_header, hop_limit, src, dst))
static_assert(std::is_standard_layout_v<Ipv6> && sizeof(Ipv6) == 40 && alignof(Ipv6) == 1);

// ---- UDP (8 bytes) ----
struct Udp {
    be<std::uint16_t> src_port;
    be<std::uint16_t> dst_port;
    be<std::uint16_t> length;
    be<std::uint16_t> checksum;
};
BOOST_DESCRIBE_STRUCT(Udp, (), (src_port, dst_port, length, checksum))
static_assert(std::is_standard_layout_v<Udp> && sizeof(Udp) == 8 && alignof(Udp) == 1);

// ---- TCP (20-byte fixed header; data_offset gives the true length) ----
struct Tcp {
    be<std::uint16_t> src_port;
    be<std::uint16_t> dst_port;
    be<std::uint32_t> seq;
    be<std::uint32_t> ack;
    bits<be<std::uint16_t>, field<"data_offset", 4>, field<"reserved", 3>, field<"flags", 9>> off_flags;
    be<std::uint16_t> window;
    be<std::uint16_t> checksum;
    be<std::uint16_t> urgent_ptr;
};
BOOST_DESCRIBE_STRUCT(Tcp, (), (src_port, dst_port, seq, ack, off_flags, window, checksum, urgent_ptr))
static_assert(std::is_standard_layout_v<Tcp> && sizeof(Tcp) == 20 && alignof(Tcp) == 1);

// EtherType / IP-protocol constants used by the decode walk.
inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeIpv6 = 0x86DD;
inline constexpr std::uint16_t kEtherTypeVlan = 0x8100;
inline constexpr std::uint16_t kEtherTypeQinQ = 0x88A8;
inline constexpr std::uint8_t kIpProtoTcp = 6;
inline constexpr std::uint8_t kIpProtoUdp = 17;

// LINKTYPE values (subset). 1 = Ethernet.
inline constexpr std::uint32_t kLinkTypeEthernet = 1;

// Copy a wire header out of the buffer at `off`. Bounds-checked; the structs are 1-aligned and packed
// so a byte-wise copy reconstructs the header exactly (be<>/bits<> convert to host on read).
template <class T>
NANOTINS_HD bool overlay(Bytes bytes, std::size_t off, T& out) {
    if (off + sizeof(T) > bytes.size()) {
        return false;
    }
    std::memcpy(&out, bytes.data() + off, sizeof(T));
    return true;
}

}  // namespace protocols
