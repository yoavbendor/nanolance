#pragma once

// gPTP / PTPv2 (IEEE 802.1AS / IEEE 1588) common message header — a *worked example of extending
// nanotins with a new protocol*. It shows the whole recipe in one place:
//
//   1. Describe the wire bytes once: a packed struct of be<>/bits<>/array fields + one
//      BOOST_DESCRIBE_STRUCT line. That single description gives you, for free:
//        - a zero-copy `overlay()` on raw packet bytes (endianness/bit-layout handled by the field types),
//        - `columns_of<Gptp>` (the flattened column list, bitfields expanded to named columns),
//        - `soa<Gptp>` storage + `arrow_schema<Gptp>()` / `to_arrow<Gptp>()` (a nanoarrow table).
//   2. Wire it into a decode walk with ONE branch on the discriminator (here: EtherType 0x88F7), exactly
//      like Ethernet/IPv4/UDP in protocol_decode.hpp. `decode_gptp` below is that branch, factored out.
//
// gPTP rides directly on L2 (Ethernet, optionally VLAN-tagged) — there is no IP/UDP layer — so the
// discriminator is the (post-VLAN) EtherType, not an ip_proto. See docs/nanotins.html for the narrative.

#include "nanotins/bits.hpp"
#include "nanotins/endian.hpp"
#include "nanotins/protocols.hpp"

#include <boost/describe.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

namespace protocols {

// EtherType for IEEE 1588 / 802.1AS PTP over Ethernet ("layer-2" PTP).
inline constexpr std::uint16_t kEtherTypePtp = 0x88F7;

// PTPv2 / gPTP common header (34 bytes). `reserved*` members keep the byte layout exact but are left out
// of BOOST_DESCRIBE_STRUCT, so they never appear as table columns — describe the fields you want, no more.
struct Gptp {
    bits<std::uint8_t, field<"transport_specific", 4>, field<"message_type", 4>> ts_msgtype;  // octet 0
    bits<std::uint8_t, field<"reserved0", 4>, field<"version_ptp", 4>> rsv_version;            // octet 1
    be<std::uint16_t> message_length;                                                          // 2..3
    std::uint8_t domain_number;                                                                // 4
    std::uint8_t reserved1;                                                                    // 5
    be<std::uint16_t> flags;                                                                   // 6..7
    be<std::uint64_t> correction_field;                                                        // 8..15
    be<std::uint32_t> reserved2;                                                               // 16..19
    std::array<std::uint8_t, 8> clock_identity;  // sourcePortIdentity.clockIdentity           // 20..27
    be<std::uint16_t> source_port_number;        // sourcePortIdentity.portNumber              // 28..29
    be<std::uint16_t> sequence_id;                                                             // 30..31
    std::uint8_t control_field;                                                                // 32
    std::int8_t log_message_interval;                                                          // 33
};
BOOST_DESCRIBE_STRUCT(Gptp, (),
                      (ts_msgtype, rsv_version, message_length, domain_number, flags, correction_field,
                       clock_identity, source_port_number, sequence_id, control_field, log_message_interval))
static_assert(std::is_standard_layout_v<Gptp> && sizeof(Gptp) == 34 && alignof(Gptp) == 1,
              "Gptp must overlay 34 wire bytes with no padding");

// PTP messageType values (octet 0, low nibble) — the common event/general messages.
inline constexpr std::uint8_t kPtpMsgSync = 0x0;
inline constexpr std::uint8_t kPtpMsgDelayReq = 0x1;
inline constexpr std::uint8_t kPtpMsgPdelayReq = 0x2;
inline constexpr std::uint8_t kPtpMsgPdelayResp = 0x3;
inline constexpr std::uint8_t kPtpMsgFollowUp = 0x8;
inline constexpr std::uint8_t kPtpMsgDelayResp = 0x9;
inline constexpr std::uint8_t kPtpMsgAnnounce = 0xB;

// The decode branch: if `ethertype` is PTP, overlay the header at the start of `bytes`. Returns false if
// it is not PTP or the header does not fit. Drop this call into a decode walk right after the VLAN loop
// (where `ethertype` is the inner-most EtherType), alongside the IPv4/IPv6 branches.
inline bool decode_gptp(std::uint16_t ethertype, Bytes bytes, Gptp& out) noexcept {
    if (ethertype != kEtherTypePtp) {
        return false;
    }
    return overlay(bytes, 0, out);
}

}  // namespace protocols
