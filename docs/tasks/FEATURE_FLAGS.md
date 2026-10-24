# Task T1: refuse manifests whose feature flags nanolance does not support

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 0, item T1. One session.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 100k:
- Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'` windows of at most ~120 lines.
  Big ones: `src/dataset/dataset.cpp`, `src/dataset/dataset_ops.cpp`, `src/table/dataset_transaction.cpp`,
  `src/table/manifest_writer.cpp`, `bindings/python/nanolance/lance/dataset.py` (3.5k+ lines).
- Cut output: `| tail -15` for pytest (add `-o addopts=`), `| tail -5` for `tools/pylance_suite.py`,
  `ninja 2>&1 | grep -E "error|warning:" | head`.

## 1. Goal

nanolance never checks a manifest's `reader_feature_flags` / `writer_feature_flags`. Measured
2026-10-19: a dataset whose manifest sets reader bit 9, 12 or 16, or only one of the paired bit-8
words, is read by nanolance (3 rows) and refused by pylance; a dataset with an unknown *writer* bit
accepts `write_dataset(mode="append")` in nanolance and is refused by pylance. A newer Lance's
fragment-tree dataset (bit 12) has an empty `fragments` list, so nanolance would read it as an empty
table. Do what Lance 12.0.0 does: refuse to read, and refuse to write, what this build does not
understand.

## 2. Facts (measured on pylance 12.0.0 and lance-table 12.0.0 -- do not re-derive)

Lance 12.0.0 `rust/lance-table/src/feature_flags.rs`
(`/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/lance-table-12.0.0/src/feature_flags.rs`):
- Known bits: 0 deletion files, 1 stable row ids, 2 v2 format (deprecated, harmless), 3 table config,
  4 base paths, 5 disable transaction file, 6 data overlays, 7 covered index metadata, 8 mixed data
  file versions. `FLAG_UNKNOWN = 1 << 9`. Supported mask: `(1 << 9) - 1` = 511. nanolance supports
  overlays (bit 6), so its mask is 511 too.
- `can_read_dataset(reader) = reader & ~supported == 0`; `can_write_dataset(writer) = writer &
  ~supported == 0`.
- `validate_paired_feature_flags`: bit 8 must be set in both words or in neither; otherwise the
  manifest is corrupt. It runs before both the read and the write check.
- Reads check only the reader word. A dataset with an unknown bit only in the writer word still opens
  and reads (measured: `count_rows()` = 3), and every write to it is refused.

pylance's errors (Python type | message; drop Lance's `, /home/runner/...rs:NN:MM` source suffix):
- open, unknown reader bit: `ValueError` | `Not supported: This dataset cannot be read by this version
  of Lance. Please upgrade Lance to read this dataset. Flags: 512` (the decimal reader word).
- open, bit 8 in one word only: `ValueError` | `Encountered corrupt file manifest: Manifest has only
  one of the mixed data-file-version reader and writer feature bits set, so its semantics are undefined`.
