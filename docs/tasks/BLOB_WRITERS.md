# Task: pylance's prepared-layout blob writers (`PackedBlobWriter`, `DedicatedBlobWriter`, `BlobDescriptorArrayBuilder`)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap item
"packed/dedicated blob writer file API" (docs/ROADMAP.md, tier 2).

## 0. Context budget (read this first)

You have about 100k tokens of context for the whole job. Spend them on code, not on output:

- **Never read a large file whole.** Find the place with `grep -n`, then read a window of at most
  ~120 lines with `sed -n 'A,Bp' file`. Files here are 1-3k lines (`src/file/blob_v2_external.cpp`,
  `bindings/python/nanolance/lance/file.py`, `dataset.py` is 3k+).
- **Always cut command output**: `... 2>&1 | tail -15` for pytest, `| tail -5` for
  `tools/pylance_suite.py` (it prints thousands of lines otherwise), `ninja 2>&1 | grep -E
  "error|warning|FAILED" | head -20` for builds. On a failure, rerun only that one test with
  `-x -q 2>&1 | tail -40`.
- This file is the spec: do not re-derive what it states, and do not read pylance's test file
  beyond the tests named in section 2 (`grep -n "def test_name" -A60`).
- Work in phases; the reviewer may run each phase as a separate session. Finish one phase cleanly
  (tests green, short report) rather than half of two.

## 1. Goal, in one paragraph

pylance 12 lets a distributed job write blob bytes to Lance's sidecar files *itself* and get back
descriptors pointing at them, then write a data file whose blob column holds those descriptors and
commit it (`LanceOperation.DataReplacement` / `Append`). nanolance.lance must offer the same four
classes with the same behaviour, so that pylance's own tests for them pass against nanolance and a
dataset built this way is read identically by nanolance and by pylance.

## 2. Done means (acceptance criteria -- all of them)

1. These pylance tests pass against nanolance (they are in
   `.deps/pylance-tests/v12.0.0/tests/test_blob.py`):
   - `test_packed_blob_writer_scalar_buffer_inputs` (4 params)
   - `test_packed_blob_writer_bulk_arrow_array` (all params)
   - `test_packed_blob_writer_bulk_excludes_physical_null_bytes`
   - `test_packed_blob_writer_bulk_rejects_non_monotonic_offsets`
   - `test_packed_blob_writer_mixed_calls_preserve_legacy_finish_alignment`
   - `test_packed_blob_writer_bulk_rejects_non_binary_array`
   - `test_packed_blob_writer_bulk_binary_view`
   - `test_packed_blob_writer_bulk_fixed_size_binary`
   - `test_blob_descriptor_array_builder_writes_prepared_packed_blob_for_data_replacement`
     (needs phase B)
   - `test_failed_blob_writer_traceback_can_be_released_on_another_thread` if it is reachable
     (read it; if it depends on something unrelated and fails for that reason, report why).
2. No regression: `python3 tools/pylance_suite.py` ends with `all N expected tests passed`
   (it compares with `bindings/python/tests/pylance_suite/expected_pass.txt`). Then run
   `python3 tools/pylance_suite.py --update` once so the new passes are recorded.
3. A new differential test file `bindings/python/tests/test_blob_writers.py` (section 6) passes, and
   the whole Python suite still passes: `cd bindings/python && python3 -m pytest tests -q -x`.
4. C++ tests still pass: `cd build && ninja && ctest -j8` (only needed if you touch C++).
5. Docs updated (section 7). **Do not commit or push** -- the reviewer does that after review.

## 3. Exact behaviour to reproduce (measured on pylance 12.0.0 in this container)

Real pylance is installed as `lance` (`import lance`); nanolance's module is `nanolance.lance`.
**When in doubt, run the same call on real pylance and copy what it does.** These facts were measured:

### 3.1 Where the bytes go

- `LanceFileSession(base).open_packed_blob_writer(path, blob_id)` and
  `.open_dedicated_blob_writer(path, blob_id)`: `path` is a data file name such as `"abc.lance"`
  relative to the session's base directory. The sidecar is
  `<base>/<path without .lance>/<blob_id as 32 binary digits, bit-reversed>.blob`, e.g. blob_id 3
  -> `<base>/abc/11000000000000000000000000000000.blob`. nanolance already has this:
  `blob_v2_sidecar_path` in `include/nanolance/blob_v2_external.hpp` (C++); in Python it is
  `f"{int(f'{blob_id:032b}'[::-1], 2):032b}.blob"`.
