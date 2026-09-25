# Seed corpus — deletion-file parsers

Checked-in inputs for `nanolance_fuzz_deletion_vector`. CI passes this directory to libFuzzer, so
every one of these is replayed on every push; a regression that reintroduces one of these bugs fails
the `Memory safety` job rather than waiting for the fuzzer to rediscover it.

| file | what it caught |
|---|---|
| `oom-arrow-ipc-declared-size`, `oom-arrow-ipc-declared-size-2` | An Arrow IPC buffer's int64 uncompressed-length prefix was trusted up to the reader's generic 8 GiB ceiling, so a few hundred bytes bought a 4 GiB zeroed allocation. `materialize_ipc_buffer` now cross-checks it against `ZSTD_getFrameContentSize` before allocating. |
| `crash-arrow-ipc-flatbuffer-field-at-end` | **Heap-buffer-overflow read.** `flatbuffer_field` returned an offset equal to `bytes.size()` as *present*, and the caller indexed it — one byte past the end of the buffer. A present flatbuffer scalar is at least one byte, so the offset must be strictly inside; single-byte reads now go through a bounds-checked `flatbuffer_byte`. |
| `crash-arrow-ipc-row-count-overflow` | The record batch's int64 row count was validated with `values.size() < rows * 4`, which **overflows** uint64 past 2^62 — the product wraps small, the check passes, and the following `resize` throws `std::length_error` out of a function whose contract is to return `false`. An uncaught exception from a malformed file is a crash, not a refusal. Now uses `checked_mul`. |

These are libFuzzer's own minimized reproducers, kept as found. Add to this directory whenever a
fuzzer finds something — the input is the most durable form the finding has.
