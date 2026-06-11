#pragma once

// pcap / pcapng block parsing — the entry layer of nanotins. Handles legacy pcap and pcapng
// SHB/IDB/EPB (+ options), with endianness honored per section. Header-only and split into two phases:
//
//   Phase A  scan_blocks() / scan_window()   sequential boundary walk -> flat BlockRef[]
//                                            (inherently serial; host-only — uses std::vector)
//   Phase B  parse_* / parse_epbs_bulk       pure per-block parse -> typed POD views / columnar SoA
//
// Phase-B functions are the device-callable kernels: `noexcept`, allocation-free, no STL/globals in the
// hot path, operating on one contiguous byte span — so they are valid as CUDA `__device__` code (hence
// `NANOTINS_HD` and why they live in this header, not a .cpp: device code needs the definitions visible).
// The scheduler-agnostic `bulk_for_each` runs `parse_epb` per `BlockRef`; on a CUDA host the same call
// runs on the GPU. Keep Phase B pure.

#include "soatins/portability.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pcapblocks {

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

enum class Kind : std::uint8_t { Shb, Idb, Epb, PcapRecord, SimplePacket, Other };

// pcapng block type constants (host order, after endianness resolution).
inline constexpr std::uint32_t kBlockTypeShb = 0x0A0D0D0AU;
inline constexpr std::uint32_t kBlockTypeIdb = 0x00000001U;
inline constexpr std::uint32_t kBlockTypeEpb = 0x00000006U;
inline constexpr std::uint32_t kBlockTypeSpb = 0x00000003U;

// Phase-A output: a flat, trivially-copyable boundary record (host<->device memcpy-able).
struct BlockRef {
    std::uint64_t file_offset;   // start of the block/record in the file
    std::uint32_t length;        // total length incl. frame/padding
    std::uint32_t type_or_link;  // pcapng block type, or pcap linktype for PcapRecord
    Kind kind;
    bool little_endian;  // byte order for this block's fields
};

// Zero-copy option cursor shared by all block kinds. Iterated, never allocated.
struct Options {
    const std::uint8_t* data;
    std::uint32_t size;
    bool little_endian;
};
struct Option {
    std::uint16_t code;
    std::uint16_t length;
    const std::uint8_t* value;
};

// Typed POD views (offsets/spans into the file buffer; no copies).
struct ShbView {
    std::uint16_t major, minor;
    std::int64_t section_length;
    Options options;
};
struct IdbView {
    std::uint16_t link_type;
    std::uint32_t snaplen;
    std::uint8_t ts_resol;  // if_tsresol (pcapng) or derived from magic (pcap); 0x06 = microseconds
    Options options;
};
struct EpbView {
    std::uint32_t interface_id;
    std::uint64_t ts_raw;  // (ts_high << 32) | ts_low
    std::uint32_t caplen, origlen;
    std::uint64_t payload_file_offset;  // offset of packet bytes (relative to the parsed buffer)
    std::uint32_t epb_flags;            // epb_flags option (0 when absent) — parsed in, so the view is
                                        // self-contained and the bulk kernel needs no option walk
    Options options;
};

// Phase-A streaming state: carries format/endianness across windows (detected on the first call from the
// leading magic; per-section endianness updated at each SHB).
struct ScanState {
    bool started = false;
    bool is_pcapng = false;
    bool little_endian = true;
    std::uint32_t pcap_link_type = 0;  // carried so PcapRecord refs report the link type
};

// Phase-B bulk SoA output buffers, pre-sized to the EPB/record count. The CPU loop calls parse_epb; a
// CUDA path runs one thread per BlockRef writing the same columns.
struct EpbColumns {
    std::uint32_t* interface_id;
    std::uint64_t* ts_raw;
    std::uint32_t* caplen;
    std::uint32_t* origlen;
    std::uint64_t* payload_off;
    std::uint32_t* payload_size;
    std::uint32_t* epb_flags;  // option-derived; 0 when absent
    std::size_t count;
};

