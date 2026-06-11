# nanotins

A small, reusable **C++20** library for parsing **pcap/pcapng** and **L2/L3/L4** packets — sequentially or
in data-parallel bulk — built around one idea: the **overlay-able, self-describing wire struct**. You
describe a packet header *once* (a packed `be<>`/`le<>`/`bits<>` struct + one `BOOST_DESCRIBE_STRUCT`
line) and get, for free:

- a zero-copy **`overlay()`** on raw bytes (endianness & bit layout handled by the field types),
- a flattened **column list** (`columns_of<T>`, bitfields expanded to named columns),
- an **SoA** store (`soa<T>`) + a **nanoarrow** schema/array (`arrow_schema<T>()` / `to_arrow<T>()`).

That describe→SoA→Arrow nucleus is its own library now — **[`soatins`](../soatins)** (namespace `soatins`,
include prefix `soatins/`) — so the reflection trick can be vendored on its own (it depends only on
**nanoarrow + header-only boost**, and knows nothing about packets). `nanotins` builds on it, adding the
pcap/pcapng scanner, the L2/L3/L4 wire structs + layered decode, the gPTP extension, and the
scheduler-agnostic `bulk_for_each` (over **stdexec**). The CUDA (nvexec) executors are split off again into
**[`gputins`](../gputins)** so the GPU dependency is isolated. All three are **header-only** — the Phase-B
parsers are device-callable (`NANOTINS_HD`), so they live inline in the headers where device code can see
them. None of them know anything about Lance — they produce nanoarrow tables that any backend (Lance,
Parquet, Arrow IPC) can persist.

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
include/nanotins/            bulk: bulk.hpp (scheduler-agnostic bulk_for_each / serial_for_each)
                             pcap: pcap_blocks.hpp (scan_blocks / scan_window / parse_epb — header-only)
                             protocols: protocols.hpp, protocol_decode{,_bulk}.hpp, gptp.hpp (extension)
../gputins/include/gputins/  gpu.hpp, protocol_decode_gpu.hpp (CUDA/nvexec, behind NANOTINS_ENABLE_CUDA)
tests/                       protocols / pcap_blocks / bulk / gptp  (reflect / soa_scatter live in soatins)
docs/nanotins.html           the guide
```

## CMake targets

| Target | Kind | What |
|---|---|---|
| `soatins::core` | INTERFACE | reflection / overlay / SoA / arrow + endian / bits (the reusable nucleus) |
| `nanotins::pcap` | INTERFACE | pcap/pcapng block scanner + per-block parse (header-only) |
| `nanotins::protocols` | INTERFACE | L2/L3/L4 wire structs + serial/bulk decode + gPTP + bulk_for_each |
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
