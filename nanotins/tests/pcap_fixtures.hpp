#pragma once

// Hand-built pcap / pcapng fixtures shared by the seam test and the driver test. Pure byte assembly
// (endianness-parametric) so the tests need no scapy/tshark and stay deterministic.

#include "nanotins/pcap_blocks.hpp"

#include <cstdint>
#include <vector>

namespace pcapfix {

struct Builder {
    std::vector<std::uint8_t> bytes;
    bool le;
    explicit Builder(bool little_endian) : le(little_endian) {}

    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) {
        if (le) {
            bytes.push_back(v & 0xFF);
            bytes.push_back((v >> 8) & 0xFF);
        } else {
            bytes.push_back((v >> 8) & 0xFF);
            bytes.push_back(v & 0xFF);
        }
    }
    void u32(std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            const int shift = le ? (8 * k) : (8 * (3 - k));
            bytes.push_back((v >> shift) & 0xFF);
        }
    }
    void u64(std::uint64_t v) {
        for (int k = 0; k < 8; ++k) {
            const int shift = le ? (8 * k) : (8 * (7 - k));
            bytes.push_back((v >> shift) & 0xFF);
        }
    }
    void raw(const std::vector<std::uint8_t>& v) { bytes.insert(bytes.end(), v.begin(), v.end()); }
};

inline void append_block(Builder& b, std::uint32_t type, const std::vector<std::uint8_t>& body) {
    const std::uint32_t total = 12 + static_cast<std::uint32_t>(((body.size() + 3) / 4) * 4);
    b.u32(type);
    b.u32(total);
    b.raw(body);
    while (b.bytes.size() % 4 != 0) {
        b.u8(0);
    }
    b.u32(total);
}

inline std::vector<std::uint8_t> shb_body(bool le) {
    Builder t(le);
    t.u32(0x1A2B3C4D);
    t.u16(1);
    t.u16(0);
    t.u64(0xFFFFFFFFFFFFFFFFULL);
    // opt: shb_os (code 3) = "nanolance-test"
    const char* os = "nanolance-test";
    t.u16(3);
    t.u16(14);
    for (int i = 0; i < 14; ++i) {
        t.u8(static_cast<std::uint8_t>(os[i]));
    }
    while (t.bytes.size() % 4 != 0) {
        t.u8(0);
    }
    t.u16(0);
    t.u16(0);
    return t.bytes;
}

inline std::vector<std::uint8_t> idb_body(bool le, std::uint16_t link_type, std::uint8_t tsresol) {
    Builder t(le);
    t.u16(link_type);
    t.u16(0);
    t.u32(65535);
    t.u16(9);  // if_tsresol
    t.u16(1);
    t.u8(tsresol);
    t.u8(0);
    t.u8(0);
    t.u8(0);
    t.u16(0);
    t.u16(0);
    return t.bytes;
}

inline std::vector<std::uint8_t> epb_body(bool le, std::uint32_t iface, std::uint64_t ts, std::uint32_t caplen,
                                          std::uint32_t origlen, const std::vector<std::uint8_t>& data,
                                          std::uint32_t flags) {
    Builder t(le);
    t.u32(iface);
    t.u32(static_cast<std::uint32_t>(ts >> 32));
    t.u32(static_cast<std::uint32_t>(ts & 0xFFFFFFFF));
    t.u32(caplen);
    t.u32(origlen);
    t.raw(data);
    while (t.bytes.size() % 4 != 0) {
        t.u8(0);
    }
    t.u16(2);  // epb_flags
    t.u16(4);
    t.u32(flags);
    t.u16(0);
    t.u16(0);
    return t.bytes;
}

inline const std::vector<std::uint8_t> kPayload0 = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
inline const std::vector<std::uint8_t> kPayload1 = {0xCA, 0xFE, 0xBA, 0xBE, 0x02, 0x03, 0x04};

inline std::vector<std::uint8_t> build_pcapng(bool le) {
    Builder b(le);
    append_block(b, pcapblocks::kBlockTypeShb, shb_body(le));
    append_block(b, pcapblocks::kBlockTypeIdb, idb_body(le, /*link_type=*/1, /*tsresol=*/6));
    append_block(b, pcapblocks::kBlockTypeEpb,
                 epb_body(le, 0, 0x0000000100000002ULL, static_cast<std::uint32_t>(kPayload0.size()),
                          static_cast<std::uint32_t>(kPayload0.size()), kPayload0, 0x00000001));
    append_block(b, pcapblocks::kBlockTypeEpb,
                 epb_body(le, 0, 0x00000003AABBCCDDULL, static_cast<std::uint32_t>(kPayload1.size()), 99,
                          kPayload1, 0));
    return b.bytes;
}

inline std::vector<std::uint8_t> build_pcap(bool le) {
    Builder b(le);
    b.u32(0xA1B2C3D4U);
    b.u16(2);
    b.u16(4);
    b.u32(0);
    b.u32(0);
    b.u32(65535);
    b.u32(1);
    b.u32(111);
    b.u32(222);
    b.u32(static_cast<std::uint32_t>(kPayload0.size()));
    b.u32(static_cast<std::uint32_t>(kPayload0.size()));
    b.raw(kPayload0);
    b.u32(333);
    b.u32(444);
    b.u32(static_cast<std::uint32_t>(kPayload1.size()));
    b.u32(55);
    b.raw(kPayload1);
    return b.bytes;
}

}  // namespace pcapfix
