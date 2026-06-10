// Seam acceptance test (targets pcap_blocks.hpp ONLY, so it re-runs unchanged against a future
// nanotins implementation). Hand-built pcap + pcapng fixtures, incl. a byte-swapped pcapng for
// endianness, plus external-offset verification (payload bytes must live at payload_off..+size).

#include "nanotins/pcap_blocks.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using pcapblocks::BlockRef;
using pcapblocks::Bytes;
using pcapblocks::Kind;

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "seam test failed: %s\n", msg);
        std::exit(1);
    }
}

// ---- byte-buffer builder (endianness-parametric) ----
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
    void pad4() {
        while (bytes.size() % 4 != 0) {
            bytes.push_back(0);
        }
    }
    std::size_t size() const { return bytes.size(); }
};

// Append a complete pcapng block: [type][total_len][body...][total_len]. `body` excludes framing.
void append_block(Builder& b, std::uint32_t type, const std::vector<std::uint8_t>& body) {
    const std::uint32_t total = 12 + static_cast<std::uint32_t>(((body.size() + 3) / 4) * 4);
    b.u32(type);
    b.u32(total);
    b.raw(body);
    while (b.bytes.size() % 4 != 0) {
        b.u8(0);
    }
    b.u32(total);
}

// Build the SHB / IDB / EPB bodies via a temporary builder so endianness is consistent.
std::vector<std::uint8_t> shb_body(bool le) {
    Builder t(le);
    t.u32(0x1A2B3C4D);                  // byte-order magic (written in section endianness)
    t.u16(1);                           // major
    t.u16(0);                           // minor
    t.u64(0xFFFFFFFFFFFFFFFFULL);       // section_length = -1
    t.u16(0);                           // opt_endofopt
    t.u16(0);
    return t.bytes;
}

std::vector<std::uint8_t> idb_body(bool le, std::uint16_t link_type, std::uint8_t tsresol) {
    Builder t(le);
    t.u16(link_type);
    t.u16(0);       // reserved
    t.u32(65535);   // snaplen
    t.u16(9);       // if_tsresol code
    t.u16(1);       // length
    t.u8(tsresol);  // value
    t.u8(0);        // pad
    t.u8(0);
    t.u8(0);
    t.u16(0);  // opt_endofopt
    t.u16(0);
    return t.bytes;
}

std::vector<std::uint8_t> epb_body(bool le, std::uint32_t iface, std::uint64_t ts, std::uint32_t caplen,
                                   std::uint32_t origlen, const std::vector<std::uint8_t>& data,
                                   std::uint32_t flags) {
    Builder t(le);
    t.u32(iface);
    t.u32(static_cast<std::uint32_t>(ts >> 32));    // ts_high
    t.u32(static_cast<std::uint32_t>(ts & 0xFFFFFFFF));  // ts_low
    t.u32(caplen);
    t.u32(origlen);
    t.raw(data);
    while (t.bytes.size() % 4 != 0) {
        t.u8(0);  // packet data padded to 32-bit
    }
    t.u16(2);  // epb_flags code
    t.u16(4);  // length
    t.u32(flags);
    t.u16(0);  // opt_endofopt
    t.u16(0);
    return t.bytes;
}

const std::vector<std::uint8_t> kPayload0 = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
const std::vector<std::uint8_t> kPayload1 = {0xCA, 0xFE, 0xBA, 0xBE, 0x02, 0x03, 0x04};

// Build a full pcapng capture: SHB + IDB + 2 EPB. Returns the bytes.
std::vector<std::uint8_t> build_pcapng(bool le) {
    Builder b(le);
    append_block(b, pcapblocks::kBlockTypeShb, shb_body(le));
    append_block(b, pcapblocks::kBlockTypeIdb, idb_body(le, /*link_type=*/1, /*tsresol=*/6));
    append_block(b, pcapblocks::kBlockTypeEpb,
                 epb_body(le, 0, 0x0000000100000002ULL, static_cast<std::uint32_t>(kPayload0.size()),
                          static_cast<std::uint32_t>(kPayload0.size()), kPayload0, 0x00000001));
    append_block(b, pcapblocks::kBlockTypeEpb,
                 epb_body(le, 0, 0x00000003AABBCCDDULL, static_cast<std::uint32_t>(kPayload1.size()),
                          99, kPayload1, 0));
    return b.bytes;
}

// Build a legacy pcap capture (24B global header + 2 records). Microsecond magic.
std::vector<std::uint8_t> build_pcap(bool le) {
    Builder b(le);
    b.u32(0xA1B2C3D4U);  // microsecond magic; builder writes bytes in `le` order
    b.u16(2);   // version major
    b.u16(4);   // version minor
    b.u32(0);   // thiszone
    b.u32(0);   // sigfigs
    b.u32(65535);  // snaplen
    b.u32(1);   // linktype = Ethernet
    // record 0
    b.u32(111);  // ts_sec
    b.u32(222);  // ts_frac
    b.u32(static_cast<std::uint32_t>(kPayload0.size()));  // incl_len
    b.u32(static_cast<std::uint32_t>(kPayload0.size()));  // orig_len
    b.raw(kPayload0);
    // record 1
    b.u32(333);
    b.u32(444);
    b.u32(static_cast<std::uint32_t>(kPayload1.size()));
    b.u32(55);
    b.raw(kPayload1);
    return b.bytes;
}

