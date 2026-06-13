#pragma once

// PTPv2 / gPTP (IEEE 1588 / 802.1AS) as struct_specs — the first real use of embed<> spec composition.
// PTP is a common 34-byte header followed by a per-messageType body, so the DAG dispatches on the header's
// message_type:4 field into one of these body specs. Rather than re-declare the header (and the Timestamp /
// PortIdentity / ClockQuality sub-records that recur across messages), each message spec embed<>s them:
//
//   SyncMsgSpec = PtpHeaderSpec @0  +  TimestampSpec @34 (prefix "origin_")
//
// embed<Prefix, Offset, SubSpec> shifts every sub-field by Offset and prefixes its name, so the double
// PortIdentity in DelayResp (the header's source + the body's requesting) lands as distinct columns
// (clock_identity vs requesting_clock_identity). The header column names match protocols::Gptp exactly, so
// PtpHeaderSpec reads byte-identically to the existing Gptp overlay (see test_struct_spec_ptp).

#include "nanotins/struct_spec.hpp"

#include <cstdint>

namespace nanotins {

using namespace literals;

// ---- reusable PTP sub-records -------------------------------------------------------------------------

// PTP Timestamp (10 bytes): 48-bit seconds (u16 msb + u32 lsb) + 32-bit nanoseconds. The double-seconds
// reconstruction is a numeric-layer concern; the spec exposes the raw wire fields.
using TimestampSpec = StructSpec<
    named_field<decltype("seconds_msb"_fld), 0, std::uint16_t, wire_endian::big>,
    named_field<decltype("seconds_lsb"_fld), 2, std::uint32_t, wire_endian::big>,
    named_field<decltype("nanoseconds"_fld), 6, std::uint32_t, wire_endian::big>>;

// PortIdentity (10 bytes): an 8-byte ClockIdentity + a 16-bit port number.
using PortIdentitySpec = StructSpec<
    named_bytes_field<decltype("clock_identity"_fld), 0, 8>,
    named_field<decltype("port_number"_fld), 8, std::uint16_t, wire_endian::big>>;

// ClockQuality (4 bytes).
using ClockQualitySpec = StructSpec<
    named_field<decltype("clock_class"_fld), 0, std::uint8_t, wire_endian::big>,
    named_field<decltype("clock_accuracy"_fld), 1, std::uint8_t, wire_endian::big>,
    named_field<decltype("offset_scaled_log_variance"_fld), 2, std::uint16_t, wire_endian::big>>;

// ---- the common 34-byte header (column names match protocols::Gptp) -----------------------------------
// byte 0: transport_specific (high nibble) | message_type (low nibble); byte 1: reserved | version_ptp.
using PtpHeaderSpec = StructSpec<
    named_bit_field<decltype("transport_specific"_fld), 0, std::uint8_t, 0, 4, wire_endian::big>,
    named_bit_field<decltype("message_type"_fld), 0, std::uint8_t, 4, 4, wire_endian::big>,
    named_bit_field<decltype("version_ptp"_fld), 1, std::uint8_t, 4, 4, wire_endian::big>,
    named_field<decltype("message_length"_fld), 2, std::uint16_t, wire_endian::big>,
    named_field<decltype("domain_number"_fld), 4, std::uint8_t, wire_endian::big>,
    named_field<decltype("flags"_fld), 6, std::uint16_t, wire_endian::big>,
    named_field<decltype("correction_field"_fld), 8, std::uint64_t, wire_endian::big>,
    named_bytes_field<decltype("clock_identity"_fld), 20, 8>,
    named_field<decltype("source_port_number"_fld), 28, std::uint16_t, wire_endian::big>,
    named_field<decltype("sequence_id"_fld), 30, std::uint16_t, wire_endian::big>,
    named_field<decltype("control_field"_fld), 32, std::uint8_t, wire_endian::big>,
    named_field<decltype("log_message_interval"_fld), 33, std::int8_t, wire_endian::big>>;

inline constexpr std::size_t kPtpHeaderLen = 34;

// ---- per-messageType bodies (header + body sub-records, composed with embed<>) -------------------------
// no-prefix splice of the header at offset 0 — written once, reused by every message below.
using _PtpHdr = embed<decltype(""_fld), 0, PtpHeaderSpec>;

// Sync / Follow_Up / Delay_Req / Pdelay_Req all = header + one Timestamp at byte 34 (44 bytes on the wire).
// The timestamp's role differs (originTimestamp / preciseOriginTimestamp / receiveTimestamp), so the column
// prefix names it.
using SyncMsgSpec = StructSpec<_PtpHdr, embed<decltype("origin_"_fld), 34, TimestampSpec>>;
using FollowUpMsgSpec = StructSpec<_PtpHdr, embed<decltype("precise_origin_"_fld), 34, TimestampSpec>>;
using DelayReqMsgSpec = StructSpec<_PtpHdr, embed<decltype("origin_"_fld), 34, TimestampSpec>>;

// Delay_Resp (54 bytes): header + receiveTimestamp @34 + requestingPortIdentity @44. The requesting
// PortIdentity gets its own prefix so it does not collide with the header's source clock_identity.
using DelayRespMsgSpec = StructSpec<
    _PtpHdr,
    embed<decltype("receive_"_fld), 34, TimestampSpec>,
    embed<decltype("requesting_"_fld), 44, PortIdentitySpec>>;

// Announce (64 bytes): header + originTimestamp @34 + the grandmaster fields. grandmasterClockQuality is a
// ClockQuality sub-record embedded at byte 48.
using AnnounceMsgSpec = StructSpec<
    _PtpHdr,
    embed<decltype("origin_"_fld), 34, TimestampSpec>,
    named_field<decltype("current_utc_offset"_fld), 44, std::int16_t, wire_endian::big>,
    named_field<decltype("grandmaster_priority1"_fld), 47, std::uint8_t, wire_endian::big>,
    embed<decltype("grandmaster_"_fld), 48, ClockQualitySpec>,
    named_field<decltype("grandmaster_priority2"_fld), 52, std::uint8_t, wire_endian::big>,
    named_bytes_field<decltype("grandmaster_identity"_fld), 53, 8>,
    named_field<decltype("steps_removed"_fld), 61, std::uint16_t, wire_endian::big>,
    named_field<decltype("time_source"_fld), 63, std::uint8_t, wire_endian::big>>;

// messageType values (header octet 0, low nibble) — the DAG's dispatch keys.
inline constexpr std::uint8_t kPtpMsgSync = 0x0;
inline constexpr std::uint8_t kPtpMsgDelayReq = 0x1;
inline constexpr std::uint8_t kPtpMsgPdelayReq = 0x2;
inline constexpr std::uint8_t kPtpMsgPdelayResp = 0x3;
inline constexpr std::uint8_t kPtpMsgFollowUp = 0x8;
inline constexpr std::uint8_t kPtpMsgDelayResp = 0x9;
inline constexpr std::uint8_t kPtpMsgPdelayRespFollowUp = 0xA;
inline constexpr std::uint8_t kPtpMsgAnnounce = 0xB;
inline constexpr std::uint8_t kPtpMsgSignaling = 0xC;

// ---- per-messageType BODY specs (decoded at offset kPtpHeaderLen, i.e. body-relative offset 0) --------
// The DAG's GptpNode emits the common header (PtpHeaderSpec) then dispatches message_type into one of
// these BODY nodes, which parse at byte kPtpHeaderLen. The bodies are grouped by wire shape — the gptp
// table's message_type column disambiguates which message a row came from. (The full-message *MsgSpec
// types above embed the header; these are body-only, for the header+body split the DAG uses.)

// Sync / Follow_Up / Delay_Req / Pdelay_Req: a single 10-byte Timestamp (origin / preciseOrigin).
using PtpTimestampBodySpec = StructSpec<embed<decltype("ts_"_fld), 0, TimestampSpec>>;

// Delay_Resp / Pdelay_Resp / Pdelay_Resp_Follow_Up: a Timestamp + the requesting PortIdentity (20 bytes).
using PtpTsPortBodySpec = StructSpec<
    embed<decltype("ts_"_fld), 0, TimestampSpec>,
    embed<decltype("req_"_fld), 10, PortIdentitySpec>>;

// Announce body (30 bytes after the header): originTimestamp + the grandmaster fields.
using PtpAnnounceBodySpec = StructSpec<
    embed<decltype("origin_"_fld), 0, TimestampSpec>,
    named_field<decltype("current_utc_offset"_fld), 10, std::int16_t, wire_endian::big>,
    named_field<decltype("grandmaster_priority1"_fld), 13, std::uint8_t, wire_endian::big>,
    embed<decltype("grandmaster_"_fld), 14, ClockQualitySpec>,
    named_field<decltype("grandmaster_priority2"_fld), 18, std::uint8_t, wire_endian::big>,
    named_bytes_field<decltype("grandmaster_identity"_fld), 19, 8>,
    named_field<decltype("steps_removed"_fld), 27, std::uint16_t, wire_endian::big>,
    named_field<decltype("time_source"_fld), 29, std::uint8_t, wire_endian::big>>;

// Signaling: the targetPortIdentity (10 bytes); any trailing TLVs are not decoded.
using PtpSignalingBodySpec = StructSpec<embed<decltype("target_"_fld), 0, PortIdentitySpec>>;

}  // namespace nanotins
