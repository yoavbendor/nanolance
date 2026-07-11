# pcapng → Lance, on nanom

The nanom sibling of [`../pcapng2lance`](../pcapng2lance). It converts pcap / pcapng captures into a Lance
dataset — one row per packet, payloads kept **external** (a `lance.blob.v2` `payload_ref` of `uri` +
position + size, never copied) — but the entire **parse** side runs through
[**nanom**](https://github.com/yoavbendor/nanom), a single-header C++23 parser-combinator library, instead
of the nanotins reflection stack. The **write** side pairs it with nanolance's compile-time typed facade:
the per-PDU tables derive their Lance schema at compile time from each row struct's single
`NANOM_DESCRIBE` (see [`include/soa_lance_writer.hpp`](include/soa_lance_writer.hpp)) and write nanom's
columnar chunk buffers zero-copy — no nanoarrow builder, no per-cell appends. Only the L1 packet table
and the remainder table still build a nanoarrow batch, because their `lance.blob.v2` nested struct
column isn't expressible in the flat typed schema.

It exists to show, concretely, that nanom has **full network-parsing capability** — swapping the parser
leaves the Lance dataset byte-for-byte identical, at both L1 (the packet table) and L2/L3/L4 (the per-PDU
tables) — and so that nanom's scan + parse + decode path can be benchmarked head-to-head against nanotins
on the exact same output.

## Not a mandatory build

nanom is pulled in **only** by this example, as its own git submodule under
[`extern/nanom`](extern/nanom), and the example is **off by default**. A normal nanolance build — and the
default `pcapng2lance` example — never touch it:

```
git submodule update --init --recursive        # fetch extern/nanom (and extern/nanotins)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNANOLANCE_BUILD_PCAPNG2LANCE_NANOM=ON
cmake --build build --target pcapng2lance_nanom
build/examples/pcapng2lance_nanom/pcapng2lance_nanom capture.pcapng out.lance
```

`-DNANOLANCE_BUILD_PCAPNG2LANCE_NANOM=ON` is independent of `NANOLANCE_BUILD_EXAMPLES`; leaving it off (the
default) keeps the nanom submodule out of the build entirely.

Usage: `pcapng2lance_nanom [--no-compress] [--no-write] [--decode-l2l3] <input.pcap|pcapng> <output.lance> [payload_uri]`

- `--no-compress` — write uncompressed columns (default: compressed).
- `--no-write` — scan + parse only, skip the Lance write (isolates nanom's Phase A/B for benchmarking).
- `--decode-l2l3` — also decode L2/L3/L4 and emit one Lance table per PDU type (see below).
- `payload_uri` — the URI stored in each external `payload_ref`; defaults to a `file://` URI of the input
  (pass an explicit `s3://…` when the dataset will be read elsewhere).

## What it does (and doesn't)

**Does:** the L1 packet table, byte-identical to what `pcapng2lance` writes as `packets.lance` —

- **Phase A** (`nm_pcap.hpp::scan_blocks`): one nanom walk over the pcapng SHB/IDB/EPB/SPB blocks (or the
  classic pcap global header + records), picking each section's endianness at parse time with
  `nm::strct<T>(order)` rather than a parallel LE/BE reader set.
- **Phase B** (`nm_pcap.hpp::parse_epb` + a small option walk here): decode each packet's fixed fields and
  the two options `pcapng2lance` denormalizes — `if_tsresol` (→ `ts_resol`) and `epb_flags`.
- The eight scalar columns (`packet_id`, `interface_id`, `ts_raw`, `caplen`, `origlen`, `link_type`,
  `ts_resol`, `epb_flags`) + the external `payload_ref`, written as one Lance fragment.

Per-section interface state resets on each SHB, so section-relative `interface_id` denormalizes correctly
across concatenated sections — verified against the nanotins converter (below).

**Also does — `--decode-l2l3` (the full protocol walk):** one nanom `walk_packet_ext` traversal per packet
(`nm_protocols.hpp`: Ethernet → VLAN* → IPv4/IPv6 → TCP/UDP, honoring `ihl` / `data_offset` and gating L4
on `frag_offset == 0`), landing **one Lance table per PDU type** — `<stem>_ethernet.lance`, `_vlan`,
`_ipv4`, `_ipv6`, `_tcp`, `_udp` — each row = `packet_id` + the decoded header fields (nanom `ubits<>` bit
fields become integer columns; MAC/IP addresses become Arrow fixed-binary). It also writes
`<stem>_remainder_after_l4.lance`: the application payload after L4 as external `blob.v2` refs. These PDU
tables are **byte-for-byte identical** to the nanotins converter's (same schema, same values, every
protocol path — verified in the interop test). Each table is emitted with **zero per-type writer code**:
a generic `soa<Row>` → Lance writer ([`include/soa_lance_writer.hpp`](include/soa_lance_writer.hpp)) reads
the columns, names, Arrow types, and widths straight from the one `NANOM_DESCRIBE` on each row struct —
nanom's "schemas for free → Lance" path, realized end to end.

**IPv6 extension headers / SRv6.** `walk_packet_ext` (added to nanom's `nm_protocols.hpp`) descends the
full IPv6 extension-header chain — Hop-by-Hop (0), Routing / **SRv6 SRH** (43), Fragment (44), Destination
Options (60), Authentication Header (51) — to reach the real L4 header, and reports each ext header, each
SRv6 segment, and each IPv6/SRH TLV option (RFC 8200 length + padding semantics). That yields five more
Lance tables — `_ipv6_hopbyhop`, `_ipv6_destopt`, `_ipv6_routing`, `_ipv6_srh_segment` (one row per SRv6
segment, address as fixed-binary(16)), `_ipv6_option` — **all byte-for-byte identical** to nanotins on the
real `srv6_sample.pcap` (7 packets; eth/ipv6/hopbyhop/destopt/routing/srh_segment/option/tcp/udp tables and
the L4 reached through the SRH all match). `walk_packet_ext` is additive: nanom's original `walk_packet`
(which stops at the base IPv6 header, matching nanotins' JSON example) is unchanged, so nanom's own goldens
still hold.

The PDU tables are byte-identical to nanotins over Ethernet / VLAN / IPv4 (options honored via `ihl`) /
IPv6 base header / TCP / UDP — verified on crafted captures and the real `ipv4_options_sample.pcap`,
`SRL_front_left_51_short.pcapng`.

**Doesn't (yet):** gPTP / SOME/IP decode (extra nanotins tables), staged enrichment (`--stage`), and
windowed streaming (`--window-bytes`); the capture is read whole here. The IPv6 Fragment and Authentication
headers *are* descended to reach L4, but their fixed rows aren't tabulated (no fixture to verify against);
adding those tables is mechanical if needed.

## Equivalence & benchmark

The interop test [`tests/test_pcapng2lance_nanom.py`](tests/test_pcapng2lance_nanom.py) reads the datasets
back with stock **pylance**, checks every external `(uri, position, size)` resolves to the exact source
bytes, and — when the nanotins `pcapng2lance` binary is also built — asserts they are **byte-for-byte
identical** to the nanotins output: the L1 table on pcap / pcapng / multi-section fixtures, **and** every
`--decode-l2l3` PDU table (Ethernet / VLAN / IPv4 / IPv6 / TCP / UDP) on a mixed capture that exercises all
paths. It is registered as `pcapng2lance_nanom_interop` (CTest label `interop`; skips with code 77 if
pylance is absent).

[`bench/compare_bench.sh`](bench/compare_bench.sh) times both converters on the same capture and reports
packets/s, best of N runs, then confirms the outputs match:

```
examples/pcapng2lance_nanom/bench/compare_bench.sh --input capture.pcapng --repeat 5
examples/pcapng2lance_nanom/bench/compare_bench.sh --input capture.pcapng --no-write   # parse-only
```

Use a large capture for meaningful numbers: on a tiny file, fixed per-run overhead (thread-pool spin-up
and streaming setup in the nanotins driver, process start) dwarfs the actual parse work, so the wall-time
ratio there is not a parser comparison. `--no-write` removes the shared file-read + single-threaded Lance
write, which otherwise dominate, leaving nanom's scan+parse. See nanom's own
[`docs/NANOTINS_COMPARISON.md`](extern/nanom/docs/NANOTINS_COMPARISON.md) for the parse-only microbench
(overlay decode at parity, ~28–32 vs ~29–31 ns/pkt).
