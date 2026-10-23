# Task: MemWAL setup -- initialize_mem_wal, the `__lance_mem_wal` index, sharding specs (ROADMAP item 12, phase M1)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). High-risk branch
`claude/nanolance-optimization-review-h3rxi7-highrisk` (it changes what nanolance writes into manifests).
M1 is the metadata half of MemWAL: 12 of the 20 pylance tests in `tests/test_mem_wal.py`. M2 (shard writers,
memtables, SSTables, the LSM scanners and planners: the other 8) is not part of this task: keep refusing it by
name.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'` windows
of at most ~120 lines. Big ones: `bindings/python/nanolance/lance/dataset.py` (5k+), `src/dataset/dataset.cpp`,
`src/dataset/dataset_ops.cpp`, the Lance sources below. Cut output: `| tail -15` for pytest (add `-o addopts=`).

## 1. Targets

`.deps/pylance-tests/v12.0.0/tests/test_mem_wal.py`:
`test_initialize_mem_wal_manual`, `_unsharded`, `_bucket_sharding`, `_bucket_sharding_without_primary_key`,
`_identity_sharding`, `_maintained_indexes`, `_writer_config_defaults`, `_no_writer_config_defaults_when_unset`,
`_rejects_conflicting_sharding`, `_rejects_partial_bucket`, `test_evaluate_sharding_spec_python_binding`,
`test_evaluate_sharding_spec_python_binding_column_parameter`, `test_mem_wal_index_details_none_before_init`
(13 ids; the module imports `lance.mem_wal` names the M2 tests use: `CompactedSsTable`, `LsmPointLookupPlanner`,
`LsmScanner`, `LsmVectorSearchPlanner`, `ShardSnapshot` must import, and raise NotSupportedError by name when used).

## 2. Sources (Lance 12.0.0, read-only copies -- never from memory)

`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/crates/` (re-download with
`curl -sSfL https://static.crates.io/crates/<name>/<name>-12.0.0.crate | tar -xz` if gone):
- `lance/lance-12.0.0/src/dataset/mem_wal/api.rs` -- `initialize_mem_wal` (~line 240: validation, the index entry,
  the commit), `mem_wal_index_details` (~line 590).
- `lance/lance-12.0.0/src/dataset/mem_wal/sharding.rs` -- `evaluate_sharding_spec`, the bucket hash (a Murmur3
  variant: tail bytes are mixed one by one, `(hash & i32::MAX) % num_buckets`, NULL hashes to the seed 0). Copy it
  exactly, every type it handles.
- `lance-table/lance-table-12.0.0/src/system_index/mem_wal.rs` -- `MEM_WAL_INDEX_NAME = "__lance_mem_wal"`, how the
  details are stored (an index entry whose `index_details` is a protobuf Any of `MemWalIndexDetails`; check its
  `fragment_bitmap` (none), `fields`, `dataset_version`, `index_version`).
- `lance-index/lance-index-12.0.0/protos/table.proto` line ~853 -- `MemWalIndexDetails`, `ShardingSpec`,
  `ShardingField`, and the writer-config map.
