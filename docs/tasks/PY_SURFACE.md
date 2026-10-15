# Task: Python surface -- `lance.log` and Lance's log output, `lance.arrow` types, `lance.sampler`

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap item 8
(logging, samplers). Three phases, each a fresh session: **P1** logging, **P2** `lance.arrow`,
**P3** `lance.sampler`. About 50 of pylance's tests; mostly Python, little or no C++.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`bindings/python/nanolance/lance/dataset.py` (3.5k+) whole. Cut all output (`| tail -15`; pytest
`-o addopts=` for a summary line; `tools/pylance_suite.py ... | tail -5`). Only your phase.

The reference is real pylance 12.0.0 (`import lance` in plain `python3`): read its module source
with `python3 -c "import inspect, lance.log as m; print(inspect.getsource(m))" | sed -n 1,120p`
(windows; `lance.arrow` is 583 lines, `lance.sampler` 670) and match its behaviour, names,
signatures, reprs and error messages. Write nanolance's own code (Apache-2.0 headers as in the repo);
do not paste pylance's modules wholesale. How nanolance exposes `lance.*` modules today: grep
`_stubs.py` (`"_arrow", "_arrow.bf16", ...` is the list of modules that only raise NotSupported) and
`bindings/python/nanolance/lance/__init__.py`; `tools/pylance_suite.py` loads nanolance as `lance`.

## 1. Facts (measured on pylance 12.0.0 here)

**P1 -- logging** (`tests/test_log.py`, 14 tests; every one also fails on teardown today because
the autouse fixture pops `LOGGER.handlers`):
- `lance.log`: `ENV_NAME_PYLANCE_LOGGING_LEVEL = "LANCE_LOG"`, `get_log_level()` (first entry of
  `LANCE_LOG` without `=`, upper-cased, `trace` -> `DEBUG`, empty -> `INFO`), `LOGGER =
  logging.getLogger("pylance")` with that level, `set_logger(file_path="pylance.log", name="pylance",
  level=logging.INFO, format_string=None, log_handler=None)` (rebinds `LOGGER`, default format
  `"%(asctime)s %(name)s [%(levelname)s] %(filename)s:%(lineno)d %(funcName)s : %(message)s"`).
  Note test_log imports `LOGGER` by name, so the module object it reads must be a real
  `logging.Logger`.
- Lance's native log (Rust `env_logger`): enabled by env `LANCE_LOG` (a filter list:
  `level` or `target=level`, comma-separated, e.g. `warn,lance::events::dataset_events=info`),
  written to **stderr**, or to `LANCE_LOG_FILE` (parent directories created; an unwritable path ->
  a message on stderr, the operation still succeeds, no file). Line format:
  `[2026-10-15T18:43:41Z INFO  lance::events::dataset_events] <message>` (level padded to 5);
  `LANCE_LOG_TS_PRECISION` = `s` (default) | `ms` | `us` | `ns` -> `...:41Z`, `...:41.123Z`,
  `...:41.123456Z`, `...:41.123456789Z`.
- Events pylance logs for `write_dataset(tbl, uri)` (at INFO):
  `lance::events::dataset_events` `target="lance::dataset_events" event="loading" uri="memory://test"
  target_ref=None status="error" ` / `event="writing" uri=... mode=Create ` /
  `event="committed" uri=... read_version=0 committed_version=1 detached=false operation="Overwrite" `;
  `lance::events::file_audit` `target="lance::file_audit" mode="create" type="data" path="<file>.lance" `
  and `type="manifest"`. At DEBUG, other targets too (`lance_file::writer::structural ...`): nanolance
  may log its own debug lines under its own targets (`nanolance::...`), they only need to exist.
- Implement the native log where events happen: Python-side helpers called from nanolance's
  write / open / commit paths are enough for these tests (decide; if C++ must log too, stop and ask).

**P2 -- `lance.arrow`** (`tests/test_arrow.py`: 14 bf16 tests, 4 image tests,
`test_struct_variable_children_roundtrip`):
- `BFloat16` scalar (pylance's is native: `BFloat16(1.1)`, `from_bytes` with its exact length error,
  repr, comparisons -- measure), `BFloat16Type` (pyarrow ExtensionType `lance.bfloat16`, storage
  `fixed_size_binary(2)`), `BFloat16Array`, `PandasBFloat16Array`, `bfloat16_array(values)`, `cast`
  (to / from bf16, fixed-size lists of bf16). Use numpy uint16 bit patterns (round to nearest even
  from float32, as pylance -- check against pylance on edge values: NaN, inf, subnormals, ties).
