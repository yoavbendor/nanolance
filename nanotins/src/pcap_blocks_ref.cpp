// CPU reference implementation of the parsing seam (pcap_blocks.hpp). Handles legacy pcap and pcapng
// SHB/IDB/EPB (+ options), with endianness honored per section. This is the *oracle*: nanotins will
// later provide a drop-in (CPU + CUDA) implementation of the same header. Keep Phase-B pure.

#include "nanotins/pcap_blocks.hpp"

#include <cstring>

namespace pcapblocks {
namespace {

// ---- endianness-aware little readers (plain, device-friendly) ----
inline std::uint16_t rd16(const std::uint8_t* p, bool le) noexcept {
    return le ? static_cast<std::uint16_t>(p[0] | (p[1] << 8))
              : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t rd32(const std::uint8_t* p, bool le) noexcept {
    return le ? (std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
                 (std::uint32_t{p[3]} << 24))
              : (std::uint32_t{p[3]} | (std::uint32_t{p[2]} << 8) | (std::uint32_t{p[1]} << 16) |
                 (std::uint32_t{p[0]} << 24));
}
inline std::uint64_t rd64(const std::uint8_t* p, bool le) noexcept {
    std::uint64_t v = 0;
    if (le) {
        for (int k = 7; k >= 0; --k) {
            v = (v << 8) | p[k];
        }
    } else {
        for (int k = 0; k < 8; ++k) {
            v = (v << 8) | p[k];
        }
    }
    return v;
}

inline std::uint32_t pad4(std::uint32_t n) noexcept { return (n + 3U) & ~3U; }

// pcap global-header magics (interpreted as a host u32 read little-endian from the first 4 bytes).
constexpr std::uint32_t kPcapMagicMicrosLE = 0xA1B2C3D4U;  // file is little-endian, microsecond ts
constexpr std::uint32_t kPcapMagicMicrosBE = 0xD4C3B2A1U;  // file is big-endian, microsecond ts
constexpr std::uint32_t kPcapMagicNanosLE = 0xA1B23C4DU;   // little-endian, nanosecond ts
constexpr std::uint32_t kPcapMagicNanosBE = 0x4D3CB2A1U;   // big-endian, nanosecond ts
constexpr std::uint32_t kPcapngShbType = 0x0A0D0D0AU;      // endianness-independent (palindrome bytes)
constexpr std::uint32_t kPcapngByteOrderMagic = 0x1A2B3C4DU;

inline bool is_pcap_magic_le(std::uint32_t m) noexcept {
    return m == kPcapMagicMicrosLE || m == kPcapMagicNanosLE;
}
inline bool is_pcap_magic(std::uint32_t m) noexcept {
    return is_pcap_magic_le(m) || m == kPcapMagicMicrosBE || m == kPcapMagicNanosBE;
}

Kind classify_block_type(std::uint32_t type) noexcept {
    switch (type) {
        case kBlockTypeShb: return Kind::Shb;
        case kBlockTypeIdb: return Kind::Idb;
        case kBlockTypeEpb: return Kind::Epb;
        case kBlockTypeSpb: return Kind::SimplePacket;
        default: return Kind::Other;
    }
}

}  // namespace

bool next_option(Options& cursor, Option& out) noexcept {
    if (cursor.data == nullptr || cursor.size < 4) {
        return false;
    }
    const std::uint16_t code = rd16(cursor.data, cursor.little_endian);
    const std::uint16_t length = rd16(cursor.data + 2, cursor.little_endian);
    if (code == 0) {  // opt_endofopt
        return false;
    }
    const std::uint32_t advance = 4U + pad4(length);
    if (advance > cursor.size) {
        return false;
    }
    out.code = code;
    out.length = length;
    out.value = cursor.data + 4;
    cursor.data += advance;
    cursor.size -= advance;
    return true;
}

bool scan_window(ScanState& st, Bytes file, std::vector<BlockRef>& out, std::size_t& consumed, bool at_eof,
                 std::string& error) {
    consumed = 0;
    error.clear();
    std::size_t pos = 0;

    // First call: detect format + endianness from the leading magic. The pcap global header (24 B) is
    // consumed here and surfaced as a synthetic IDB so the driver builds its interface table uniformly.
    if (!st.started) {
        if (file.size() < 4) {
            if (at_eof) {
                error = "file too short";
                return false;
            }
            return true;  // need more bytes
        }
        const std::uint32_t lead = rd32(file.data(), /*le=*/true);
        if (lead == kPcapngShbType) {
            st.is_pcapng = true;
            st.started = true;  // endianness resolved at the first SHB in the walk below
        } else if (is_pcap_magic(lead)) {
            if (file.size() < 24) {
                if (at_eof) {
                    error = "pcap file shorter than global header";
                    return false;
                }
                return true;  // need more bytes for the global header
            }
            st.is_pcapng = false;
            st.little_endian = is_pcap_magic_le(lead);
            st.pcap_link_type = rd32(file.data() + 20, st.little_endian);
            st.started = true;
            out.push_back(BlockRef{0, 24, st.pcap_link_type, Kind::Idb, st.little_endian});
            pos = 24;
        } else {
            error = "unrecognized file magic (not pcap or pcapng)";
            return false;
        }
    }

    if (!st.is_pcapng) {
        while (pos + 16 <= file.size()) {
            const std::uint8_t* rec = file.data() + pos;
            const std::uint32_t incl_len = rd32(rec + 8, st.little_endian);
            const std::uint64_t total = std::uint64_t{16} + incl_len;
            if (pos + total > file.size()) {
                break;  // record not fully buffered yet
            }
            out.push_back(BlockRef{pos, static_cast<std::uint32_t>(total), st.pcap_link_type, Kind::PcapRecord,
                                   st.little_endian});
            pos += total;
        }
    } else {
        while (pos + 12 <= file.size()) {
            const std::uint8_t* blk = file.data() + pos;
            // The SHB type is endianness-independent (palindrome bytes); every other block type must be
            // read in the section's byte order. An SHB also (re)sets that byte order from its BOM.
            const bool is_shb = (rd32(blk, /*le=*/true) == kPcapngShbType);
            const bool le = is_shb ? (rd32(blk + 8, true) == kPcapngByteOrderMagic) : st.little_endian;
            const std::uint32_t type = is_shb ? kPcapngShbType : rd32(blk, le);
            const std::uint32_t total = rd32(blk + 4, le);
            if (total < 12) {
                error = "pcapng block length invalid";
                return false;
            }
            if (pos + total > file.size()) {
                break;  // block not fully buffered yet
            }
            if (is_shb) {
                st.little_endian = le;  // commit per-section endianness only once the block fully fits
            }
            out.push_back(BlockRef{pos, total, type, classify_block_type(type), le});
            pos += total;
        }
    }

    consumed = pos;
    if (at_eof && pos < file.size()) {
        error = "trailing truncated block at end of capture";
        return false;
    }
    return true;
}

bool scan_blocks(Bytes file, std::vector<BlockRef>& out, std::string& error) {
    out.clear();
    ScanState st;
    std::size_t consumed = 0;
    return scan_window(st, file, out, consumed, /*at_eof=*/true, error);
}

bool parse_shb(Bytes file, const BlockRef& ref, ShbView& out) noexcept {
    if (ref.kind != Kind::Shb || ref.file_offset + ref.length > file.size() || ref.length < 28) {
        return false;
    }
    const std::uint8_t* body = file.data() + ref.file_offset + 8;  // skip type+total_len
    out.major = rd16(body + 4, ref.little_endian);
    out.minor = rd16(body + 6, ref.little_endian);
    out.section_length = static_cast<std::int64_t>(rd64(body + 8, ref.little_endian));
    const std::uint32_t opt_off = 8U + 16U;  // type+len + (bom+major+minor+section_length)
    out.options.data = file.data() + ref.file_offset + opt_off;
    out.options.size = ref.length - opt_off - 4U;  // minus trailing total_len
    out.options.little_endian = ref.little_endian;
    return true;
}

bool parse_idb(Bytes file, const BlockRef& ref, IdbView& out) noexcept {
    if (ref.kind != Kind::Idb || ref.file_offset + ref.length > file.size()) {
        return false;
    }
    const std::uint8_t* base = file.data() + ref.file_offset;

    // Synthetic IDB over a legacy pcap global header.
    if (ref.length == 24 && is_pcap_magic(rd32(base, true))) {
        const std::uint32_t magic = rd32(base, true);
        const bool nanos = (magic == kPcapMagicNanosLE || magic == kPcapMagicNanosBE);
        out.link_type = static_cast<std::uint16_t>(rd32(base + 20, ref.little_endian));
        out.snaplen = rd32(base + 16, ref.little_endian);
        out.ts_resol = nanos ? 0x09 : 0x06;
        out.options = Options{nullptr, 0, ref.little_endian};
        return true;
    }

    if (ref.length < 20) {
        return false;
    }
    const std::uint8_t* body = base + 8;
    out.link_type = rd16(body + 0, ref.little_endian);
    out.snaplen = rd32(body + 4, ref.little_endian);
    out.ts_resol = 0x06;  // default microseconds unless if_tsresol present
    const std::uint32_t opt_off = 8U + 8U;
    out.options.data = base + opt_off;
    out.options.size = ref.length - opt_off - 4U;
    out.options.little_endian = ref.little_endian;

    Options cursor = out.options;
    Option opt{};
    while (next_option(cursor, opt)) {
        if (opt.code == 9 /*if_tsresol*/ && opt.length >= 1) {
            out.ts_resol = opt.value[0];
        }
    }
    return true;
}

bool parse_epb(Bytes file, const BlockRef& ref, EpbView& out) noexcept {
    if (ref.file_offset + ref.length > file.size()) {
        return false;
    }
    const std::uint8_t* base = file.data() + ref.file_offset;

    if (ref.kind == Kind::PcapRecord) {
        if (ref.length < 16) {
            return false;
        }
        const std::uint32_t ts_sec = rd32(base + 0, ref.little_endian);
        const std::uint32_t ts_frac = rd32(base + 4, ref.little_endian);
        out.interface_id = 0;
        out.ts_raw = (std::uint64_t{ts_sec} << 32) | ts_frac;
        out.caplen = rd32(base + 8, ref.little_endian);
        out.origlen = rd32(base + 12, ref.little_endian);
        out.payload_file_offset = ref.file_offset + 16;
        out.epb_flags = 0;  // legacy pcap records have no options
        out.options = Options{nullptr, 0, ref.little_endian};
        return true;
    }

    if (ref.kind != Kind::Epb || ref.length < 32) {
        return false;
    }
    const std::uint8_t* body = base + 8;  // skip type+total_len
    out.interface_id = rd32(body + 0, ref.little_endian);
    const std::uint32_t ts_high = rd32(body + 4, ref.little_endian);
    const std::uint32_t ts_low = rd32(body + 8, ref.little_endian);
    out.ts_raw = (std::uint64_t{ts_high} << 32) | ts_low;
    out.caplen = rd32(body + 12, ref.little_endian);
    out.origlen = rd32(body + 16, ref.little_endian);
    out.payload_file_offset = ref.file_offset + 8 + 20;  // frame(8) + EPB fixed fields(20)

    const std::uint32_t data_padded = pad4(out.caplen);
    const std::uint32_t opt_off = 8U + 20U + data_padded;
    if (opt_off + 4U <= ref.length) {
        out.options.data = base + opt_off;
        out.options.size = ref.length - opt_off - 4U;
        out.options.little_endian = ref.little_endian;
    } else {
        out.options = Options{nullptr, 0, ref.little_endian};
    }

    out.epb_flags = 0;
    Options cursor = out.options;
    Option opt{};
    while (next_option(cursor, opt)) {
        if (opt.code == 2 /*epb_flags*/ && opt.length >= 4) {
            out.epb_flags = rd32(opt.value, ref.little_endian);
        }
    }
    return true;
}

bool parse_epbs_bulk(Bytes file, const BlockRef* epbs, std::size_t n, EpbColumns& out, std::string& error) {
    if (out.count < n) {
        error = "EpbColumns not pre-sized to the record count";
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        EpbView v{};
        if (!parse_epb(file, epbs[i], v)) {
            error = "failed to parse packet block at index " + std::to_string(i);
            return false;
        }
        out.interface_id[i] = v.interface_id;
        out.ts_raw[i] = v.ts_raw;
        out.caplen[i] = v.caplen;
        out.origlen[i] = v.origlen;
        out.payload_off[i] = v.payload_file_offset;
        out.payload_size[i] = v.caplen;
        out.epb_flags[i] = v.epb_flags;
    }
    return true;
}

}  // namespace pcapblocks