- pylance's Python API: `python3 -c "import lance,os;print(os.path.dirname(lance.__file__))"`, `dataset.py`
  `initialize_mem_wal` (line ~5474: the keyword arguments, and how the writer-config defaults are named, e.g.
  `max_wal_flush_interval_ms`) and `mem_wal.py` (the classes' constructors and their dict forms).

## 3. Measure first (with pylance 12.0.0)

Write probes in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/m1-work/`:
1. For each `initialize_mem_wal` form the targets use: pylance's manifest after the call (the transaction kind,
   the new index entry's every field, the Any's type_url and bytes), and `mem_wal_index_details()`'s dict.
2. What nanolance does today when it opens a dataset pylance initialized (list_indices, describe_indices, an append,
   a delete, compaction, optimize_indices, cleanup): every one must keep the `__lance_mem_wal` entry as pylance does
   (do the same operations with pylance and compare its manifests), and must not list it as a user index where
   pylance does not.
3. `evaluate_sharding_spec` outputs for every type in sharding.rs (pylance's results are the oracle; include NULLs,
   negative ints, floats with -0.0 and NaN, empty and non-ASCII strings, strings of length 1-9).

## 4. Implementation

- C++: the manifest index entry (write and read: wherever nanolance reads and rewrites index entries, a
  `__lance_mem_wal` entry is carried over untouched, as Lance carries it), the commit (the transaction pylance
  writes, measured in 3.1), the sharding hash (in C++ if the Python layer reaches it through `_nanolance`, else
  Python is fine for M1 -- say which and why).
- Python: `LanceDataset.initialize_mem_wal`, `mem_wal_index_details`, module `nanolance/lance/mem_wal.py`
  mirroring pylance's names (`ShardingField`, `ShardingSpec`, `evaluate_sharding_spec`, and the M2 classes as
  refusals), the error messages the targets match.
- Interop: pylance opens a dataset nanolance initialized and returns the same `mem_wal_index_details()`; nanolance
  opens one pylance initialized and returns pylance's dict.

## 5. Tests you write

`bindings/python/tests/test_mem_wal.py` (`require_pylance()`): every initialize form through both engines, details
equal; each engine reads the other's; the 3.2 operations on a pylance-initialized dataset keep the entry
(manifest-compared with pylance doing the same); `evaluate_sharding_spec` over the 3.3 inputs equal to pylance's.

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The local native
build has `-DNANOLANCE_WERROR=ON`: delete helpers you stop using. Do not add fields to Python classes that mirror
pylance's. Do not commit, push, change branches, or run `--update`. **A dataset without MemWAL must be written
exactly as before** (same manifest bytes for every operation). Builds: `cd build && ninja`, then
`cd bindings/python && python3 -m pip install -q -e .` (use `python3 -m pip`: bare `pip` here is another Python).
Run each full suite once, at the end; the bindings suite with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 7. Stop and ask (question first in the report)

- the `__lance_mem_wal` entry needs a manifest field or feature flag nanolance does not have;
- an operation of 3.2 cannot keep the entry without touching code M2 would own;
- a pylance test that passes today fails.

## 8. Done and report

Tails of: `cd build && ctest -j4 | tail -3`; your new tests; the targets through nanolance (`cd
.deps/pylance-tests/v12.0.0 && python3 -m pytest -o addopts= -q -p nanolance.lance._pytest_plugin
tests/test_mem_wal.py | tail -5`); `python3 tools/pylance_suite.py | tail -4` (add the passing targets to
`tests/pylance_suite/expected_pass.txt` by hand); last, once, the full bindings suite. Report under 40 lines:
questions first, then files, per-target status, tails, decisions.

**M1 outcome (2026-10-22): done.** Implemented by Haiku up to its usage limit, then finished and reviewed by
Opus on the high-risk branch. `initialize_mem_wal` (every form: manual, unsharded, bucket, identity, maintained
indexes, writer-config defaults, the refusals) commits Lance's `__lance_mem_wal` index entry as a CreateIndex
(`src/table/mem_wal.cpp`: no fields, no fragment bitmap, no files, index_version 0, the details in an Any);
`mem_wal_index_details` decodes it (`nanolance/lance/mem_wal.py`); `evaluate_sharding_spec` ports Lance's bucket
hash (its Murmur3 variant) in Python, equal to pylance's over every type sharding.rs handles. The entry is listed
and described as pylance lists it, survives append / delete / update / merge_insert / compaction /
optimize_indices / cleanup as pylance keeps it, and a second initialize is refused. 13 pylance targets pass
(expected list 1255); the 7 LSM tests (shard writers, memtables, SSTables, scanners and planners: M2) stay
refused by name. Fixed in review: manual initialize (all defaults) encodes empty details, which is valid; two of
the new tests compared against a dataset that had not been initialized, or expected a nested source column that
Lance itself refuses.
