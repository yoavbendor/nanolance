# pcapng → Lance (nanotins reference converter)

A worked example that converts legacy **pcap** and **pcapng** captures into a Lance dataset — one row
per packet, with packet **payloads kept external** (referenced by `uri` + offset + size, never
copied). It demonstrates the full nanotins stack: the pcapng block scanner, the wire_spec declarative
wire-parsing core + spec_dag DAG dispatcher, windowed streaming, staged enrichment (L1 → L2 → L3 → L4),
and both CPU bulk (stdexec) and GPU paths.

See the docs/ folder for details: [`DESIGN.md`](docs/DESIGN.md) (architecture + parsing seam), 
[`NANOTINS_REFLECTION.md`](docs/NANOTINS_REFLECTION.md) (struct → SoA → Arrow → Lance machinery), 
and [`KICKOFF.md`](docs/KICKOFF.md) (build order + traps).

## What's built (M0 + M1 + M2 + M3/M6)

- **M0 — `soatins` reflection core** (now in the standalone [`soatins/`](../../extern/nanotins/soatins) library, vendored from the sister [nanotins](https://github.com/yoavbendor/nanotins) repo): `be<>`/`le<>` 
  wire scalars, `bits<Word, field<…>…>` bitfields, `column_traits`, `columns_of<T>` (flattened column list),
  `soa<T>`/`store`, `arrow_schema<T>()` + `to_arrow()`. One `BOOST_DESCRIBE_STRUCT` line per row type
  drives SoA storage, an Arrow schema, and a Lance table. Proven bit-exact by `nanotins_roundtrip`.
  
- **M1 — the converter**: the parsing seam `nanotins/pcap_blocks.hpp` (Phase A scan → `BlockRef[]`) + the
  driver `pcapng2lance_main.cpp`. Phase B (per-block parse → SoA) is now built on top of nanotins.

- **M2 — windowed streaming + bulk** (for endless / S3-backed captures): the driver never reads the whole
  file. It pulls bounded windows (`include/streaming_reader.hpp`), the seam's stateful `scan_window` walks
  the complete blocks in each window, and the **scheduler-agnostic bulk parse** runs over resident bytes →
  one Lance fragment per window, committed and freed before the next. Section/interface state and a global
  `packet_id` carry across windows; stored payload offsets are absolute (fetchable from S3 regardless of
  windowing). `--window-bytes` is the RAM/VRAM budget (default 512 MiB; a small file is one window).
  `--sequential` or `--threads N` selects the CPU path; `--gpu` (requires CUDA build) selects the GPU path.
  
- **M3/M6 — L2/L3/L4 decode via wire_spec + spec_dag** (`--decode-l2l3`): The **wire_spec** declarative
  core (nanotins `protocol_specs.hpp`) defines Ethernet / 802.1Q VLAN / IPv4 / IPv6 / TCP / UDP with explicit
  byte offsets; the **spec_dag** DAG/FSM (`spec_dag.hpp`) chains them together (Ethernet → VLAN* → IPv4/IPv6
  → TCP/UDP, honoring `ihl`/`data_offset`). One walk of the DAG (via `dag_decode.hpp`/`dag_bulk.hpp`) decodes
  both on host (CPU bulk via `dag_decode_bulk`) and on GPU (via `dag_decode_gpu`). Output: **one Lance table
  per PDU type** (`<stem>_ethernet.lance`, `_vlan`, `_ipv4`, `_ipv6`, `_tcp`, `_udp`), each row = `packet_id`
  + the reflected header fields. Also writes `<stem>_remainder_after_l4.lance` — the application payload after
  L4 as external blob.v2 refs. **The DAG-emitted PDU tables are byte-identical to the older hand-written decode**
  (verified by `test_pdu_table_interop`/`test_pdu_table_lance_interop`). Staged enrichment (`--stage l1→l2→l3→l4`)
  decodes one layer per stage via the per-layer `protocols::` decode (not the all-layers DAG), and its
  remainder is byte-identical to the one-shot path (guarded by `pcapng2lance_frag_harmony`).
- **Scheduler-agnostic bulk** (`nanotins/include/nanotins/bulk.hpp`): `bulk_for_each(sched, num_tasks,
  n, kernel)` is a partitioned stdexec `ex::schedule | ex::bulk` — the CPU path passes an
  `exec::static_thread_pool` scheduler; a CUDA build passes `nvexec::stream_context` and the SAME
  call runs on the GPU (the only difference, exactly as in `stdexec_gpu_experiment`). 
  
  The **L1 Phase-B parse** runs through it: a device-safe kernel (POD captures, no alloc) calls the pure
  `parse_epb` per `BlockRef` and scatters into the SoA columns. stdexec builds and runs on this MinGW
  host (verified), so the CPU bulk is real stdexec, not a stand-in.
  
  The **L2/L3/L4 decode** (`--decode-l2l3`) runs through the DAG via `dag_decode_bulk` (CPU) / 
  `dag_decode_gpu` (GPU), implementing the canonical variable-outputs-per-input pattern: two device-safe
  bulk passes bracket a prefix-sum — pass 1 `count_packet` per packet → exclusive scan per PDU type →
  size each output column exactly → pass 2 `scatter_packet` writes each PDU to its own prefix-summed slot
  (disjoint writes, no `push_back`). Both passes walk the one shared DAG traversal (so count == scatter by
  construction), and row order is packet order → byte-identical tables to the serial path
  (`test_pdu_table_interop` verifies; `pcapng2lance_frag_harmony` verifies staged vs. one-shot). On a
  CUDA host the scan becomes a `thrust::exclusive_scan` and the two kernels run on the GPU unchanged.
  
  **Sequential reference path** (`--sequential`): both the L1 parse and the L2/L3/L4 decode run through one
  policy seam (`nanotins::bulk_for_each` vs `nanotins::serial_for_each`), so `--sequential` swaps the whole
  Phase B to a plain in-thread loop — the readable/debuggable baseline and a byte-identical correctness
  oracle for the bulk path. `--threads N` sets the ex::bulk pool size (default = `hardware_concurrency`),
  for tuning on many-core hosts. Measured on this 8-core host (1.9 GB capture, single window): bulk and
  sequential are within noise (L1 ~0.8 s both; L1+L2/L3/L4 ~0.71 vs 0.73 s), plateauing by ~2–8 threads —
  the kernels are light and the pipeline is memory-bandwidth-bound, so CPU thread-parallelism barely
  helps. Sweep it yourself with [`bench/decode_bench.sh`](../../bench/decode_bench.sh)
  (`--threads 4,8,16,32,64,128,164`); pass `--no-write` (a driver flag that runs scan+parse+decode but
  skips the Lance output) to isolate Phase B from the I/O+write cost. Confirmed even with `--no-write`,
  Phase B is flat across thread counts — one thread already saturates memory-read bandwidth, so more
  threads only contend for it. The bulk path's real payoff is the **GPU** (swap the scheduler to `nvexec`;
  far higher memory bandwidth + latency hiding), not multicore CPU.

## Build & run

Built as part of the standalone nanolance build (`NANOLANCE_BUILD_EXAMPLES`, ON by default). The only
extra dependency is header-only `boost::describe`/`mp11`, fetched automatically.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pcapng2lance
build/examples/pcapng2lance/pcapng2lance capture.pcapng out.lance
```

Usage: `pcapng2lance [--no-compress] [--decode-l2l3] [--sequential|--threads N|--gpu] [--window-bytes N] <input.pcap|pcapng> <output.lance> [payload_uri]`.

- `--no-compress` — write uncompressed columns (default: compressed).
- `--decode-l2l3` — also decode L2/L3/L4 via the wire_spec + spec_dag core; emits `<stem>_<pdu>.lance` tables (Ethernet captures only; non-Ethernet link types pass through as payload-only).
- `--sequential` — run Phase B in-thread (reference/debug path); `--threads N` (default: hardware concurrency) or `--gpu` (requires CUDA build) select parallelism.
- `--window-bytes N` (default 512 MiB) — bounds the per-window RAM/VRAM budget; the capture is streamed in windows and written as one fragment per window, so memory stays bounded regardless of capture size.
- `--gpu` — run Phase B on the GPU (nvexec); also requires `--cuda-device D` (optional, default 0) and either `--vram-bytes B` or `--vram-pct P` (default 80% of free VRAM) to size the per-window VRAM budget.
- `--stage l1|l2|l3|l4` — staged enrichment (see "Staged / incremental parsing" below).
- `--mem-bytes B`, `--read-tile-bytes B` — enrich-stage chunking (see "Staged / incremental parsing" below).

### Staged / incremental parsing (`--stage`)

Decode one layer at a time, enriching the same data folder across separate runs — demonstrating that
parsing work can be done in parts and added to the dataset later, with the still-unparsed bytes kept as
a blob.v2 external reference into the *original* capture (never copied):

```
pcapng2lance --stage l1 capture.pcapng data/   # packets.lance + payload_ref (whole payload)
pcapng2lance --stage l2 data/                  # ethernet/vlan tables + remainder_after_l2
pcapng2lance --stage l3 data/                  # ipv4/ipv6 tables    + remainder_after_l3
pcapng2lance --stage l4 data/                  # tcp/udp tables      + remainder_after_l4 (= app payload)
```

Each enrich stage reads the previous stage's table, `nano_lance_fetch_external_blob`s each packet's
current remainder, decodes exactly one more layer, writes that layer's PDU tables, and writes the
advanced remainder (`packet_id` + next-layer discriminator + blob.v2 ref with `offset += header_len`).
Everything is joined by `packet_id`; the original capture must remain present. The final
`remainder_after_l4.lance` holds the application payloads as external references (packets fully consumed
by L4 have no remainder row).

The enrich stages are **memory-bounded and chunked, sized to the host they run on** (which may differ
from the L1 chunker — so the original capture's `uri` must be reachable, e.g. `s3://`). No forward scan
is needed: the table already gives each row's `(uri, off, size)`, so enrich is a pure random-access
bulk. Per chunk it (1) sizes `N = mem_budget / per_row_cost` from `--mem-bytes` (default: detected free
RAM × a fraction), (2) fetches the chunk's bytes in large contiguous tiles (`--read-tile-bytes`, default
32 MiB — far faster on S3 than many tiny ranged GETs) and **carves** each row's header prefix (256 B)
out of the resident tile, (3) bulk-decodes one layer, (4) appends a fragment to each table. Memory is
bounded by `read-tile + N × small`, independent of total rows and payload sizes.
The `payload_uri` defaults to a `file://` URI of the input; pass an explicit one (e.g. `s3://…`) when
the Lance dataset will be read elsewhere.

