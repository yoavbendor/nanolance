# pcapng → Lance, step 1 — design

**Status:** design only (no code yet). Scope of step 1: parse legacy **pcap** packets and **pcapng**
`SHB` / `IDB` / `EPB` blocks (with options for all), and write one Lance row per packet using the
nanolance writer — with packet **payloads kept external** (referenced by URI + offset + size), not
copied into Lance.

This document fixes the **architecture and the parsing seam** so that the current in-tree parser can
be replaced, byte-for-byte, by the future **`nanotins`** library (modern C++ tiny pcap/ng parsing,
later CUDA-accelerated) with **no changes above the seam**.

---

## 1. Why this shape (the two hard constraints)

1. **Replaceable by `nanotins`.** All format knowledge lives behind one header (`pcap_blocks.hpp`,
   §4). The example depends only on that header's POD types and free functions. The in-tree CPU
   implementation is a *reference impl*; `nanotins` later provides a drop-in implementation of the
   same header (CPU and CUDA).
2. **CUDA-capable.** pcap/pcapng blocks are **length-chained** (block *N*'s length tells you where
   *N+1* starts), so finding boundaries is inherently sequential, but **parsing a block body, given
   its boundary, is a pure function of bytes** — embarrassingly parallel. The seam therefore splits
   into two phases:
   - **Phase A — scan** (sequential, cheap): walk the length chain, emit a flat array of
     `BlockRef{kind, file_offset, length, endianness}`. No body parsing, no allocation per block
     beyond the output array.
   - **Phase B — parse** (parallel / bulk): given `BlockRef[]`, fill **columnar (SoA)** output
     arrays. This is the form a CUDA bulk-lambda/kernel implements; the CPU reference impl just loops.

   SoA output (not arrays-of-structs) is deliberate: it is both the GPU-friendly layout *and* exactly
   what the nanolance writer consumes (one contiguous buffer per column → one Arrow array → one
   `nano_lance_write_batch`).

```
 file bytes (mmap)
        │
        ▼   Phase A: scan_blocks()         ── sequential, ~memcpy-cheap
   BlockRef[]  (kind, offset, len, le)
        │
        ├─ IDBs ─► interface table (tsresol, linktype, snaplen per interface_id)
        │
        ▼   Phase B: parse_epbs_bulk()      ── pure per-block → CPU loop today, CUDA lambda later
   columnar SoA: interface_id[], ts_raw[], caplen[], origlen[], payload_off[], opt_* []
        │
        ▼   assemble Arrow arrays → nanolance write_batch(--compress) → commit
   out.lance  (payloads stay external: payload_uri + payload_off + payload_size)
```

---

## 2. Formats handled in step 1

All multi-byte fields honor the section/file byte order. Lengths are padded to 32-bit boundaries.

### 2.1 Legacy pcap (`.pcap`)
- **Global header (24 B):** `magic`(u32; `0xa1b2c3d4`=µs, `0xa1b23c4d`=ns, byte-swapped variants set
  endianness), `version_major/minor`(u16), `thiszone`(i32), `sigfigs`(u32), `snaplen`(u32),
  `linktype`(u32). Models a single implicit interface (id 0).
- **Per-record header (16 B):** `ts_sec`(u32), `ts_frac`(u32; µs or ns per magic), `incl_len`(u32),
  `orig_len`(u32), then `incl_len` bytes of packet data. Next record starts right after.

### 2.2 pcapng (`.pcapng`)
Generic block frame: `[Block Type u32][Total Length u32][Body…][Total Length u32]` (the trailing
length enables backward walking; we use the leading one). Body length = total − 12, then 32-bit pad.
- **SHB** (type `0x0A0D0D0A`): `byte_order_magic`(u32 `0x1A2B3C4D` → endianness), `major`/`minor`(u16),
  `section_length`(i64), options. Options of interest: `shb_hardware`(2), `shb_os`(3), `shb_userappl`(4).
