# nanotins

A small, reusable **C++20** library for parsing **pcap/pcapng** and **L2/L3/L4** packets — sequentially or
in data-parallel bulk — built around one idea: the **overlay-able, self-describing wire struct**. You
describe a packet header *once* (a packed `be<>`/`le<>`/`bits<>` struct + one `BOOST_DESCRIBE_STRUCT`
line) and get, for free:

- a zero-copy **`overlay()`** on raw bytes (endianness & bit layout handled by the field types),
- a flattened **column list** (`columns_of<T>`, bitfields expanded to named columns),
- an **SoA** store (`soa<T>`) + a **nanoarrow** schema/array (`arrow_schema<T>()` / `to_arrow<T>()`).

It depends only on **nanoarrow + header-only boost** (plus **stdexec** for the scheduler-agnostic bulk
path). It knows nothing about Lance — it produces nanoarrow tables that any backend (Lance, Parquet, Arrow
IPC) can persist.

## Read this first

**[docs/nanotins.html](docs/nanotins.html)** — the teaching document: the ideas, the two-phase
parsing pipeline, the CPU/GPU bulk path, and a complete worked example of **extending the library with a
new protocol (gPTP / IEEE 802.1AS)**. Open it in a browser.

## Layout

```
include/nanotins/   core: reflect, bits, endian, fixed_string, column_traits, arrow_glue, bulk
                    pcap: pcap_blocks.hpp (scan_blocks / scan_window / parse_epb)
                    protocols: protocols.hpp, protocol_decode{,_bulk}.hpp, gptp.hpp (the extension example)
src/                pcap_blocks_ref.cpp (the compiled scanner/parser)
tests/              reflect / protocols / pcap_blocks / bulk / gptp unit tests
docs/nanotins.html  the guide
```

## CMake targets

| Target | Kind | What |
|---|---|---|
| `nanotins::core` | INTERFACE | reflection / overlay / SoA / arrow + endian / bits / bulk |
| `nanotins::pcap` | STATIC | pcap/pcapng block scanner + per-block parse |
| `nanotins::protocols` | INTERFACE | L2/L3/L4 wire structs + serial/bulk decode + gPTP |
| `nanotins` | INTERFACE | umbrella (core + pcap + protocols) |

```cmake
add_subdirectory(nanotins)
target_link_libraries(my_app  PRIVATE nanotins)        # whole stack
target_link_libraries(my_tool PRIVATE nanotins::core)  # just the reflection core
```

Consumed in-tree by [`examples/pcapng2lance`](../examples/pcapng2lance) (the reference pcapng→Lance
converter), whose test suite — including the `tshark` field-alignment cross-checks — is this library's
golden.