- `writer.path` is that sidecar path **as a string without the leading `/`** (pylance prints
  `tmp/pb/abc/1100...0.blob` for base `/tmp/pb`). `writer.blob_id` is the id.
- Packed: blobs are appended back to back into the one sidecar. Null rows write nothing. Empty
  (non-null) blobs write nothing but get a descriptor of size 0 at the current offset.
- Dedicated: `write(data)` may be called several times; the bytes are concatenated into the sidecar.

### 3.2 `PackedBlobWriter`

- `write_blob(data)`: one blob. Accepts `bytes`, `bytearray`, `memoryview`, a list of ints 0..255.
  Errors (exact type; message need not be identical but keep the gist):
  `str` -> `TypeError("argument 'data': Can't extract `str` to `Vec`")`; `int` / `None` -> `TypeError`;
  `[300]` -> `OverflowError("out of range integral type conversion attempted")`. Returns `None`.
- `write_blobs(payloads)`: `pa.BinaryArray`, `LargeBinaryArray`, `BinaryViewArray`,
  `FixedSizeBinaryArray`, or a `pa.ChunkedArray` of those. One descriptor row per input row, in
  order, across chunks and calls. **Sliced arrays: only the slice counts. Bytes under null slots are
  NOT written** (see `test_packed_blob_writer_bulk_excludes_physical_null_bytes`: the junk between
  offsets of a null row must not reach the sidecar). Returns `None`. Errors, all `ValueError`, and
  the writer stays usable afterwards (a failed call writes nothing):
  - non-binary Array: `Packed blob payload array must have a Binary Arrow type, got Int32`
  - non-binary ChunkedArray: `Packed blob payloads must have a Binary Arrow type, got Int32`
  - not Arrow at all (list, bytes): `payloads must be a pyarrow BinaryArray, LargeBinaryArray, or ChunkedArray, got list`
  - non-monotonic offsets (validate before writing anything!):
    `Packed blob payload array contains invalid Arrow data: Invalid argument error: Offset invariant failure: non-monotonic offset at slot 1: 2 > 1`;
    in chunk k of a chunked array the prefix is `Packed blob payload chunk 1 contains invalid Arrow data: ...`.
    `pa.Array.validate(full=True)` raises for these; build the message yourself around the slot numbers.
- `finish()` -> Python `list` of `BlobDescriptor` (null rows give the `Null` descriptor).
  `repr`: `Packed { blob_id: 7, offset: 0, size: 7 }`, `Null`.
- `finish_array(field_name)` -> `pa.StructArray` of the descriptor type (3.5), null rows null.
  After it, `writer.field` is the descriptor field named `field_name` (equal to
  `BlobDescriptorArrayBuilder(field_name).field`). Before it, `writer.field` raises
  `ValueError("PackedBlobWriter field is available after finish_array")`.
- After `finish`/`finish_array`, any call raises `ValueError("PackedBlobWriter is already finished")`.

### 3.3 `DedicatedBlobWriter`

`write(data)` (same input rules as `write_blob`), returns `None`; `finish()` -> one `BlobDescriptor`,
repr `Dedicated { blob_id: 5, size: 6 }`; afterwards `ValueError("DedicatedBlobWriter is already finished")`.

### 3.4 `BlobDescriptor`

Opaque value object. Kinds and reprs: `Packed { blob_id, offset, size }`,
`Dedicated { blob_id, size }`, `Null`; also inline and external (choose a repr like
`Inline { size: N }` / `External { uri: "...", position: P, size: N }` -- not tested). `str == repr`.
Equality / hash: value-based is fine.

### 3.5 `BlobDescriptorArrayBuilder(field_name)`

- `.field`: `pa.field(field_name, STRUCT, nullable=True, metadata={b"ARROW:extension:name": b"lance.blob.v2"})`
  where STRUCT = `struct<kind: uint8, data: large_binary, uri: string, blob_id: uint32, blob_size: uint64, position: uint64>`.