- **IDB** (type `0x00000001`): `link_type`(u16), `reserved`(u16), `snaplen`(u32), options. Options of
  interest: `if_name`(2), `if_description`(3), `if_speed`(8), **`if_tsresol`(9)** (1 byte; default
  `0x06`=µs; high bit selects power-of-two vs ten), `if_filter`(11), `if_os`(12). One IDB per
  interface, in order → `interface_id` = index.
- **EPB** (type `0x00000006`): `interface_id`(u32), `ts_high`(u32), `ts_low`(u32), `caplen`(u32),
  `origlen`(u32), `packet_data`(caplen B, padded), options. Options: `epb_flags`(2, u32),
  `epb_hash`(3, var), `epb_dropcount`(4, u64), `opt_comment`(1, var UTF-8).
- **Options frame** (all blocks): repeated `[code u16][length u16][value, 32-bit padded]`, terminated
  by `opt_endofopt`(code 0, len 0). `opt_comment`(1) is generic to every block.
- **Other block types** (SPB/NRB/ISB/custom): step 1 **classifies and skips** them (counted, not
  parsed). The schema is forward-compatible (see §6 non-goals).

`payload_file_offset` for an EPB = `block_offset + 12 (frame) + 20 (EPB fixed fields)`. For a pcap
record = `record_offset + 16`. This offset + `caplen` is the external payload reference.

---

## 3. Lance output (leverages the encodings we already built)

**One row per packet.** Payloads are **external**: the row stores *where* the bytes are, not the
bytes. This is the columns model we benchmarked (beats Parquet, Lance size-parity) and it lights up
every encoder we built.

### 3.1 `packets` table (proposed columns)
| column | type | typical encoding (auto via `--compress`) | notes |
|---|---|---|---|
| `interface_id` | uint32 | RLE / bitpack | usually 0; low-card |
| `ts_raw` | uint64 | bitpack | raw `(ts_high<<32)|ts_low`; interpret with `ts_resol` |
| `caplen` | uint32 | bitpack | |
| `origlen` | uint32 | bitpack | |
| `payload_uri` | string | **ConstantLayout** (1 file) / **dict-RLE** (per-minute files) | the per-minute-batch case we optimized |
| `payload_off` | uint64 | bitpack (monotonic) | byte offset of packet data in the source file |
| `payload_size` | uint32 | bitpack / RLE | == caplen (or constant snaplen) |
| `link_type` | uint16 | **ConstantLayout** | denormalized from IDB (constant per single-interface file) |
| `ts_resol` | uint8 | **ConstantLayout** | denormalized from IDB `if_tsresol` (so a row self-describes its time unit) |
| `comment` | string | **ConstantLayout** (usually empty) | `opt_comment` |
| `epb_flags` | uint32 | **ConstantLayout** (usually 0) | `epb_flags` |

Denormalizing the handful of interface attributes needed to *interpret* a packet (`link_type`,
`ts_resol`) onto each row costs ~0 bytes thanks to `ConstantLayout`, and makes each row
self-contained (no join needed to read it back). Recommended for step 1.

### 3.2 Section / interface metadata (the rest of the options)
SHB options (`shb_os`, `shb_userappl`, …) and richer IDB options (`if_name`, `if_speed`, `if_filter`,
…) are **not** per-packet. Recommended for step 1: store them as **dataset/schema-level key-value
metadata** (nanolance already round-trips field metadata; a small section/interface blob can ride
there) **or** a tiny sidecar `interfaces.json`. A normalized second Lance table is possible but
deferred — decision in §8.

### 3.3 Fetching a payload back
A reader resolves a row to bytes with `nano_lance_fetch_external_blob(payload_uri, payload_off,
payload_size, …)` — the existing nanolance reader API. The example will include a verification step
that fetches a sampled row and byte-compares against the source file.

---

## 4. The parsing seam (`pcap_blocks.hpp`) — proposed interface

This is the contract `nanotins` implements. **POD only, zero-copy, no allocation in Phase B, no
virtual dispatch, no globals** — every Phase-B function is a pure function of `(file_bytes,
BlockRef)`, which is what makes it a valid device lambda.

