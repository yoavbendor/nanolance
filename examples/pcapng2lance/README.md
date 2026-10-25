# pcapng → Lance

A converter from pcap / pcapng captures to Lance datasets, with the payloads kept **external**: one row per packet, and
each packet's bytes stay in the capture, referenced by a `lance.blob.v2` `payload_ref` (`uri`, `position`, `size`). It is
built on [nanom](https://github.com/yoavbendor/nanom), a header-only C++23 parser-combinator library, and on nanolance's
writer. The DPAR and LLDP rule tools (`dpar2lance`, `lldp2lance`) are built on the same parser.

## Build

nanom is a git submodule, pinned under `extern/nanom`. Clone with it, or fetch it:

```
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release     # from the repository root
cmake --build build --target pcapng2lance dpar2lance lldp2lance
```

Without the submodule, the library and tools still build; this example is skipped with a message.

## Usage

```
pcapng2lance capture.pcapng out.lance                       # the packet table
pcapng2lance --decode-l2l3 capture.pcapng out.lance         # plus the per-protocol tables
pcapng2lance --stage l1 capture.pcapng data/                # staged: the packet table in data/
pcapng2lance --stage l2 data/                               # then Ethernet / VLAN, l3 IPv4 / IPv6, l4 TCP / UDP
```

- `--no-compress` writes uncompressed columns (default: compressed). `--no-write` parses without writing.
- `--decode-l2l3` writes `<stem>_ethernet.lance`, `_vlan`, `_ipv4`, `_ipv4_option`, `_ipv6`, `_tcp`, `_udp`, the IPv6
  extension-header tables, and `<stem>_remainder_after_l4.lance` (the application payload as external references).
  Tables with no rows are not written.
- `--window-bytes N` (default 16 MiB) is the window the capture is read and committed in: each window is a run of whole
  blocks of at most N bytes, scanned and written on its own, one fragment of each table per window. Only one window is in
  memory at a time, so the converter's memory is bounded by the window, not by the capture's size.
- `--mem-bytes B` and `--read-tile-bytes B` size the staged stages' chunks and fetch tiles. Each chunk's rows are committed
  as one fragment of each table.
- `-d N` / `--drop N` and `-c N` / `--count N` write a slice: packets are numbered over the whole capture, and only those in
  `[N, N+count)` are written, so slices stitch into the full dataset.
- `--threads N` and `--sequential` are accepted; the converter reads the capture on one thread and its output does not depend
  on them.
- `--gpu` is a planned future feature and is reported as unsupported.

### Staged parsing

`--stage l1` writes the packet table into a data folder. `--stage l2`, `l3` and `l4` each decode one more layer of that folder,
reading the previous table and writing its own layer tables and `remainder_after_<stage>.lance`, the payload after that
layer, as an external reference. The original capture must still be reachable at the `uri` of the packet rows. Each stage
fetches each row's first 256 bytes from the capture in tiles, decodes what it needs, and commits each chunk as a fragment.

## DPAR and LLDP tools

`dpar2lance [--compress] <rules.txt> <input.pcap|pcapng> <outdir>` applies DPAR rules to a capture and writes one dataset per
parser table at `<outdir>/<parser>.lance`. The rule grammar is `<node>.<field> <op> <value> [&& ...] => <parser> <region> "<label>"`,
with the parsers `someip_tlv`, `raw_tlv`, `oddtlv` and `lldp`, and the regions `eth_payload`, `vlan_payload`, `udp_payload`,
`tcp_payload` and `someip_payload`. Each row carries the `rule_id` of the rule that produced it.

`lldp2lance` takes the same arguments and accepts the `lldp` parser only.

The engine is nanom's DPAR parity example (`include/nanom_dpar.hpp`, from nanom's `examples/nanotins_parity/dpar_lite.cpp`).

## Tests

