# nanotins

A small, reusable **C++20** library for parsing **pcap/pcapng** and **L2/L3/L4** packets — sequentially or
in data-parallel bulk — built around one core idea: the **declarative wire-parsing struct spec**. Describe a
protocol header once with explicit byte offsets (`StructSpec<named_field<>, named_bytes_field<>, …>`)
and get, for free:

- a zero-copy **host read** overlay on raw bytes (endianness & bit layout handled by the field types),
- an equivalent **GPU device read** (device-callable, produces the same columns),
- a flattened **column list** (bitfields expanded to named scalar columns),
- an **SoA** store (`soa<T>`) + a **nanoarrow** schema/array (`arrow_schema<T>()` / `to_arrow<T>()`).

The **struct_spec** subsystem (files: `struct_spec.hpp`, `struct_spec_soa.hpp`) replaces hand-written
protocol overlays and gives the **spec_dag** (`spec_dag.hpp`, `dag_decode.hpp`) — a declarative DAG/FSM
dispatcher — a single source of truth. One walk of the DAG decodes on both host (CPU bulk path via
`dag_decode_bulk`) and device (GPU via `dag_decode_gpu`). The L2/L3 protocol specs live in
`protocol_specs.hpp`; PTPv2/gPTP extension specs in `protocol_specs_ptp.hpp`. All DAG-emitted PDU tables are
byte-identical to the older hand-written `walk_packet` decoder (verified by the
`test_pdu_table_interop`/`test_pdu_table_lance_interop` suite).

The soatins nucleus (describe→SoA→Arrow) is its own library — **[`soatins`](../soatins)** (namespace `soatins`,
include prefix `soatins/`) — vendorable on its own (depends only on **nanoarrow + header-only boost**,
knows nothing about packets). `nanotins` builds on it, adding pcap/pcapng scanning, the struct_spec + spec_dag
wire-parsing core, the gPTP extension, and scheduler-agnostic `bulk_for_each` (over **stdexec**). The CUDA
(nvexec) executors are split off again into **[`gputins`](../gputins)** so the GPU dependency is isolated.
All three are **header-only** — the Phase-B parsers are device-callable (`NANOTINS_HD`), so they live inline
in the headers where device code can see them. None of them know anything about Lance — they produce nanoarrow
tables that any backend (Lance, Parquet, Arrow IPC) can persist.

```
soatins  →  nanotins  →  gputins        (each depends only on the one to its left)
reflect     pcap + protocols + bulk      CUDA/nvexec executors (inert without NANOTINS_ENABLE_CUDA)
```

## Read this first

**[docs/nanotins.html](docs/nanotins.html)** — the teaching document: the ideas, the two-phase
parsing pipeline, the CPU/GPU bulk path, and a complete worked example of **extending the library with a
new protocol (gPTP / IEEE 802.1AS)**. Open it in a browser.

## Layout

```
../soatins/include/soatins/  reflect, bits, endian, fixed_string, column_traits, describe, arrow_glue
include/nanotins/            struct_spec: struct_spec.hpp, struct_spec_soa.hpp (declarative wire specs)
                             spec_dag: spec_dag.hpp, dag_decode.hpp, dag_bulk.hpp (DAG dispatcher + bulk)
                             protocols: protocol_specs.hpp, protocol_specs_ptp.hpp (L2/L3/L4 specs)
                             protocol_decode.hpp, protocol_decode_bulk.hpp (walk_packet legacy path)
                             bulk: bulk.hpp (scheduler-agnostic bulk_for_each / serial_for_each)
                             pcap: pcap_blocks.hpp (scan_blocks / scan_window / parse_epb — header-only)
                             gptp.hpp (PTPv2 extension, legacy overlay)
../gputins/include/gputins/  gpu.hpp, struct_spec_gpu.hpp, dag_decode_gpu.hpp (CUDA/nvexec, behind NANOTINS_ENABLE_CUDA)
tests/                       spec_dag / dag_decode / dag_bulk / struct_spec / protocol_specs
                             protocols / pcap_blocks / bulk / gptp  (reflect / soa_scatter live in soatins)
docs/nanotins.html           the guide
```

## CMake targets

| Target | Kind | What |
|---|---|---|
| `soatins::core` | INTERFACE | reflection / overlay / SoA / arrow + endian / bits (the reusable nucleus) |
| `nanotins::pcap` | INTERFACE | pcap/pcapng block scanner + per-block parse (header-only) |
| `nanotins::protocols` | INTERFACE | struct_spec + spec_dag core + L2/L3/L4 protocol specs + serial/bulk decode + gPTP + bulk_for_each |
| `nanotins` | INTERFACE | umbrella (pcap + protocols, pulls soatins) |
| `gputins` | INTERFACE | CUDA (nvexec) executors; pulls nanotins; inert without NANOTINS_ENABLE_CUDA |

```cmake
add_subdirectory(soatins)                              # the reflection nucleus, on its own…
target_link_libraries(my_tool PRIVATE soatins::core)   #   …for any describe→SoA→Arrow use
add_subdirectory(nanotins)                             # …or the whole packet stack
target_link_libraries(my_app  PRIVATE nanotins)
```

Consumed in-tree by [`examples/pcapng2lance`](../examples/pcapng2lance) (the reference pcapng→Lance
converter), whose test suite — including the `tshark` field-alignment cross-checks — is this library's
golden.
