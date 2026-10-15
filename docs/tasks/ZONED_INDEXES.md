# Task: ZONEMAP and BLOOMFILTER scalar indexes (build, search, maintain)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap order item 5.

## 0. Context budget (read this first)

About 100k tokens per session:
- Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'` windows of at most ~120 lines.
  Big ones: `src/scalar_index.cpp` (~1.1k), `src/index_search.cpp` (~800), `src/index_optimize.cpp`,
  `bindings/python/nanolance/lance/dataset.py` (3.5k+), and Lance's own Rust (below, 3-5k lines each:
  read only the functions named here).
- Cut output: `| tail -15` for pytest (add `-o addopts=`; the config passes `-q`), `| tail -5` for
  `tools/pylance_suite.py`, `ninja 2>&1 | grep -E "error|warning:" | head`.
- Only your phase.

## 1. Goal

pylance builds two "zoned" scalar indexes nanolance refuses today (`ValueError: unsupported index type
'ZONEMAP'`): **ZONEMAP** (min / max / null count per zone of rows) and **BLOOMFILTER** (a Parquet-style
split-block bloom filter per zone). Both answer filters *inexactly*: they say which zones may hold
matching rows, and the scan re-checks those rows. Build them as Lance does (pylance must read and use
nanolance's), read and use pylance's, keep them across mutations as nanolance does for BTree / Bitmap.

## 2. Format facts (measured on pylance 12.0.0 here -- do not re-derive)

Lance's Rust source is unpacked under `.deps/rust-suite/`:
`lance-index-12.0.0/src/scalar/zoned.rs` (zone building, shared), `.../zonemap.rs`, `.../bloomfilter.rs`,
`lance-core-12.0.0/src/utils/bloomfilter/sbbf.rs` (the filter), `.../as_bytes.rs` (value -> bytes).

**Zones** (`zoned.rs`, `ZoneTrainer`): the column is scanned with `_rowaddr`, in fragment order; a zone
holds up to `rows_per_zone` rows and never crosses a fragment. `zone_start` = the first row's offset
in its fragment; `zone_length` = last offset - first offset + 1 (a span: deleted rows leave gaps).
Default `rows_per_zone` = 8192 (ZONEMAP) and `number_of_items` = 8192 (BLOOMFILTER).

**ZONEMAP**: one file `zonemap.lance` in the index directory. Columns, in order:
`min: <column type>, max: <column type>, null_count: uint32 not null, nan_count: uint32 not null,
fragment_id: uint64 not null, zone_start: uint64 not null, zone_length: uint64 not null`.
Schema metadata: `rows_per_zone` (decimal string), `null_bitmap` (decimal index of a **global buffer**
holding a serialized RowAddrTreeMap of the null rows -- nanolance already writes that serialization:
`serialize_treemap` in `src/scalar_index.cpp`). Manifest: `details_type_url` =
`/lance.table.ZoneMapIndexDetails`, empty details message; `index_version` 0. Example (10000 int64 rows,
2 fragments of 6000 / 4000): two rows `{min:0,max:5999,null_count:0,nan_count:0,fragment_id:0,
zone_start:0,zone_length:6000}`, `{min:6000,max:9999,...,fragment_id:1,zone_length:4000}`; file 1798 bytes.

**BLOOMFILTER**: one file `bloomfilter.lance`. Columns: `fragment_id: uint64 not null, zone_start:
uint64 not null, zone_length: uint64 not null, has_null: bool not null, bloom_filter_data: binary not
null`. Schema metadata: `bloomfilter_item` (e.g. "8192"), `bloomfilter_probability` (e.g. "0.00057"),
`null_bitmap` (as above). Manifest `details_type_url` = `/lance.index.pb.BloomFilterIndexDetails`,
empty details. Filter = Parquet SBBF exactly (`sbbf.rs`): blocks of 8 little-endian u32 words; size
from `min_log2_bytes(ndv, fpp)` clamped to the min/max there (8192 items at 0.00057 -> 32768 bytes);
hash = XXH64(value bytes, seed 0) -- nanolance links zstd, whose `lib/common/xxhash.h` provides XXH64
(`.deps/_nanolance_zstd-src/lib/common/xxhash.h`; include it with `XXH_INLINE_ALL` in one .cpp, or
copy only what you need); block index = `((hash >> 32) * num_blocks) >> 32`; within the block, word i
gets bit `((u32)hash * SALT[i]) >> 27`. Value bytes per type: `as_bytes.rs` (little-endian fixed width;
strings / binary their bytes). Check your filter bit-for-bit against pylance's on the same column
before anything else (the test in section 5 does).

**When unsure, run the same calls on pylance and compare files** (`lance.file.LanceFileReader(path)
.read_all().to_table()` and `.metadata().schema.metadata`).

## 3. Where things are in nanolance

- Scalar index build: `src/scalar_index.cpp` (`ScalarIndexType`, BTree / Bitmap / LabelList writers,
  `serialize_treemap`), header `include/nanolance/scalar_index.hpp`; Python `create_scalar_index` in
  `bindings/python/nanolance/lance/dataset.py` (grep "unsupported index type").
- Search: `src/index_search.cpp`: `Search::predicate` tries each index of the field and returns a
  `RowSet` of rows that *may* pass (`covered` fragments, `rows` per fragment). Verify the scan
  re-applies the filter to those rows (it must, for inexact indexes -- check, do not assume) and that
  the plan line names the index (`used.push_back("ScalarIndexQuery: ...")`, pylance's wording).
- Stable row ids: nanolance's indexes hold row ids on such datasets (`AddressToRowId` in
  `include/nanolance/row_ids.hpp`); Lance's zoned indexes keep **addresses** with fragment ids -- check
  `test_zonemap_with_stable_row_ids_after_compaction` and copy pylance.
- Maintenance: `src/index_optimize.cpp` (optimize_indices), and how compaction / updates keep or
  narrow BTree indexes (`src/dataset_ops.cpp`, `include/nanolance/index_maintenance.hpp`). Zoned
  indexes: new fragments get new zones (append rows to the file, as Lance's update does).
- Distributed segments (`create_index_uncommitted` / `merge_existing_index_segments` /
  `commit_existing_index_segments`): see how BTree does it in `dataset.py` (grep "merge_existing").

## 4. Phases

- **Z1 -- ZONEMAP**: build, use in filters (`=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN`, `IS NULL`),
  read pylance's, maintenance (append / delete / update / compaction / optimize_indices), segments.
  Oracle: `tests/test_scalar_index.py` -- `test_zonemap_index`, `test_zonemap_zone_size`,
  `test_zonemap_with_stable_row_ids_after_compaction`, `test_zonemap_deletion_handling`,
  `test_zonemap_index_remapping`, `test_zonemap_fsl_column`, `test_vector_and_zonemap_on_fsl_column`,
  `test_zonemap_fragment_ids_parameter_validation`, `test_zonemap_segment_merge_and_commit_from_python`,
  the zonemap params of `test_scalar_index_types` and of the tests parametrized over
  `["ZONEMAP", "BLOOMFILTER"]` / `["BTREE", "BITMAP", "ZONEMAP"]` (grep the file for `ZONEMAP`).
- **Z2 -- BLOOMFILTER** (a fresh session): the same for `=`, `IN`, `IS NULL`.
  Oracle: `test_bloomfilter_index`, `test_bloomfilter_deletion_handling`,
  `test_bloomfilter_segment_merge_and_commit_from_python`, the bloomfilter params above.

Iterate with `cd /home/user/nanolance && python3 tools/pylance_suite.py -k "zonemap or ZONEMAP" 2>&1 |
grep -E "^FAILED|test suite"` (a `tests/test_datagen.py` collection error appears with `-k`; ignore).

## 5. Tests you write

`bindings/python/tests/test_zoned_indexes.py` (style of `tests/test_stable_row_ids.py`,
`require_pylance()`), each case on both engines: (a) index files of the same column built by each are
equal row for row, and the bloom filter bytes are **identical**; (b) nanolance-built index used by
pylance and vice versa: same rows and the plan names the index; (c) after append / delete / update /
compaction, same results on both.

## 6. Rules

`AGENTS.md` sections 5 and 7 (grep the headings). No skipped / weakened tests; no `except Exception`
around failures. C++ without warnings (CI uses `-Werror`). Datasets and indexes nanolance writes
today must not change. Do not commit, push, change branches, or run `--update`. Builds:
`cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.

## 7. Stop and ask (question first in the report)

- pylance's file and yours differ in a way you cannot explain from the Rust source;
- you would need to change how BTree / Bitmap / LabelList indexes are searched or kept;
- a test needs NGRAM, JSON or RTREE indexes (out of scope).

## 8. Done (per phase) and report

Tails of: `cd build && ctest -j8 | tail -3`; the phase's `-k` run; `python3 -m pytest -o addopts= -q
tests/test_zoned_indexes.py | tail -2`; `python3 tools/pylance_suite.py | tail -4` ("all N expected
tests passed"); last, once, quiet machine: the full `pytest -o addopts= -q -x -p no:cacheprovider tests
| tail -3`. Report under 40 lines: files, per-test status, tails, decisions, questions.