- Methods: `append(descriptor)`, `extend(iterable of descriptors)`, `append_null()`,
  `append_inline(data)`, `append_dedicated(blob_id, size)`, `extend_packed(blob_id, offsets, sizes)`,
  `finish()` -> `pa.StructArray` of STRUCT. Measured rows (`to_pylist()`):
  - packed: `{'kind': 1, 'data': None, 'uri': None, 'blob_id': 2, 'blob_size': 3, 'position': 0}`
  - null descriptor / append_null: `None` (a null struct row)
  - inline: `{'kind': 0, 'data': b'inl', 'uri': None, 'blob_id': None, 'blob_size': None, 'position': None}`
  - dedicated: `{'kind': 2, 'data': None, 'uri': None, 'blob_id': 9, 'blob_size': 100, 'position': None}`
  - `extend_packed(3, [0, 10], [10, 5])` -> two packed rows (positions 0, 10; sizes 10, 5).
- Errors: `append(b'raw')` / `extend([b'r'])` -> `TypeError("'bytes' object is not an instance of 'BlobDescriptor'")`;
  `append(None)` -> `TypeError("'None' is not an instance of 'BlobDescriptor'")`;
  `extend_packed` length mismatch -> `ValueError("offsets and sizes must have the same length, got 1 and 2")`;
  after `finish()`: `ValueError("BlobDescriptorArrayBuilder is already finished")`.
  An empty builder's `finish()` returns a 0-length array of STRUCT.

### 3.6 Names

Exported as `lance.PackedBlobWriter`, `lance.DedicatedBlobWriter`, `lance.BlobDescriptorArrayBuilder`,
`lance.BlobDescriptor` and the same under `lance.blob`; the session methods on
`lance.file.LanceFileSession`. Today these names resolve to placeholders (look at
`bindings/python/nanolance/lance/_stubs.py` and how `__getattr__` in the modules uses it) -- replace
the placeholders with the real classes.

## 4. Plan (do it in this order)

**Phase A -- Python only, no C++.** Implement 3.1-3.6 in a new module
`bindings/python/nanolance/lance/_blob_writers.py`, export it from `nanolance/lance/__init__.py`,
`nanolance/lance/blob.py` and add the two methods to `LanceFileSession` in `nanolance/lance/file.py`.
Plain file I/O is fine (open the sidecar lazily or at open time -- check what pylance does if a writer
is finished with no blobs: does the file exist? copy that). Use pyarrow's buffers
(`arr.buffers()`, `arr.offset`, offsets dtype) to copy only valid slices; never `to_pylist()` the
payload bytes of large arrays one by one if you can slice buffers instead (performance matters in
this repo, but correctness first). Run the 8 phase-A pylance tests:

    cd /home/user/nanolance && python3 tools/pylance_suite.py -k "packed_blob_writer"

**Phase B -- descriptor-form blob columns in the file writer (C++).** The data-replacement test
writes a data file whose `lance.blob.v2` column *is* the descriptor struct of 3.5. nanolance's writer
rejects it today:

    OSError: Invalid user input: Blob v2 field 'blob' must be struct<data: large_binary, uri: utf8> or struct<data, uri, position: uint64, size: uint64>

That comes from `append_blob_v2_batch_column_values` in `src/file/blob_v2_external.cpp` (~line 720).
Teach it a third accepted input shape: children `kind, data, uri, blob_id, blob_size, position`
(exactly STRUCT above). For each row emit the descriptor as given (kinds 1 packed / 2 dedicated /
3 external: copy blob_id, blob_size -> size, position, uri; kind 0 inline: the bytes in `data` are
written inline exactly as the existing `struct<data, uri>` path writes inline data; null row: null).
Look at how the existing paths fill `ColumnValues` / `BlobV2ExternalDescriptor` and reuse that --
do not invent a new on-disk encoding; the page encoding must stay what `blob_v2_descriptor_page_encoding`
produces. Do not try to validate that the sidecar exists. Then make the data-replacement test pass,
plus the same scenario read back by **real pylance** (section 6).

**Phase B design notes (from the reviewer, read before coding):**

- In `append_blob_v2_batch_column_values` (`src/file/blob_v2_external.cpp`, ~line 720) the input shape is
  chosen from the children found with `find_child(mapping, blob_field.id, name)`. Add the
  descriptor shape (all six names present) as a third branch; the existing loop (~lines 780-860) shows
  how a row is read (`large_binary_value`, `utf8_value`, `uint64_value`), how a null row is recorded
  (`record_validity(out, index, false)` + `row_packed_sizes.push_back(0U)`), and how a row is emitted:
  `append_packed_row(out, kind, position, size, blob_id, uri_ptr, uri_len, error)` (~line 188). You
  will need a `uint8` and a `uint32` reader like `uint64_value` -- add them next to it.