// ---- device-friendly endianness-aware readers + small helpers (implementation detail) ----
namespace detail {

inline NANOTINS_HD std::uint16_t rd16(const std::uint8_t* p, bool le) noexcept {
    return le ? static_cast<std::uint16_t>(p[0] | (p[1] << 8))
              : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline NANOTINS_HD std::uint32_t rd32(const std::uint8_t* p, bool le) noexcept {
    return le ? (std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
                 (std::uint32_t{p[3]} << 24))
              : (std::uint32_t{p[3]} | (std::uint32_t{p[2]} << 8) | (std::uint32_t{p[1]} << 16) |
                 (std::uint32_t{p[0]} << 24));
}
inline NANOTINS_HD std::uint64_t rd64(const std::uint8_t* p, bool le) noexcept {
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
inline NANOTINS_HD std::uint32_t pad4(std::uint32_t n) noexcept { return (n + 3U) & ~3U; }

// pcap global-header magics (interpreted as a host u32 read little-endian from the first 4 bytes).
inline constexpr std::uint32_t kPcapMagicMicrosLE = 0xA1B2C3D4U;  // little-endian file, microsecond ts
inline constexpr std::uint32_t kPcapMagicMicrosBE = 0xD4C3B2A1U;  // big-endian file, microsecond ts
inline constexpr std::uint32_t kPcapMagicNanosLE = 0xA1B23C4DU;   // little-endian, nanosecond ts
inline constexpr std::uint32_t kPcapMagicNanosBE = 0x4D3CB2A1U;   // big-endian, nanosecond ts
inline constexpr std::uint32_t kPcapngByteOrderMagic = 0x1A2B3C4DU;

inline NANOTINS_HD bool is_pcap_magic_le(std::uint32_t m) noexcept {
    return m == kPcapMagicMicrosLE || m == kPcapMagicNanosLE;
}
inline NANOTINS_HD bool is_pcap_magic(std::uint32_t m) noexcept {
    return is_pcap_magic_le(m) || m == kPcapMagicMicrosBE || m == kPcapMagicNanosBE;
}
inline NANOTINS_HD Kind classify_block_type(std::uint32_t type) noexcept {
    switch (type) {
        case kBlockTypeShb: return Kind::Shb;
        case kBlockTypeIdb: return Kind::Idb;
        case kBlockTypeEpb: return Kind::Epb;
        case kBlockTypeSpb: return Kind::SimplePacket;
        default: return Kind::Other;
    }
}

}  // namespace detail

// Advance to the next option; false at opt_endofopt or end of buffer.
inline NANOTINS_HD bool next_option(Options& cursor, Option& out) noexcept {
    if (cursor.data == nullptr || cursor.size < 4) {
        return false;
    }
    const std::uint16_t code = detail::rd16(cursor.data, cursor.little_endian);
    const std::uint16_t length = detail::rd16(cursor.data + 2, cursor.little_endian);
    if (code == 0) {  // opt_endofopt
        return false;
    }
    const std::uint32_t advance = 4U + detail::pad4(length);
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

// ---- Phase A: sequential boundary scan (the only inherently serial part; host-only). ----
// Streaming scan: walk complete blocks in one window of an endless capture without buffering the whole
// file. `st` carries format/endianness across windows. Emits BlockRefs with WINDOW-relative file_offset;
// `consumed` is set to the bytes covered by complete blocks (slide the window by it). A trailing partial
// block means "need more bytes" unless `at_eof`, where it is a truncation error. `out` is appended to.
inline bool scan_window(ScanState& st, Bytes file, std::vector<BlockRef>& out, std::size_t& consumed,
                        bool at_eof, std::string& error) {
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
        const std::uint32_t lead = detail::rd32(file.data(), /*le=*/true);
        if (lead == kBlockTypeShb) {
            st.is_pcapng = true;
            st.started = true;  // endianness resolved at the first SHB in the walk below
        } else if (detail::is_pcap_magic(lead)) {
            if (file.size() < 24) {
                if (at_eof) {
                    error = "pcap file shorter than global header";
                    return false;
                }
                return true;  // need more bytes for the global header
            }
            st.is_pcapng = false;
            st.little_endian = detail::is_pcap_magic_le(lead);
            st.pcap_link_type = detail::rd32(file.data() + 20, st.little_endian);
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
            const std::uint32_t incl_len = detail::rd32(rec + 8, st.little_endian);
            const std::uint64_t total = std::uint64_t{16} + incl_len;
            if (pos + total > file.size()) {
                break;  // record not fully buffered yet
            }
            out.push_back(BlockRef{pos, static_cast<std::uint32_t>(total), st.pcap_link_type,
                                   Kind::PcapRecord, st.little_endian});
            pos += total;
        }
    } else {
        while (pos + 12 <= file.size()) {
            const std::uint8_t* blk = file.data() + pos;
            // The SHB type is endianness-independent (palindrome bytes); every other block type must be
            // read in the section's byte order. An SHB also (re)sets that byte order from its BOM.
            const bool is_shb = (detail::rd32(blk, /*le=*/true) == kBlockTypeShb);
            const bool le = is_shb ? (detail::rd32(blk + 8, true) == detail::kPcapngByteOrderMagic)
                                   : st.little_endian;
            const std::uint32_t type = is_shb ? kBlockTypeShb : detail::rd32(blk, le);
            const std::uint32_t total = detail::rd32(blk + 4, le);
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
            out.push_back(BlockRef{pos, total, type, detail::classify_block_type(type), le});
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

// Detects pcap vs pcapng from the leading magic, sets endianness, walks the length chain over the whole
// buffer. `BlockRef.file_offset` is relative to the start of `file` (so parse_* can index `file` directly).
inline bool scan_blocks(Bytes file, std::vector<BlockRef>& out, std::string& error) {
    out.clear();
    ScanState st;
    std::size_t consumed = 0;
    return scan_window(st, file, out, consumed, /*at_eof=*/true, error);
}

// ---- Phase B: pure per-block parse (parallelizable; device-callable). ----
inline NANOTINS_HD bool parse_shb(Bytes file, const BlockRef& ref, ShbView& out) noexcept {
    if (ref.kind != Kind::Shb || ref.file_offset + ref.length > file.size() || ref.length < 28) {
        return false;
    }
    const std::uint8_t* body = file.data() + ref.file_offset + 8;  // skip type+total_len
    out.major = detail::rd16(body + 4, ref.little_endian);
    out.minor = detail::rd16(body + 6, ref.little_endian);
    out.section_length = static_cast<std::int64_t>(detail::rd64(body + 8, ref.little_endian));
    const std::uint32_t opt_off = 8U + 16U;  // type+len + (bom+major+minor+section_length)
    out.options.data = file.data() + ref.file_offset + opt_off;
    out.options.size = ref.length - opt_off - 4U;  // minus trailing total_len
    out.options.little_endian = ref.little_endian;
    return true;
}

inline NANOTINS_HD bool parse_idb(Bytes file, const BlockRef& ref, IdbView& out) noexcept {
    if (ref.kind != Kind::Idb || ref.file_offset + ref.length > file.size()) {
        return false;
    }
    const std::uint8_t* base = file.data() + ref.file_offset;

    // Synthetic IDB over a legacy pcap global header.
    if (ref.length == 24 && detail::is_pcap_magic(detail::rd32(base, true))) {
        const std::uint32_t magic = detail::rd32(base, true);
        const bool nanos = (magic == detail::kPcapMagicNanosLE || magic == detail::kPcapMagicNanosBE);
        out.link_type = static_cast<std::uint16_t>(detail::rd32(base + 20, ref.little_endian));
        out.snaplen = detail::rd32(base + 16, ref.little_endian);
        out.ts_resol = nanos ? 0x09 : 0x06;
        out.options = Options{nullptr, 0, ref.little_endian};
        return true;
    }

    if (ref.length < 20) {
        return false;
    }
    const std::uint8_t* body = base + 8;
    out.link_type = detail::rd16(body + 0, ref.little_endian);
    out.snaplen = detail::rd32(body + 4, ref.little_endian);
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

// Parses pcapng EPB and legacy pcap records into a self-contained EpbView (epb_flags walked in).
inline NANOTINS_HD bool parse_epb(Bytes file, const BlockRef& ref, EpbView& out) noexcept {
    if (ref.file_offset + ref.length > file.size()) {
        return false;
    }
    const std::uint8_t* base = file.data() + ref.file_offset;

    if (ref.kind == Kind::PcapRecord) {
        if (ref.length < 16) {
            return false;
        }
        const std::uint32_t ts_sec = detail::rd32(base + 0, ref.little_endian);
        const std::uint32_t ts_frac = detail::rd32(base + 4, ref.little_endian);
        out.interface_id = 0;
        out.ts_raw = (std::uint64_t{ts_sec} << 32) | ts_frac;
        out.caplen = detail::rd32(base + 8, ref.little_endian);
        out.origlen = detail::rd32(base + 12, ref.little_endian);
        out.payload_file_offset = ref.file_offset + 16;
        out.epb_flags = 0;  // legacy pcap records have no options
        out.options = Options{nullptr, 0, ref.little_endian};
        return true;
    }

    if (ref.kind != Kind::Epb || ref.length < 32) {
        return false;
    }
    const std::uint8_t* body = base + 8;  // skip type+total_len
    out.interface_id = detail::rd32(body + 0, ref.little_endian);
    const std::uint32_t ts_high = detail::rd32(body + 4, ref.little_endian);
    const std::uint32_t ts_low = detail::rd32(body + 8, ref.little_endian);
    out.ts_raw = (std::uint64_t{ts_high} << 32) | ts_low;
    out.caplen = detail::rd32(body + 12, ref.little_endian);
    out.origlen = detail::rd32(body + 16, ref.little_endian);
    out.payload_file_offset = ref.file_offset + 8 + 20;  // frame(8) + EPB fixed fields(20)

    const std::uint32_t data_padded = detail::pad4(out.caplen);
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
            out.epb_flags = detail::rd32(opt.value, ref.little_endian);
        }
    }
    return true;
}

// Host convenience: loop parse_epb over a BlockRef[] into pre-sized SoA columns. The scheduler-agnostic
// bulk path (bulk_for_each over parse_epb) is the primary form; this is the simple reference loop.
inline bool parse_epbs_bulk(Bytes file, const BlockRef* epbs, std::size_t n, EpbColumns& out,
                            std::string& error) {
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
