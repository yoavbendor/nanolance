# base: threads, statistics, safety limits, small numerics and helpers

<!-- layer-deps:start -->
Layer 1 of 11 (1 is the bottom, 11 the top). Uses: none. Used by: [io](../io/README.md), [arrow](../arrow/README.md), [encoding](../encoding/README.md), [file](../file/README.md), [table](../table/README.md), [expr](../expr/README.md), [scan](../scan/README.md), [index](../index/README.md), [api](../api/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->

The lowest layer: what every other layer relies on and nothing in it knows about Lance files. It holds the
thread pool, the read-safety limits and overflow-checked arithmetic of the read path, the work counters
tests guard with, Unicode text by character, the Roaring bitmap codec, protobuf wire helpers, a small JSON
reader, float16 and 128-bit integers, float text, UUIDs and the library version. It uses no other layer,
only the standard library; the Lance format's own structures start in `io` and `encoding`.

## Using it

- **Threads** (`include/nanolance/parallel.hpp`): `parallel::for_each(n, task)` runs `task(i)` for every
  `i` on the calling thread and idle pool threads and returns when all are done. Nested calls are fine (the
  waiting caller works on its own tasks, so nesting cannot deadlock); an exception from a task is rethrown
  once all tasks have finished, the first in index order. `threads()` is `set_threads(n)`, else
  `NANOLANCE_THREADS`, else the CPUs in the process's affinity mask; `threads() == 1` runs a plain loop and
  starts no pool. The calling thread's read limits apply to every task. A layer above that keeps
  per-operation state registers it with `set_worker_scope` (the data-file reader does); do not register a
  second one. What is written never depends on the thread count.
- **Read safety** (`include/nanolance/read_safety.hpp`): everything the read path takes from a file (sizes,
  counts, offsets) is untrusted. Check declared sizes once per page or chunk with the checked helpers and the
  `ReadLimits` budget, load integers from bytes with the byte-assembly loads (never `reinterpret_cast` file
  bytes), and grow buffers in loops with `reserve_more`, not `reserve` (which is quadratic in a loop).
- **Work counters** (`include/nanolance/work_stats.hpp`): process-wide relaxed atomics for tests that check
  how a result was produced (bytes read, page windows). Not a stable API.
- **Buffer pool** (`include/nanolance/buffer_pool.hpp`): `take(bytes)` / `give(std::move(v))` reuse large
  decoded buffers across threads; bounded by `NANOLANCE_BUFFER_POOL_MB` (0 turns it off). A taken vector is
  empty: resize it before writing.
- **Unicode** (`src/base/unicode.hpp`, private): `to_lower` / `to_upper` (full mappings, final sigma),
  `fold` (simple case fold, for ILIKE and the regex `i` flag), the character classes a regex asks about,
  `nfc` / `nfd`, and `remove_semitic_marks`. Every function has an ASCII fast path. Invalid UTF-8 decodes as
  U+FFFD of one byte, so a scan always advances. The tables are generated: regenerate
  `src/base/unicode_tables.inc` with `tools/gen_unicode_tables.py`, never edit it.
- **Roaring bitmaps** (`include/nanolance/roaring_bitmap.hpp`): `decode` / `encode` for the portable 32-bit
  format (an index's `fragment_bitmap`), `decode_prefix` for a bitmap followed by more bytes, and
  `encode_treemap` / `decode_treemap` for Lance's 64-bit RoaringTreemap. Inputs to `encode*` must be
  ascending and distinct. Errors come back as `false` plus a message.
- **Small helpers**, all private headers under `src/base/` included by path (`#include "base/int128.hpp"`):
  `I128` and its overflow-checked operations (`int128.hpp`; MSVC gets a two-word class with the same
  semantics), `float16_bits` (`float16.hpp`, rounds to nearest even as numpy does), `float_text::to_text` /
  `parse` (`float_text.hpp`, what `std::to_chars` writes, for platforms without it), `proto_wire`
  (`proto_wire.hpp`, varints and a field reader for messages kept outside the generated types),
  `fts::json` (`fts_json.hpp`, a small JSON reader / `quote`), `SharedCache<T>` (`shared_cache.hpp`, 64
  entries, thread-safe), `new_uuid` (`uuid.hpp`), `bytes_of` (`bytes.hpp`).
- `boolpack::pack_lsb_first` / `unpack_lsb_first` (`include/nanolance/bool_bitpack.hpp`) convert between
  nanolance's one byte per bool and Lance's LSB-first bits; `nanolance::library_version()`
  (`include/nanolance/version.hpp`) is the configured version string.

## Modules

| module | what it holds | tests |
|---|---|---|
| `parallel` | the thread pool and `for_each` | `nano_lance_parallel` |
| `read_safety` | read limits, checked arithmetic, byte loads, `reserve_more` | `nano_lance_read_safety` |
| `work_stats` | process-wide work counters | `nano_lance_work_stats` |
| `buffer_pool` | reuse of large decoded buffers across threads | `nano_lance_buffer_pool` |
| `unicode` (+ `unicode_tables.inc`) | case mapping and folding, classes, NFC / NFD, Semitic marks | `nano_lance_unicode` |
| `roaring_bitmap` | portable Roaring bitmaps and RoaringTreemap | `nano_lance_roaring_bitmap` |
| `bool_bitpack` | LSB-first bool packing | `nano_lance_bool_bitpack` |
| `float16` | float to IEEE binary16 bits | `nano_lance_float16` |
| `int128` | 128-bit integers for decimals | `nano_lance_int128` |
| `float_text` | shortest round-trip float text | `nano_lance_float_text` |
| `proto_wire` | protobuf varints and a field reader | `nano_lance_proto_wire` |
| `fts_json` | a small JSON reader and `quote` | `nano_lance_fts_json` |
| `shared_cache` | a bounded cache of shared immutable objects | `nano_lance_shared_cache` |
| `uuid` | random version 4 UUIDs | `nano_lance_uuid` |
| `bytes` | a string's bytes as a vector | `nano_lance_bytes` |
| `version` | the library version string | `nano_lance_version` |

## Spec coverage

The Lance format specification at https://lance.org/format/ (as of Lance 12.0.0) has no page for this
layer: its rows name the reference each helper follows.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `base.unicode.case` | ref: Rust std `str::to_lowercase` / `to_uppercase` (as DataFusion's lower() / upper()), Python unicodedata tables | done | `src/base/unicode.cpp::to_lower`, `src/base/unicode.cpp::to_upper`, `src/base/unicode.cpp::fold` | `nano_lance_unicode`, `bindings/python/tests/test_unicode_filters.py::test_lower_upper_values_match_pylance`, `bindings/python/tests/test_unicode_filters.py::test_filter_matches_pylance` | unit+interop | Full mappings (ß -> SS, İ -> i̇), final sigma. Tables are Unicode 14.0.0 (the Python that generated them); characters added later map as Rust's newer tables may not. |
| `base.unicode.classes` | ref: Rust regex crate `\w`, `\d`, `\s` (Unicode classes) | done | `src/base/unicode.cpp::is_word`, `src/base/unicode.cpp::is_digit`, `src/base/unicode.cpp::is_space`, `src/base/unicode.cpp::is_alpha` | `nano_lance_unicode`, `bindings/python/tests/test_unicode_filters.py::test_filter_matches_pylance` | unit+interop | |
| `base.unicode.normalization` | ref: Unicode UAX #15 canonical normalization (NFC / NFD) | done | `src/base/unicode.cpp::nfc`, `src/base/unicode.cpp::nfd`, `src/base/unicode.cpp::remove_semitic_marks` | `nano_lance_unicode` | unit | Used by nanolance's multi-script full-text settings, which pylance does not have, so there is nothing to compare with. |
| `base.roaring.portable` | ref: [RoaringFormatSpec](https://github.com/RoaringBitmap/RoaringFormatSpec) (as `IndexMetadata.fragment_bitmap` uses it) | done | `src/base/roaring_bitmap.cpp::decode_prefix`, `src/base/roaring_bitmap.cpp::encode` | `nano_lance_roaring_bitmap`, `bindings/python/tests/test_index_preservation.py::test_compaction_without_reindex_narrows_coverage` | unit+interop | Reads array, bitmap and run containers with either cookie; writes array and bitmap containers only (run containers are optional for a writer). |
| `base.roaring.treemap` | ref: RoaringTreemap portable serialization (Rust `roaring::RoaringTreemap`) | done | `src/base/roaring_bitmap.cpp::encode_treemap`, `src/base/roaring_bitmap.cpp::decode_treemap` | `nano_lance_roaring_bitmap`, `bindings/python/tests/test_frag_reuse.py::test_pylance_reads_the_index_nanolance_wrote` | unit+interop | |
| `base.roaring.deletion_files` | ref: RoaringFormatSpec, as [deletion files](https://lance.org/format/table/layout/#deletion-files) (`.bin`) use it | n/a | | | - | Deletion files are parsed by the table layer's own reader (`src/table/deletion_vector.cpp`, `parse_roaring_bitmap`); see src/table/README.md. |
| `base.threads` | ref: README "Threads" (results and files do not depend on the thread count) | done | `src/base/parallel.cpp::for_each`, `src/base/parallel.cpp::threads`, `src/base/parallel.cpp::set_threads` | `nano_lance_parallel`, `bindings/python/tests/test_parallel.py::test_reads_agree_across_thread_counts`, `bindings/python/tests/test_parallel.py::test_written_files_do_not_depend_on_the_thread_count`, `bindings/python/tests/test_parallel.py::test_thread_count_api` | unit+interop | |
| `base.bool_bits` | [Encoding Strategy](https://lance.org/format/file/encoding/) (a bool column is 1 bit per value, LSB first) | done | `include/nanolance/bool_bitpack.hpp::pack_lsb_first`, `include/nanolance/bool_bitpack.hpp::unpack_lsb_first` | `nano_lance_bool_bitpack`, `bindings/python/tests/test_bool_bits.py::test_rust_written_bools`, `bindings/python/tests/test_bool_bits.py::test_sliced_bools_round_trip` | unit+interop | |
| `base.float16` | ref: IEEE 754 binary16, rounded to nearest even as numpy rounds | done | `src/base/float16.hpp::float16_bits` | `nano_lance_float16`, `bindings/python/tests/test_expr_types.py::test_cast_list_f16_equals_pylance`, `bindings/python/tests/test_expr_types.py::test_cast_list_f16_rounds_like_numpy` | unit+interop | Every finite half and every midpoint between two halves is checked. |
| `base.int128` | ref: Arrow Decimal128 / Decimal256 integer arithmetic (two's complement, truncating division) | done | `src/base/int128.hpp::Wide128`, `src/base/int128.hpp::i128_mul_overflow` | `nano_lance_int128`, `bindings/python/tests/test_expr_types.py::test_decimal_filters_equal_pylance` | unit+interop | |
| `base.float_text` | ref: C++ `std::to_chars` / `from_chars` (shortest round-trip digits) | done | `src/base/float_text.hpp::to_text`, `src/base/float_text.hpp::parse` | `nano_lance_float_text` | unit | |
| `base.proto_wire` | ref: [Protocol Buffers wire format](https://protobuf.dev/programming-guides/encoding/) | done | `src/base/proto_wire.hpp::put_varint`, `src/base/proto_wire.hpp::Reader` | `nano_lance_proto_wire` | unit | Groups (wire types 3 and 4) are refused; Lance's messages have none. |
| `base.json` | ref: RFC 8259 JSON (the subset index params and full-text queries use) | done | `src/base/fts_json.hpp::Reader`, `src/base/fts_json.hpp::quote` | `nano_lance_fts_json` | unit | Lenient where it does not matter: an unknown escape `\x` reads as `x`, a lone surrogate is written as is. |
| `base.uuid` | ref: RFC 4122 version 4 UUIDs | done | `src/base/uuid.cpp::new_uuid` | `nano_lance_uuid` | unit | |
| `base.version` | ref: nanolance's version (CMakeLists.txt `NANOLANCE_VERSION_*`) | done | `src/base/version.cpp::library_version` | `nano_lance_version` | unit | |

## Known gaps

Nothing in this layer is missing against what the other layers need. The Unicode tables are Unicode 14.0.0
(generated with the Python at hand); regenerating them with a newer Python follows newer Rust behavior for
characters added since, at the cost of a larger table. The Roaring writer never emits run containers: any
reader accepts that, though a pylance-written bitmap of long runs is smaller. The JSON reader is lenient on
malformed escapes, which only affects malformed index parameters.