## Columns

Scalar columns flow through the `nanotins` reflection core from `PacketRow`
(`interface_id`, `ts_raw`, `caplen`, `origlen`, `link_type`, `ts_resol`, `epb_flags`); the payload
reference is a real `lance.blob.v2` external `payload_ref` struct (`data`=null, `uri`, `position`,
`size`). SHB/IDB options ride as dataset KV metadata (on a field — see note below). `link_type` and
`ts_resol` are denormalized per row so a row self-describes its time unit (≈free under ConstantLayout).

## Tests

| test | label | covers |
|---|---|---|
| `nanotins_reflect_smoke` | smoke | boost::describe → std adapter |
| `nanotins_roundtrip` | smoke | struct → soa → arrow → Lance → read back (all-scalar, `be<>/le<>`, `bits<>`) |
| `pcapng2lance_seam` | smoke | seam: pcap + pcapng (LE & byte-swapped), options, external offsets |
| `nanotins_reader_parity` | smoke | nanolance reads back **every** fixed-width type it writes (incl. `fixed_size_binary`) with a blob column present |
| `pcapng2lance_driver` | smoke | end-to-end run; manifest + KV metadata; `nano_lance_fetch_external_blob` premise |
| `pcapng2lance_interop` | interop | stock **pylance** reads the dataset; stored offsets resolve to source bytes |
| `pcapng2lance_realfile` | interop | converts a real committed capture (`tests/test_short.pcapng`, 274 pkts) and cross-checks every row against an independent Python pcapng walker |
| `pcapng2lance_realfile_multisection` | interop | same, on `tests/test_framed.pcapng` — 12 concatenated SHB sections (per-section interface reset) |
| `pcapng2lance_protocols` | smoke | L2/L3 wire structs overlay real bytes; bitfields + `columns_of` expansion |
| `pcapng2lance_l2l3` | interop | `--decode-l2l3` on a crafted Ethernet capture; per-PDU tables verified via stock lance |
| `pcapng2lance_staged` | interop | `--stage l1→l2→l3→l4` incremental enrichment; per-stage tables + final external remainder verified |
| `pcapng2lance_streaming` / `_multisection` | interop | tiny `--window-bytes` (refill/straddle/grow/multi-fragment) gives byte-identical output to the whole-file path |
| `pcapng2lance_enrich_chunking` | interop | tiny `--mem-bytes`/`--read-tile-bytes` (many chunks/fragments) enrich == single-chunk enrich for every PDU + remainder table |
| `nlance2table_smoke` | interop | `nlance2table` (top-level tool) dumps PDU + L1 tables to CSV/NDJSON: header, row counts, `--limit`, `fixed_size_binary` hex, nested-struct flatten |
| `nlance2table_tshark` | interop | per-PDU tables dumped via `nlance2table` match **tshark**'s dissection of the same pcapng field-for-field (eth/vlan/ipv4/ipv6/tcp/udp); skips if `tshark` absent |
| `nlance2table_tshark_realfile` | interop | real fragmented capture (`tests/SRL_front_left_51_short.pcapng`, 224 frames): L4 gated to first fragments (udp on 7 only) + eth/vlan/ipv4 fields match `tshark` (reassembly off); skips if `tshark` absent |
| `pcapng2lance_frag_harmony` | interop | one-shot (`--decode-l2l3`) and staged (`--stage l1→l4`) emit **identical** PDU tables **and `remainder_after_l4`** on the fragmented capture — the IPv4-fragmentation L4 gate + the L4 remainder match in both decode paths |

