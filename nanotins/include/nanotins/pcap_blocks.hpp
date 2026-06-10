#pragma once

// The parsing seam. The example depends ONLY on this header's POD types and free functions; the
// in-tree CPU reference implementation (pcap_blocks_ref.cpp) is swappable, byte-for-byte, by the
// future `nanotins` library (CPU and CUDA) with no changes above this seam.
//
// Two phases (DESIGN section 1):
//   Phase A  scan_blocks()       sequential boundary walk -> flat BlockRef[]
//   Phase B  parse_*/parse_..bulk pure per-block parse -> columnar SoA (CPU loop today, CUDA later)
//
// CUDA-readiness: Phase-B functions are noexcept, allocation-free, no STL/globals in the hot path,
// and operate on a single contiguous byte span — valid as __device__ lambdas.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pcapblocks {

using Bytes = std::span<const std::uint8_t>;  // POD (ptr+size), usable in CUDA device code

enum class Kind : std::uint8_t { Shb, Idb, Epb, PcapRecord, SimplePacket, Other };

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
// Advance to the next option; false at opt_endofopt or end of buffer.
bool next_option(Options& cursor, Option& out) noexcept;

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

// ---- Phase A: sequential boundary scan (the only inherently serial part). ----
// Detects pcap vs pcapng from the leading magic, sets endianness, walks the length chain.
// `BlockRef.file_offset` is relative to the start of `file` (so parse_* can index `file` directly).
bool scan_blocks(Bytes file, std::vector<BlockRef>& out, std::string& error);

// Streaming scan: walk complete blocks in one window of an endless capture without buffering the whole
// file. `st` carries format/endianness across windows (detected on the first call from the leading
// magic; per-section endianness updated at each SHB). Emits BlockRefs with WINDOW-relative file_offset;
// `consumed` is set to the bytes covered by complete blocks (slide the window by it). A trailing partial
// block means "need more bytes" unless `at_eof`, where it is a truncation error. `out` is appended to.
struct ScanState {
    bool started = false;
    bool is_pcapng = false;
    bool little_endian = true;
    std::uint32_t pcap_link_type = 0;  // carried so PcapRecord refs report the link type
};
bool scan_window(ScanState& st, Bytes window, std::vector<BlockRef>& out, std::size_t& consumed, bool at_eof,
                 std::string& error);

// ---- Phase B: pure per-block parse (parallelizable). ----
bool parse_shb(Bytes file, const BlockRef& ref, ShbView& out) noexcept;
bool parse_idb(Bytes file, const BlockRef& ref, IdbView& out) noexcept;
bool parse_epb(Bytes file, const BlockRef& ref, EpbView& out) noexcept;  // also handles PcapRecord

// ---- Phase B (bulk / CUDA form) — the primary path the example uses. ----
// SoA output buffers, pre-sized to the EPB/record count. The CPU reference impl loops calling
// parse_epb; nanotins later provides a CUDA impl (one thread per BlockRef) writing the same columns.
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
bool parse_epbs_bulk(Bytes file, const BlockRef* epbs, std::size_t n, EpbColumns& out, std::string& error);

// pcapng block type constants (host order, after endianness resolution).
inline constexpr std::uint32_t kBlockTypeShb = 0x0A0D0D0AU;
inline constexpr std::uint32_t kBlockTypeIdb = 0x00000001U;
inline constexpr std::uint32_t kBlockTypeEpb = 0x00000006U;
inline constexpr std::uint32_t kBlockTypeSpb = 0x00000003U;

}  // namespace pcapblocks