std::size_t count_kind(const std::vector<BlockRef>& refs, Kind k) {
    std::size_t n = 0;
    for (const auto& r : refs) {
        if (r.kind == k) {
            ++n;
        }
    }
    return n;
}

void verify_external_offset(const std::vector<std::uint8_t>& file, std::uint64_t off, std::uint32_t size,
                            const std::vector<std::uint8_t>& expected, const char* what) {
    require(off + size <= file.size(), what);
    require(size == expected.size(), what);
    require(std::memcmp(file.data() + off, expected.data(), size) == 0, what);
}

void test_pcapng(bool le) {
    const auto file = build_pcapng(le);
    Bytes span(file.data(), file.size());
    std::vector<BlockRef> refs;
    std::string err;
    require(pcapblocks::scan_blocks(span, refs, err), err.c_str());
    require(count_kind(refs, Kind::Shb) == 1, "pcapng: 1 SHB");
    require(count_kind(refs, Kind::Idb) == 1, "pcapng: 1 IDB");
    require(count_kind(refs, Kind::Epb) == 2, "pcapng: 2 EPB");

    // IDB
    pcapblocks::IdbView idb{};
    for (const auto& r : refs) {
        if (r.kind == Kind::Idb) {
            require(pcapblocks::parse_idb(span, r, idb), "parse_idb");
        }
    }
    require(idb.link_type == 1, "idb link_type");
    require(idb.snaplen == 65535, "idb snaplen");
    require(idb.ts_resol == 6, "idb ts_resol");

    // EPBs + external offsets
    std::vector<BlockRef> epbs;
    for (const auto& r : refs) {
        if (r.kind == Kind::Epb) {
            epbs.push_back(r);
        }
    }
    pcapblocks::EpbView e0{}, e1{};
    require(pcapblocks::parse_epb(span, epbs[0], e0), "parse_epb 0");
    require(pcapblocks::parse_epb(span, epbs[1], e1), "parse_epb 1");
    require(e0.ts_raw == 0x0000000100000002ULL, "epb0 ts_raw");
    require(e0.caplen == kPayload0.size(), "epb0 caplen");
    require(e1.origlen == 99, "epb1 origlen");
    verify_external_offset(file, e0.payload_file_offset, e0.caplen, kPayload0, "epb0 payload");
    verify_external_offset(file, e1.payload_file_offset, e1.caplen, kPayload1, "epb1 payload");

    // Bulk path + epb_flags
    std::vector<std::uint32_t> iface(2), caplen(2), origlen(2), psize(2), flags(2);
    std::vector<std::uint64_t> ts(2), poff(2);
    pcapblocks::EpbColumns cols{iface.data(), ts.data(),  caplen.data(), origlen.data(),
                                poff.data(),  psize.data(), flags.data(),  2};
    require(pcapblocks::parse_epbs_bulk(span, epbs.data(), 2, cols, err), err.c_str());
    require(flags[0] == 0x00000001, "epb0 flags");
    require(flags[1] == 0, "epb1 flags");
    require(poff[0] == e0.payload_file_offset, "bulk payload_off");
}

void test_pcap() {
    const auto file = build_pcap(true);
    Bytes span(file.data(), file.size());
    std::vector<BlockRef> refs;
    std::string err;
    require(pcapblocks::scan_blocks(span, refs, err), err.c_str());
    require(count_kind(refs, Kind::Idb) == 1, "pcap: 1 synthetic IDB");
    require(count_kind(refs, Kind::PcapRecord) == 2, "pcap: 2 records");

    pcapblocks::IdbView idb{};
    for (const auto& r : refs) {
        if (r.kind == Kind::Idb) {
            require(pcapblocks::parse_idb(span, r, idb), "pcap parse_idb");
        }
    }
    require(idb.link_type == 1, "pcap link_type");
    require(idb.ts_resol == 6, "pcap ts_resol microseconds");

    std::vector<BlockRef> recs;
    for (const auto& r : refs) {
        if (r.kind == Kind::PcapRecord) {
            recs.push_back(r);
        }
    }
    pcapblocks::EpbView r0{}, r1{};
    require(pcapblocks::parse_epb(span, recs[0], r0), "pcap parse rec0");
    require(pcapblocks::parse_epb(span, recs[1], r1), "pcap parse rec1");
    require(r0.ts_raw == ((std::uint64_t{111} << 32) | 222), "pcap rec0 ts");
    require(r1.origlen == 55, "pcap rec1 origlen");
    verify_external_offset(file, r0.payload_file_offset, r0.caplen, kPayload0, "pcap rec0 payload");
    verify_external_offset(file, r1.payload_file_offset, r1.caplen, kPayload1, "pcap rec1 payload");
}

}  // namespace

int main() {
    test_pcapng(/*le=*/true);
    test_pcapng(/*le=*/false);  // byte-swapped (big-endian) section
    test_pcap();
    std::puts("pcap seam test ok");
    return 0;
}
