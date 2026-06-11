# soatins

**Describe a struct once → get an SoA store and an Arrow table.** soatins is the reflection nucleus that
was split out of [`nanotins`](../nanotins): the genuinely reusable trick, with nothing packet-specific
about it. Put a packed struct of endianness-aware fields behind one `BOOST_DESCRIBE_STRUCT` line and you
get, for free:

- a zero-copy **`overlay<T>(bytes)`** view onto raw wire bytes (`be<>`/`le<>`/`bits<>` handle byte order
  and bit layout),
- a flattened **column list** — `columns_of<T>`, with `bits<>` bitfields expanded into named scalar
  columns,
- a **Struct-of-Arrays** store, `soa<T>` (`.store(i, row)`), plus a device-fillable view
  (`soa<T>::raw()` → `soa_ptrs<T>`, and a free `scatter(ptrs, i, row)`) for filling columns from a kernel,
- a **nanoarrow** `ArrowSchema`/`ArrowArray` — `arrow_schema<T>()` / `to_arrow<T>(soa)` — so any backend
  (Lance, Parquet, Arrow IPC) can persist it.

It depends only on **nanoarrow** and **header-only boost** (`describe` + `mp11`). No stdexec, no CUDA, no
Lance. Namespace `soatins`, include prefix `soatins/`.

```cpp
#include "soatins/bits.hpp"
#include "soatins/reflect.hpp"
#include "soatins/arrow_glue.hpp"
#include <boost/describe.hpp>

struct Sample {                       // a wire-shaped record
    soatins::be<std::uint32_t> id;    // big-endian on the wire, host order when read
    soatins::be<std::uint16_t> kind;
    std::uint8_t                flags;
};
BOOST_DESCRIBE_STRUCT(Sample, (), (id, kind, flags))   // the one line

soatins::soa<Sample> rows;
rows.store(0, soatins::overlay<Sample>(bytes));        // zero-copy read → columnar store
ArrowArray arr = soatins::to_arrow<Sample>(rows);      // hand to Lance / Parquet / IPC
```

## Layout

```
include/soatins/   endian, bits, fixed_string   field types (byte order + bit layout)
                   describe, column_traits       reflection: members → flattened columns
                   reflect                        soa<T>, soa_ptrs<T>, scatter, columns_of
                   arrow_glue                     arrow_schema<T>() / to_arrow<T>()
                   portability                    NANOTINS_HD (host/device annotation)
tests/             reflect_smoke, soa_scatter
```

## CMake

```cmake
add_subdirectory(soatins)
target_link_libraries(my_tool PRIVATE soatins::core)
```

Used by [`nanotins`](../nanotins) (the pcap/pcapng + L2/L3/L4 parser) and, through it, the
[`pcapng2lance`](../examples/pcapng2lance) reference converter.
