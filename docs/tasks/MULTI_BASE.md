# Task: multi-base datasets (`DatasetBasePath`, `initial_bases`, `target_bases`, `add_bases`)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap order item 3.

## 0. Context budget (read this first)

You have about 100k tokens for the whole session. Spend them on code, not output:

- **Never read a large file whole.** `grep -n` first, then `sed -n 'A,Bp'` windows of at most ~120
  lines. `src/scan/lance_table_reader.cpp` is 3k+ lines, `bindings/python/nanolance/lance/dataset.py` 3.5k+.
- **Always cut output**: `2>&1 | tail -15` (pytest), `| tail -5` (`tools/pylance_suite.py`),
  `ninja 2>&1 | grep -E "error|warning:|FAILED" | head -20`. pytest's config already passes `-q`, so
  for a summary line add `-o addopts=` (e.g. `python3 -m pytest -o addopts= -q tests/x.py | tail -2`).
- Read only the pylance tests named for your phase (`grep -n "def test_name" -A60 file`).
- Do only your phase. Finish it cleanly rather than half of two.

## 1. Goal

A Lance dataset can keep data files outside its own directory, in other "base paths" listed in the
manifest. pylance 12 creates them (`write_dataset(initial_bases=[DatasetBasePath(...)],
target_bases=[...] | target_all_bases=True)`), adds bases (`ds.add_bases`), and writes fragments into
them (`write_fragments(target_bases=)`, `merge_insert(...).execute(..., target_bases=)`). Today
nanolance cannot even **read** such a dataset (it looks for every data file under `<dataset>/data`
and fails with "data file is too small or unreadable"). Make nanolance read and write them as pylance
does, verified against pylance on the same calls.

## 2. Format facts (measured on pylance 12.0.0 in this container -- do not re-derive)

Protos: `.deps/rust-suite/lance-12.0.0/protos/table.proto`.

- `Manifest.base_paths = 18`: `repeated BasePath`, where
  `message BasePath { uint32 id = 1; optional string name = 2; bool is_dataset_root = 3; string path = 4; }`.
  Ids count from 1 in the order the bases are given (0 / absent means "the dataset itself").
- `DataFile.base_id = 7` (`optional uint32`); `DeletionFile.base_id = 7`; `IndexMetadata.base_id = 9`.
  Absent = the dataset's own directory.
- Where a file is: base absent -> `<dataset>/data/<path>`. Base with `is_dataset_root = false` ->
  `<base.path>/<path>` (directly in the base directory). Base with `is_dataset_root = true` ->
  `<base.path>/data/<path>`. Deletion files: same rule with `_deletions` instead of `data`
  (pylance itself writes deletions of base fragments to the primary `_deletions`, base_id unset --
  but honour a base_id if one is present). Blob sidecars sit next to their data file, so they follow it.
- Measured example: `write_dataset(t, primary, initial_bases=[DatasetBasePath(p1, name="path1"),
  DatasetBasePath(p2, name="path2", is_dataset_root=True)], target_all_bases=True, max_rows_per_file=100)`
  on 300 rows -> 3 fragments with `base_id` `None, 1, 2` (rotation: the primary first, then each base
  in order); files at `primary/data/x.lance`, `p1/y.lance`, `p2/data/z.lance`.
- `DatasetBasePath(path, name=None, is_dataset_root=False, id=None)`, attributes `id, is_dataset_root,
  name, path`, repr `DatasetBasePath(id=0, name=Some("n"), path=/x, is_dataset_root=false)`
  (`name=None` when unnamed). Exported as `lance.DatasetBasePath`.
- `DataFile.base_id` is already decoded/encoded by the Python transaction codec
  (`bindings/python/nanolance/lance/_transactions.py`, `_data_file` / `_encode_data_file`).
- C++: the generated codec (`generated/lance_minimal.pb.hpp`) keeps fields it does not know in
  `unknown` bytes (`Manifest::unknown`, `DataFile::unknown`, `DeletionFile::unknown`). That is how
  base_paths / base_id survive today. **Follow the pattern of `include/nanolance/row_ids.hpp`**
  (`read_fragment_row_meta` / `write_fragment_row_meta` read and replace fields 5..10 inside
  `DataFragment::unknown`) for reading `Manifest.unknown` field 18 and `DataFile.unknown` field 7. Do
  not regenerate the codec.