## Notes / known limitations

- The L1 packet row is **all-scalar** (payload external), so it needs no variable-width SoA or prefix-sum
  — the simplest GPU-friendly path. Variable-width fields (comments, extracted PDU byte slices, protocol
  payloads) remain future work. L2/L3/L4 protocol decoding is now built (via wire_spec + spec_dag),
  but advanced features like inline comments or UDP-internal-PDU registries are still planned.
- Write/read parity: nanolance now reads back every fixed-width type it writes — including narrow ints
  and `fixed_size_binary` — even with a blob column present (`nanotins_reader_parity`). The per-column
  value checks for the actual converter output still also run through stock pylance (`pcapng2lance_interop`).
  Still open: `bool` row fields (Arrow's 1-bit storage vs the byte-wide writer path) are forbidden for
  now, and stock-Lance interop of `fixed_size_binary` is unverified (nanolance round-trips it).
- Dataset KV metadata is attached to a scalar **field**, not the root struct (the schema mapper treats
  any root metadata as an extension marker, which breaks record-batch flattening).
- **IPv4 fragmentation**: the L4 (TCP/UDP) header lives only in the first fragment, so the decode emits a
  TCP/UDP row **only when `frag_offset == 0`** (verified against `tshark` with reassembly off on a real
  fragmented capture). Continuation fragments still appear in the `ipv4` table (every fragment carries
  `protocol`), so "how many packets belong to a UDP datagram" is the `ipv4.protocol==17` count, while the
  `udp` table holds the real headers only. Payload **reassembly** across fragments is not done. **Both
  decode paths apply this gate identically** — the one-shot `--decode-l2l3` walk and the staged
  `--stage l4` enrich produce byte-identical PDU tables on a fragmented capture (guarded by
  `pcapng2lance_frag_harmony`).
