# pcapng → Lance (+ `nanotins` reflection core)

A worked example that converts legacy **pcap** and **pcapng** captures into a Lance dataset — one row
per packet, with packet **payloads kept external** (referenced by `uri` + offset + size, never
copied). It doubles as the test harness/golden for the future **`nanotins`** library.

See [`DESIGN.md`](DESIGN.md) (step-1 architecture + the parsing seam), [`NANOTINS_REFLECTION.md`](NANOTINS_REFLECTION.md)
(struct → SoA → Arrow → Lance machinery), and [`KICKOFF.md`](KICKOFF.md) (build order + traps).

## What's built (M0 + M1 + M3)

- **M0 — `nanotins` reflection core** (`include/nanotins/`, header-only): `be<>`/`le<>` wire scalars,
  `bits<Word, field<…>…>` bitfields, `column_traits`, `columns_of<T>` (flattened column list),
  `soa<T>`/`store`, `arrow_schema<T>()` + `to_arrow()`. One `BOOST_DESCRIBE_STRUCT` line per row type
  drives SoA storage, an Arrow schema, and a Lance table. Proven bit-exact by `nanotins_roundtrip`.
- **M1 — the converter**: the parsing seam `include/pcap_blocks.hpp` + the in-tree CPU reference impl
  `src/pcap_blocks_ref.cpp` (Phase A scan → `BlockRef[]`, Phase B pure per-block parse → SoA), and the
  driver `src/pcapng2lance_main.cpp`. The seam is the contract a future `nanotins` (CPU + CUDA) drops
  into unchanged.
- **M3 — L2/L3 decode** (`--decode-l2l3`): `include/protocols.hpp` defines Ethernet / 802.1Q VLAN /
  IPv4 / IPv6 / TCP / UDP as `be<>`/`bits<>` packed structs (one `BOOST_DESCRIBE_STRUCT` each);
  `include/protocol_decode.hpp` walks each packet (Ethernet → VLAN* → IPv4/IPv6 → TCP/UDP, honoring
  `ihl`/`data_offset`); `include/pdu_table_writer.hpp` writes **one Lance table per PDU type**
  (`<stem>_ethernet.lance`, `_vlan`, `_ipv4`, `_ipv6`, `_tcp`, `_udp`), each row = `packet_id` + the
  reflected header fields (MAC/IP addresses as fixed-size-binary). Adding a protocol = a struct + a
  branch in the walk; a UDP-internal-PDU registry hook (dispatch on `dst_port`) is the next extension
  point. (M2 — extracting the core into the standalone `nanotins` lib + a CUDA `ex::bulk` path — is the
  remaining milestone; the seam and reflection core are already shaped for it.)

## Build & run

Built as part of the standalone nanolance build (`NANOLANCE_BUILD_EXAMPLES`, ON by default). The only
extra dependency is header-only `boost::describe`/`mp11`, fetched automatically.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pcapng2lance
build/examples/pcapng2lance/pcapng2lance capture.pcapng out.lance
```

Usage: `pcapng2lance [--no-compress] [--decode-l2l3] <input.pcap|pcapng> <output.lance> [payload_uri]`.
With `--decode-l2l3` it also emits `<stem>_<pdu>.lance` tables (Ethernet captures only; non-Ethernet
link types pass through as payload-only).
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

## Notes / known limitations

- The packet row is **all-scalar** (payload external), so M1 needs no variable-width SoA or prefix-sum
  — the simplest GPU-friendly path. Comments / extracted PDU byte slices (variable width) and L2/L3
  decoding (`be<>`/`bits<>` structs) are later steps (M2/M3 in `KICKOFF.md`).
- Write/read parity: nanolance now reads back every fixed-width type it writes — including narrow ints
  and `fixed_size_binary` — even with a blob column present (`nanotins_reader_parity`). The per-column
  value checks for the actual converter output still also run through stock pylance (`pcapng2lance_interop`).
  Still open: `bool` row fields (Arrow's 1-bit storage vs the byte-wide writer path) are forbidden for
  now, and stock-Lance interop of `fixed_size_binary` is unverified (nanolance round-trips it).
- Dataset KV metadata is attached to a scalar **field**, not the root struct (the schema mapper treats
  any root metadata as an extension marker, which breaks record-batch flattening).