- Kinds 1 / 2 / 3: emit `append_packed_row(out, kind, position, blob_size, blob_id, <uri or nullptr>, ...)`
  directly -- the bytes already exist, nothing is placed. A null `position` reads as 0.
- Kind 0 (inline bytes in `data`): today, rows with bytes are emitted as `kBlobKindPendingData` and
  `BlobV2Placer::place` (~line 359) later decides inline / packed / dedicated **by size thresholds**.
  A descriptor that says inline must stay inline: add `kBlobKindPendingInline = 0xFE` next to
  `kBlobKindPendingData` in `include/nanolance/blob_v2_external.hpp`, emit that, and in `place` and
  `blob_v2_has_pending` treat it like pending data but always take the inline branch.
- Then `finalize_blob_v2_schema_for_write` rewrites the field to the physical descriptor columns; check
  it does not assume 2 or 4 input children (grep it; fix only if it does).
- Write a C++ test only if cheap; the Python differential test (section 6 item 5) is the oracle.
- If `DataFile.create` / the `DataReplacement` commit then fails for a reason in field ids or schema
  mapping (not in the blob column itself), stop and report it with the error -- do not redesign that.

Build after C++ changes (both are needed for Python to see it):

    cd /home/user/nanolance/build && ninja && ctest -j8
    cd /home/user/nanolance/bindings/python/build/cp311-cp311-linux_x86_64 && ninja && \
      cp _nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/

## 5. Rules of this repository (read `AGENTS.md` first -- it is short)

- A test may skip only for a missing optional dependency. Never weaken, skip or delete a test to get
  green; never add `except Exception: pass` around a failing call.
- Match the surrounding code: comment density, naming, error style (`_errors.py`: `native()`,
  `unsupported()`). Files start with the SPDX header used everywhere.
- Keep the diff to this task. No refactors of unrelated code.
- C++ must build without warnings (`-Wall -Wextra`); CI uses `-Werror`.
- Do not `git commit`, `git push`, or change branches.

## 6. The differential test you write: `bindings/python/tests/test_blob_writers.py`

Style: copy the header docstring / fixture pattern of `bindings/python/tests/test_stable_row_ids.py`
(`require_pylance()` from `tests.support`). Every case runs the **same calls on `nanolance.lance` and
on real `lance`** and compares results:

1. Packed writer, mixed `write_blob` / `write_blobs` (binary, large_binary, binary_view,
   fixed-size binary, sliced, chunked, with nulls and empties): equal `repr`s of `finish()`, equal
   `finish_array(...).to_pylist()`, equal `field`, byte-identical sidecar files, equal `writer.path`
   relative to the base.
2. Dedicated writer: two `write`s, equal descriptor repr and sidecar bytes.
3. Builder: the full sequence of 3.5 on both, equal `finish().to_pylist()` and `field`.
4. Every error in 3.2/3.3/3.5: same exception type on both engines.
5. End to end (phase B): with nanolance, create a dataset (`id uint32`, `lance.blob_field("blob")`),
   write a packed sidecar + a dedicated sidecar with nanolance's writers, write the data file with
   nanolance's `LanceFileSession.open_writer`, commit `DataReplacement` (as in the pylance test), then
   read the blobs with `take_blobs` / `read_blobs` in **both** nanolance and pylance: same bytes.
   Also the reverse: sidecars written by pylance's writers, data file + commit by nanolance.

## 7. Docs

- `docs/PYLANCE_COMPAT.md`: remove `PackedBlobWriter`, `DedicatedBlobWriter`,
  `BlobDescriptorArrayBuilder` from the "not supported" list (search for them) and add one line where
  blob writes are described.
- `docs/PROGRESS.md`: append a short section in the style of the last ones (what, found on the way,
  verified with numbers).
- `docs/ROADMAP.md`: mark the item done (search "packed/dedicated" / "PackedBlobWriter").

## 8. When you are stuck or must decide something not covered here

Stop and put the question at the top of your final report, with what you tried and the options you
see. The reviewer will answer and you will continue. Do not guess on: on-disk format changes,
public API names not listed above, or deleting existing behaviour.

## 9. Your final report (keep it short)

- What you changed (files), phase A / B status.
- The exact pylance-suite line (`... N passed ...` and `all N expected tests passed`), the
  pytest summary line, ctest summary if C++ changed.
- Anything you could not make pass, with the error text.
- Open questions.
