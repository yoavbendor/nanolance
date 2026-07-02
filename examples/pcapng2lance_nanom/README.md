# pcapng → Lance, on nanom

The nanom sibling of [`../pcapng2lance`](../pcapng2lance). It converts pcap / pcapng captures into a Lance
dataset — one row per packet, payloads kept **external** (a `lance.blob.v2` `payload_ref` of `uri` +
position + size, never copied) — but the entire **parse** side runs through
[**nanom**](https://github.com/yoavbendor/nanom), a single-header C++23 parser-combinator library, instead
of the nanotins reflection stack. The **write** side is unchanged: nanoarrow builds the record batch and
nanolance writes the fragment.

It exists so nanom's scan + parse + tabulate path can be benchmarked head-to-head against nanotins on the
**exact same L1 output** — and to show, concretely, that swapping the parser leaves the Lance dataset
byte-for-byte identical.

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

Usage: `pcapng2lance_nanom [--no-compress] [--no-write] <input.pcap|pcapng> <output.lance> [payload_uri]`

- `--no-compress` — write uncompressed columns (default: compressed).
- `--no-write` — scan + parse only, skip the Lance write (isolates nanom's Phase A/B for benchmarking).
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

**Doesn't (yet):** L2/L3/L4 PDU decoding (`--decode-l2l3`), staged enrichment (`--stage`), and windowed
streaming (`--window-bytes`). Those live in the nanotins example; nanom has the building blocks for them
(`nm_protocols.hpp` walks Eth→VLAN→IPv4/IPv6→TCP/UDP, and `<nanom/bulk.hpp>` is a data-parallel decode),
but porting the full multi-table pipeline is future work. The capture is read whole here (no windowing).

## Equivalence & benchmark

The interop test [`tests/test_pcapng2lance_nanom.py`](tests/test_pcapng2lance_nanom.py) reads the dataset
back with stock **pylance**, checks every external `(uri, position, size)` resolves to the exact source
bytes, and — when the nanotins `pcapng2lance` binary is also built — asserts the two datasets are
**byte-for-byte identical** (schema + every column value, including `payload_ref`) on pcap, pcapng, and
multi-section fixtures. It is registered as `pcapng2lance_nanom_interop` (CTest label `interop`; skips with
code 77 if pylance is absent).

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