- append (`write_dataset(mode="append")`), unknown writer bit: `OSError` | `Not supported: This dataset
  cannot be written by this version of Lance. Please upgrade Lance to write to this dataset.\n Flags:
  2048` (the insert path's wording: "to write to", then a newline and a space before "Flags").
- every other write (delete, update, merge_insert, compaction, index builds, commits): `OSError` |
  `Not supported: This dataset cannot be written by this version of Lance. Please upgrade Lance to
  write this dataset. Flags: 2048`.

Patching a manifest's flags for tests (verified on pylance-written manifests): the file ends with a
16-byte footer `<Q pos><H major><H minor>LANC`; at `pos` is a `u32` length and the protobuf body.
Appending the field again wins (proto3 scalar, last value): reader flags are field 9 (tag byte `0x48`),
writer flags field 10 (tag `0x50`), each followed by the varint value. Rewrite the file as
`b[:pos] + u32(len(new)) + new + b[pos+4+n:]`. The working helper:
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/flags/patch_flags.py`
(copy it into the test; do not import from the scratchpad).

## 3. Where things are in nanolance

- Every manifest is decoded in one place: `load_manifest_source` in `src/table/manifest_reader.cpp` (about
  line 194, `pb::decode_manifest`). The paired-bit check and the reader check go there or right after,
  so the C++ library, the Python binding and lance-c all refuse.
- Flag constants: `generated/lance_minimal.pb.hpp` (about line 173, `kFlag*`) and
  `include/nanolance/overlays.hpp` (`kFlagDataOverlays`). Add the supported mask there as one constant.
- Writes: find the one function every commit's manifest goes through (`grep -n "write_manifest\|commit_next_version" src/*.cpp`).
  Check the writer word of the manifest the write was made from, and of the latest version when a
  commit rebases. `src/table/manifest_writer.cpp` (about line 433) copies the prior manifest's flags into
  the new one: that is how an unknown bit is carried forward today.
- Python error types: see how the binding turns a C++ error into `ValueError` or `OSError`
  (`grep -n "OSError\|ValueError" bindings/python/nanolance/lance/dataset.py | head -40`). Match pylance's
  type for each case above.

## 4. Tests you write

`bindings/python/tests/test_feature_flags.py` (style of `tests/test_stable_row_ids.py`,
`require_pylance()`). For each case, assert both engines give the same exception type and the same
message (nanolance's message must equal pylance's with the source suffix stripped):
1. reader bits 9, 10, 12, 16 (set in both words): open and `to_table()` refused;
2. bit 8 in the reader word only, and in the writer word only: refused as corrupt;
3. writer bit 11 only: `count_rows()` and `to_table()` work and match; `write_dataset(mode="append")`,
   `delete`, `update`, `merge_insert`, `optimize.compact_files`, `create_scalar_index` all refused with
   the right message, and the dataset's version does not change;
4. regression: datasets with each known bit that pylance writes (deletions, stable row ids, table
   config, base paths, overlays) still open and write.
Also one C++ test (ctest, label `smoke`) that opens a patched dataset and checks the error text.

## 5. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures.
C++ without warnings (CI uses `-Werror`). Datasets nanolance writes today must not change (their flags
are all within the mask). Do not commit, push, change branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/t1/`. Builds:
`cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Run the full suites once, at the end, one at a time; never two at once; no sleep loops; kill by PID.

## 6. Stop and ask (question first in the report)

- a nanolance write path sets a flag outside the mask, or a test dataset nanolance writes would be
  refused by the new check;
- refusing at `load_manifest_source` breaks something that must read such a manifest (listing
  versions, cleanup of old versions): report which and why, do not work around it silently;
- the Python error type cannot match pylance's without changing the binding's general error mapping.

## 7. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; `python3 -m pytest -o addopts= -q
bindings/python/tests/test_feature_flags.py | tail -2`; `python3 tools/pylance_suite.py | tail -4`
("all N expected tests passed"); last, once: `cd bindings/python && python3 -m pytest -o addopts= -q -x
-p no:cacheprovider tests | tail -3`. Report under 40 lines: questions first, then files changed,
per-test status, tails, decisions.

**Outcome (2026-10-19):** done in one Haiku session; reviewed. Reads refuse a reader word outside bits 0-8
and a half-set bit 8 at the one manifest decode point (`load_manifest_source`), so the C++ library, the
binding and lance-c all refuse; every write path refuses an unsupported writer word before it writes
anything (the append writer with pylance's insert wording, every other commit with the commit wording).
Drop keeps accepting such a dataset as a root, as Lance's drop does. Reviewer's change: index builds
(`create_scalar_index`, `create_index`) raise `NotImplementedError`, as pylance does (measured), where the
session had left an xfail; `add_columns` and `drop_columns` added to the tests (`OSError`). Not compared
with pylance: whether a refused overwrite leaves an unreferenced data file behind.