- Image types: `ImageURIArray` / `ImageURIType` (`lance.arrow.image_uri`), `EncodedImageArray`,
  `FixedShapeImageTensorArray` and their scalars; `from_uris`, `read_uris`, `to_tensor`,
  `to_encoded`, pickling. Check which need Pillow / tensorflow (tests may skip) and match.
- Round trips through nanolance datasets: the extension name / metadata must survive
  `write_dataset` -> `to_table` (check what nanolance's writer keeps of `ARROW:extension:*` field
  metadata; if bf16 columns need the C++ writer to keep it, that is in scope -- small).
- `test_struct_variable_children_roundtrip` fails differently (an assertion on the written file):
  read it, report what it checks before fixing.

**P3 -- `lance.sampler`** (`tests/test_sampler.py`, 9 tests; `test_pytorch_integration` skips
without torch): `ShardedFixedBatchSampler`, `ShardedFragmentSampler`, `ShardedBatchSampler`,
`FullScanSampler`, `SampleParams`, `maybe_sample`, `reservoir_sampling` -- whatever the module
exports (`dir()`); same rows / batches for the same seeds as pylance. Also
`lance._dataset.sharded_batch_iterator.ShardedBatchIterator` (3 tests in test_dataset, grep).

## 2. Oracles

Run pylance's tests through the runner (it loads nanolance as `lance`):
`python3 tools/pylance_suite.py -k "<expr>" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(a `tests/test_datagen.py` collection error shows with `-k`; ignore it). P1: `-k test_log` (file
filter: use the test names). P2: `-k "bf16 or image or struct_variable_children"`. P3:
`-k "test_sampler or sharded"`.

## 3. Tests you write

`bindings/python/tests/test_py_surface.py` (new): both engines side by side (`require_pylance()` from
`tests.support`, as other test files do) -- same log lines (modulo timestamps and file names), same
bf16 bit patterns and reprs, same sampler outputs.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7 (grep the headings). No skipped / weakened tests. Do not commit, push,
change branches, or run `--update`. Do not schedule reminders or triggers. Disk is tight: no
`pytest -n`, clean temp dirs. If C++ changes are needed: `cd build && ninja`, then
`cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) if logging needs the C++ core, if a type needs a new on-disk encoding,
or if pylance's behaviour depends on its Rust internals in a way you cannot reproduce. If context
nears its limit, stop at a clean point and report what is done and left.

## 5. Done per phase -- report (under 40 lines, sent once)

Tails of: `cd build && ctest -j8 | grep "tests passed"` (if C++ changed); the oracle `-k` run; your
test file; `python3 tools/pylance_suite.py | tail -4` ("all N expected tests passed"); last, once:
`cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -1`.
Files changed, per-test status, decisions, questions.

**P1 outcome (2026-10-15):** done (Python only). `lance.log` (pylance's API), and Lance's native
log in `_native_log.py`: `LANCE_LOG` filters, `LANCE_LOG_FILE` (parents created, unwritable path ->
stderr message), `LANCE_LOG_TS_PRECISION`; `dataset_events` loading / writing / committed and
`file_audit` data / manifest lines equal pylance's for write / append / open, committed + manifest for
delete (every commit path logs through `_refresh_latest(operation)`). +17 pylance tests (1080).
Not emitted: pylance's deletion-file audit lines and `lance::events::execution` plan lines; operation
names of rarer commits follow Lance's Operation enum, unmeasured. `test_lance_log_file_invalid_path`
cannot pass in this sandbox (root can create `/invalid/...`; pylance fails it too) -- a stray
`/invalid/path/.../lance.log` from a September pylance baseline run is still there (removal needs the
user).

**P2 outcome (2026-10-15):** done. `lance.arrow` (BFloat16 scalar / type / arrays / pandas dtype,
`bfloat16_array`, `cast`, image URI / encoded / tensor types): bit patterns and reprs equal pylance's
on 3950 values (edge values, ties, subnormals, NaN payloads). C++: `lance_extension_is_lance_owned`
now means only `lance.blob.v2` (every `lance.*` column used to be read as a blob shell, so bf16 and
image columns failed); a bf16 fixed-size list's element is Lance's `lance.bfloat16` on disk
(reviewer added the shape to the encoding matrix). +17 pylance tests (1097). Not done:
`test_struct_variable_children_roundtrip` needs a new writer encoding (variable-width packed struct,
FullZip) -- roadmap; the two image-tensor tests need Pillow (pylance fails them here too; not
installed). nanolance accepts a few conversions pylance refuses (to_numpy / cast with nulls,
BFloat16Array.from_pandas returns bfloat16).