```cpp
// === Proposed; NOT yet implemented. Lives in examples/pcapng2lance/include/pcap_blocks.hpp ===
namespace pcapblocks {

struct ByteSpan { const std::uint8_t* data; std::size_t size; };   // → std::span in impl
enum class Kind : std::uint8_t { Shb, Idb, Epb, PcapRecord, SimplePacket, Other };

// Phase-A output: a flat, trivially-copyable boundary record (GPU-transferable).
struct BlockRef {
    std::uint64_t file_offset;   // start of the block/record in the file
    std::uint32_t length;        // total length incl. frame/padding
    std::uint32_t type_or_link;  // pcapng block type, or pcap linktype for PcapRecord
    Kind kind;
    bool little_endian;          // byte order for this block's fields
};

// Zero-copy option cursor (shared by all blocks). Iterated, never allocated.
struct Options { const std::uint8_t* data; std::uint32_t size; bool little_endian; };
struct Option  { std::uint16_t code; std::uint16_t length; const std::uint8_t* value; };
bool next_option(Options& cursor, Option& out) noexcept;          // false at opt_endofopt/end

// Typed POD views (offsets/spans into the file buffer; no copies).
struct ShbView { std::uint16_t major, minor; std::int64_t section_length; Options options; };
struct IdbView { std::uint16_t link_type; std::uint32_t snaplen; Options options; };
struct EpbView { std::uint32_t interface_id; std::uint64_t ts_raw; std::uint32_t caplen, origlen;
                 std::uint64_t payload_file_offset; Options options; };

// ---- Phase A: sequential boundary scan (cheap; the only inherently serial part) ----
// Detects pcap vs pcapng from the leading magic, sets endianness, walks the length chain.
bool scan_blocks(ByteSpan file, std::vector<BlockRef>& out, std::string& error);

// ---- Phase B: pure per-block parse (parallelizable) ----
bool parse_shb(ByteSpan file, const BlockRef&, ShbView&) noexcept;
bool parse_idb(ByteSpan file, const BlockRef&, IdbView&) noexcept;
bool parse_epb(ByteSpan file, const BlockRef&, EpbView&) noexcept;  // also handles PcapRecord

// ---- Phase B (bulk / CUDA form) — the primary path the example uses ----
// SoA output buffers, pre-sized to the EPB/record count. The CPU reference impl loops calling
// parse_epb; nanotins provides a CUDA impl (one thread per BlockRef) writing the same columns.
struct EpbColumns {
    std::uint32_t* interface_id;  std::uint64_t* ts_raw;
    std::uint32_t* caplen;        std::uint32_t* origlen;
    std::uint64_t* payload_off;   std::uint32_t* payload_size;
    // option-derived columns filled in the same pass (0/empty when absent):
    std::uint32_t* epb_flags;     /* comment handled via a separate offsets+data builder */
    std::size_t    count;
};
bool parse_epbs_bulk(ByteSpan file, const BlockRef* epbs, std::size_t n, EpbColumns& out,
                     std::string& error);

}  // namespace pcapblocks
```

**Swap point:** the example `#include`s `pcap_blocks.hpp` and links one implementation. Today:
`pcap_blocks_ref.cpp` (in-tree, CPU, ~a few hundred lines). Tomorrow: `find_package(nanotins)` /
link `nanotins::nanotins`, delete the reference impl, **nothing above the seam changes**. The header
is intentionally `std`-only and free-function based so a CUDA build can compile the same declarations.

### 4.1 CUDA-readiness checklist (constraints the seam must keep)
- Phase-B functions: `noexcept`, no allocation, no `std::string`/STL containers in the hot path, no
  global/static state, branch on `little_endian` (or pre-byteswap) — valid as `__device__` lambdas.
- Output is SoA, pre-sized from the Phase-A count → kernels write by index, no push_back.
- `BlockRef`/`EpbColumns` are trivially copyable (host↔device memcpy).
- Options parsing is bounded per block (no unbounded recursion); a thread can parse one block's
  options independently.