**When in doubt about behaviour, run the same call on real pylance (`import lance`) and copy it.**

## 3. Phases

### Phase A -- read (C++ + a little Python). Ends when nanolance reads pylance's multi-base datasets.

1. New header/source `include/nanolance/base_paths.hpp` + `src/table/base_paths.cpp` (add to the
   `nanolance` library in `CMakeLists.txt` next to `src/table/row_ids.cpp`): `struct BasePath { id, name,
   has_name, is_dataset_root, path }`; `read_base_paths(const pb::Manifest&, std::vector<BasePath>&, error)`;
   `write_base_paths(pb::Manifest&, const std::vector<BasePath>&)` (phase B needs it);
   `data_file_base_id(const pb::DataFile&, std::optional<uint32_t>&)` and a setter;
   the same for `pb::DeletionFile`; and the one resolver everything uses:
   `std::filesystem::path data_file_dir(dataset_path, manifest, data_file)` /
   `deletion_file_dir(...)` returning the directory per section 2 (an unknown base_id is an error).
2. Use the resolver at every read site. Find them with
   `grep -rn '/ "data"\|"_deletions"' src/*.cpp`: `src/scan/lance_table_reader.cpp` (the `PlannedFile`
   has a `data_dir` field already -- empty means `<dataset>/data`; set it from the resolver where
   PlannedFiles are built from manifest fragments), `src/dataset/dataset.cpp:~490`, `src/api/lance_c.cpp:~1373`,
   `src/table/deletion_vector.cpp` (read side, ~673). Also Python: `bindings/python/nanolance/lance/dataset.py:~1348`
   and `fragment.py:~408` join `data/` by hand -- give them the base directory too (expose a small
   binding or compute it from the manifest's base paths in Python, your choice; keep it to one helper).
3. `DatasetBasePath` class in Python (new small module or in `dataset.py`), exported from
   `nanolance.lance`; `LanceDataset` gains what pylance exposes for reading bases if a test needs it
   (check `dir(lance.LanceDataset)` for `base_paths`-like names; do not invent API).
4. Tests: new `bindings/python/tests/test_multi_base.py` (style of `tests/test_stable_row_ids.py`,
   `require_pylance()`): datasets **written by pylance** with each base kind (not root, root,
   target_all_bases, appends to a base, deletions of rows in base fragments, a blob column with
   packed / dedicated blobs in a base, a scalar index), read by nanolance: `to_table`, filters,
   `take`, `_take_rows`, `count_rows`, `take_blobs`, `data_files()[i].base_id` -- equal to pylance.
   Also a C++ unit test for base_paths encode/decode in `tests/` only if cheap.
5. Done: those tests pass; `cd build && ninja && ctest -j8 | tail -3` green; pylance suite
   `python3 tools/pylance_suite.py 2>&1 | tail -4` ends "all N expected tests passed".

### Phase B -- `write_dataset` and `add_bases`. Oracle: pylance's `TestMultiBase` and `TestAddBases`.

`.deps/pylance-tests/v12.0.0/tests/test_multi_base.py` classes `TestMultiBase` (lines 21-546) and
`TestAddBases` (547-1026). Run them with
`python3 tools/pylance_suite.py -k "TestMultiBase or TestAddBases" 2>&1 | grep -E "^FAILED|test suite"`.
Read each failing test's body (`sed -n`), measure pylance, implement:
- `write_dataset(initial_bases=, target_bases=, target_all_bases=)` in create / append / overwrite
  (overwrite inherits the bases; validation errors such as "not found", "Cannot register new bases in
  Append mode" -- copy pylance's messages); `storage_options` with `base_<id>.<key>` entries are
  accepted and ignored for local paths.
- The C++ writer must place each new fragment's file in its target directory and set `base_id`;
  the manifest writer must carry `base_paths`. Find where the writer names data files
  (`grep -n '"data"' src/dataset/write_session.cpp src/table/manifest_writer.cpp`) and add a per-fragment target
  directory + base id; plumb the bases list through `CommitExtras` (`include/nanolance/manifest_writer.hpp`)
  the way `stable_row_ids` was plumbed.
- `ds.add_bases([...], transaction_properties=?)`: a new version with the bases appended (Lance's
  `UpdateBases` operation; check how `_transactions.py` encodes operations and what field number
  UpdateBases has in `.deps/rust-suite/lance-12.0.0/protos/transaction.proto`).
- Mutations nanolance already supports (update, delete, compaction, merge_insert without
  target_bases) must keep working on a multi-base dataset: new files go to the primary unless a
  target is given (check pylance for compaction).

### Phase C -- fragment-level writes. Oracle: `TestWriteFragmentsWithTargetBases`,
`TestDataReplacementWithBases`, `TestMergeInsertMultiBase`, and in `test_blob.py`
`test_blob_extension_write_fragments_external_registered_base`, `test_blob_extension_take_blobs_multi_base`.
`write_fragments(target_bases=, initial_bases=)`, `DataFile.create(..., base_id=)` (today refused in
`fragment.py:~80`), Overwrite transactions with `initial_bases` (refused in `_transactions.py:~769` and
`src/table/dataset_transaction.cpp` `apply_operation`: `op.has_initial_bases`), `merge_insert` `target_bases`.
**Reviewer note for phase C:** phase A plans one directory per fragment (`fragment_data_dir` in
`src/scan/lance_table_reader.cpp` refuses a fragment whose files span bases). `TestDataReplacementWithBases`
adds a new column's file in a base to a fragment whose other files are in the primary: make the
reader resolve the directory **per data file** (`PlannedFile` / the per-file open path) instead of
per fragment -- that is the one planner change allowed here.

**Phase A outcome (2026-10-15):** done. Deletion files naming a base are refused (pylance never
writes them); `fragment_data_dir` per fragment as above.

## 4. Rules (read `AGENTS.md` sections 5 and 7 only -- grep the headings)

- A test may skip only for a missing optional dependency. Never weaken, skip or delete a test; never
  wrap a failure in `except Exception`.
- Match the surrounding code (comment density, naming, SPDX headers, `_errors.py` helpers
  `native()` / `unsupported()`). C++ builds without warnings (`-Wall -Wextra`; CI uses `-Werror`).
- Keep the diff to the task. Do not commit, push or change branches. Do not run
  `tools/pylance_suite.py --update` (the reviewer does).
- Builds:
  `cd /home/user/nanolance/build && ninja 2>&1 | grep -E "error|warning:" | head` and for Python
  `cd /home/user/nanolance/bindings/python/build/cp311-cp311-linux_x86_64 && ninja 2>&1 | grep -E "error:|warning:" | head && cp _nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`
  (needed after any C++ change before Python sees it).

## 5. Stop and ask (put the question first in your report) when

- a change would alter what nanolance writes for datasets **without** bases (byte-for-byte today);
- you need a public name not listed here or seen in pylance;
- the work spills into object stores / URIs (`s3://...` bases): local paths only;
- a fix needs redesigning the reader's file planning beyond setting `data_dir`.

## 6. Report (under 40 lines)

Files changed; per phase status; the tail lines of the done-commands; failures with error text;
decisions you made that this spec did not cover; questions.

**Phase B outcome (2026-10-15):** done. 21 of pylance's TestMultiBase / TestAddBases tests pass.
New writer calls `nano_lance_writer_set_initial_bases` / `set_fragment_targets` (fragments rotate
over the targets per write call), the UpdateBases operation (field 114), a private
`LanceDataset._ds.base_paths()` shim that pylance's own tests reach through. Overwrite with
`initial_bases` in a hand-built transaction is still refused (phase C). Reviewer fix: `add_bases`
raises its checks as ValueError, as pylance does.

**Phase C outcome (2026-10-15):** done but one test. `write_fragments(target_bases=)`,
`DataFile.create(base_id=)`, Overwrite transactions with `initial_bases`, DataReplacement into a base
(the reader now resolves a directory per data file when a fragment's files span bases),
`merge_insert(...).target_bases / target_all_bases`. Left (reviewer's call): external blob URIs
under a registered base -- they need the descriptor's blob_id to be the base id, which crosses the
external-blob writer's URI-dictionary packing; logged in ROADMAP.md.