| test | label | covers |
|---|---|---|
| `pcapng2lance_interop` | interop | stock **pylance** reads the dataset; stored offsets resolve to the source bytes |
| `pcapng2lance_realfile`, `_multisection` | interop | real captures (`tests/test_short.pcapng`, `tests/test_framed.pcapng`) cross-checked row by row against an independent Python pcapng walker |
| `pcapng2lance_parity_golden` | interop | every stage, the one-shot tables and the DPAR / LLDP tables match the nanotins reference (below) |
| `pcapng2lance_l2l3` | interop | `--decode-l2l3` on a crafted Ethernet capture, verified through pylance |
| `pcapng2lance_staged` | interop | `--stage l1` → `l4` on a data folder, per-stage tables and final remainder verified |
| `pcapng2lance_streaming`, `_multisection` | interop | tiny `--window-bytes` gives the same rows as the whole-file path, in several fragments |
| `pcapng2lance_enrich_chunking` | interop | tiny `--mem-bytes` / `--read-tile-bytes` give the same tables as one chunk, in several fragments |
| `pcapng2lance_frag_harmony` | interop | one-shot and staged PDU tables are identical on a fragmented capture |
| `pcapng2lance_stitch_*` | interop | slices stitched with `nlance_stitch` equal the full dataset |
| `nlance2table_*` | interop | `nlance2table` dumps match `tshark` field for field, where `tshark` is installed |
| `dpar2lance` / `lldp2lance` | — | exercised by `pcapng2lance_parity_golden` |
| `nano_lance_typed_writer`, `nano_lance_typed_blob_writer` | smoke | the nanolance writer facade the converter uses |

### Parity with the nanotins reference

The converter and tools were ported from nanotins, the parser library this example used before nanom. The reference they
had to match is stored as digests: `tests/golden/parity_digests.json` has one SHA-256 per table, over its column names, schema
metadata and rows in order, for the packet and staged tables of six captures, the one-shot tables of four, and the DPAR and
LLDP tables. `tests/golden/make_parity_digests.py` regenerates it from a build that still has the nanotins tools.

## Columns

The packet table (`packets.lance`): `packet_id`, `interface_id`, `ts_raw`, `caplen`, `origlen`, `link_type`, `ts_resol`
(the timestamp resolution, per row so a row is self-describing), `epb_flags`, and `payload_ref`, the external blob.
Section header and interface options are dataset metadata, on the first field, as `pcapng:shb<k>:<name>` and
`pcapng:if<j>:<name>`.

## Regenerating the golden digests

`tests/golden/parity_digests.json` is the reference the converter is checked against. It was produced by the nanotins
converters, which are no longer in the tree. To regenerate it (only needed if the reference itself changes):

```
git checkout 27f4ae7                # the last commit whose tree has the nanotins converters
git submodule update --init --recursive
cmake -S . -B build && cmake --build build --target pcapng2lance dpar2lance_nanotins lldp2lance_nanotins
python3 examples/pcapng2lance/tests/golden/make_parity_digests.py \
    build/examples/pcapng2lance/pcapng2lance \
    build/examples/pcapng2lance/dpar2lance_nanotins build/examples/pcapng2lance/lldp2lance_nanotins \
    . examples/pcapng2lance/extern/nanom/examples/nanotins_parity/testdata /tmp/golden-work
```

The nanotins submodule for that commit is `d52a4e2`. Copy the regenerated file back and check it into the
current branch. Routine runs do not need any of this: the test compares against the committed file.

## Memory

The converter reads the capture a window at a time (`--window-bytes`), and each stage reads its input table a batch at a
time (one data file of the dataset). Peak memory was measured on a 1M-packet classic pcap (200-byte packets, 205 MiB file):
about 66 MiB for the packet table with a 16 MiB window, and about 70 MiB with `--decode-l2l3` at the same window. Doubling the
capture size does not change the peak; the window does. A stage's batches are the table's fragments, which the window
keeps small.

## Limitations

- The SHB and IDB options recorded as metadata are the first window's, as the nanotins converter records them: a capture
  whose later sections begin after the first window does not get their options.
- IPv4 fragments: the TCP or UDP header is in the first fragment only, so a TCP/UDP row is produced only when
  `frag_offset == 0` (checked against `tshark` with reassembly off). Payload reassembly is not done.
- IPv6 extension headers are walked for the one-shot tables; the staged `l3` stage reads the IPv6 base header only.
- `oddtlv` is accepted by `dpar2lance`; the nanotins tool it was ported from rejected it, so it has no nanotins table to check
  against, only nanom's own golden.
