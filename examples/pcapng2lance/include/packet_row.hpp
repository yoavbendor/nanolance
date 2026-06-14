// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

// The all-scalar L1 packet row that flows through the nanotins reflection core (one Lance row per packet).
// payload_uri/off/size are NOT here — they ride in the lance.blob.v2 `payload_ref` struct appended
// alongside. Kept in its own header so the driver, tests, and tools share one definition of the schema.

#include "soatins/describe.hpp"

#include <boost/describe.hpp>

#include <cstdint>

namespace pcapng2lance {

struct PacketRow {
    std::uint64_t packet_id;  // stable row id; join key for the per-PDU / staged tables
    std::uint32_t interface_id;
    std::uint64_t ts_raw;
    std::uint32_t caplen;
    std::uint32_t origlen;
    std::uint16_t link_type;  // denormalized from the interface (ConstantLayout when single-iface)
    std::uint8_t ts_resol;    // denormalized; lets a row self-describe its time unit
    std::uint32_t epb_flags;
};
BOOST_DESCRIBE_STRUCT(PacketRow, (),
                      (packet_id, interface_id, ts_raw, caplen, origlen, link_type, ts_resol, epb_flags))

}  // namespace pcapng2lance