- Keep file bytes in one contiguous span (mmap or pinned host buffer) so a single H2D copy suffices.

---

## 5. Pipeline / module layout

```
examples/pcapng2lance/
  DESIGN.md                     ← this file
  include/pcap_blocks.hpp       ← the seam (POD views + scan/parse decls)   [step-1 next]
  src/pcap_blocks_ref.cpp       ← in-tree CPU reference impl of the seam     [step-1 next]
  src/pcapng2lance_main.cpp     ← driver: scan → interface table → bulk parse → nanolance write
  tests/…                       ← golden + interop tests (§7)
```
Driver stages (mirrors the "progressive tools" idea — can also be split into `parse_pcap_blocks`,
`pcap_to_lance`):
1. `mmap` the source file (single contiguous `ByteSpan`).
2. `scan_blocks` → `BlockRef[]`; tally kinds.
3. Build the interface table from IDBs (`interface_id → {link_type, snaplen, ts_resol}`).
4. Pre-size SoA columns to the EPB/record count; `parse_epbs_bulk`.
5. Fill `payload_uri` (= source URI, constant or per-file → dict-RLE), `link_type`/`ts_resol`
   (denormalized), `comment`/`epb_flags` from options.
6. Wrap SoA buffers as Arrow arrays → `nano_lance_writer_set_compression(true)` →
   `nano_lance_write_batch` → `commit`. Payload bytes are never read/copied.

The example links `nanolance` (writer). It uses **no** Lance Rust core.

---

## 6. Non-goals for step 1 (explicit)
- No L2/L3 decoding of packet *contents* (that is a later step, also behind nanotins / the bulk seam).
- No SPB/NRB/ISB/custom-block parsing (scanned + skipped, counted for completeness).
- No multi-section merging semantics beyond resetting interface table per SHB.
- No payload inlining mode (external-only in step 1; an inline mode can be added later).
- No CUDA implementation yet — only the seam shaped so it can be added without API change.

---

## 7. Testing & validation plan
- **Golden small files:** a hand-built `.pcap` and `.pcapng` (1 SHB, 1–2 IDB, a few EPBs with a
  representative option each). Assert scan kind/counts and parsed field values against known truth.
- **Cross-check:** compare a handful of fields (ts, caplen, interface_id) against `scapy`/`tshark`
  output for a real capture.
- **External-offset correctness:** for sampled rows, `nano_lance_fetch_external_blob` and byte-compare
  the returned payload to the source file at `payload_off..+payload_size`.
- **Lance interop:** read `out.lance` with Rust `lance` and verify the columns (reuses the existing
  interop harness).
- **Endianness:** include one byte-swapped `SHB`/pcap-magic fixture.
- **Seam stability:** the tests target `pcap_blocks.hpp` only, so they re-run unchanged against the
  future `nanotins` implementation (this is the acceptance test for the swap).

---

## 8. Decisions to confirm before coding step 1
1. **Section/interface metadata sink:** dataset-level key-value metadata (recommended, simplest) vs a
   separate normalized `interfaces` Lance table. *Recommendation: KV metadata for step 1.*
2. **Timestamp:** store raw `ts_raw` + denormalized `ts_resol` (recommended; defers/normalizes
   interpretation, keeps it bitpackable) vs convert to a fixed unit (e.g., ns `uint64`) at write time.
3. **`payload_uri` policy:** one URI for the whole input file (→ ConstantLayout) vs per-input-file
   when batching many files in one dataset (→ the dict-RLE run-length case). *Recommendation: support
   both; a single conversion run writes one constant URI, batch runs produce run-length URIs.*
4. **Multiple interfaces / linktypes:** denormalize `link_type`/`ts_resol` per row (recommended;
   ConstantLayout makes it free for the common single-interface file) vs require a join.
5. **`std::span` vs the `ByteSpan` shim:** use `std::span<const std::uint8_t>` directly (C++20, already
   our standard) unless the future CUDA path needs the POD shim. *Recommendation: `std::span` now.*
