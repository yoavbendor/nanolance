# Task: `lance.tokenize`, `lance.Session` and cache sizes, batch (multi-query) KNN

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Three phases, each a
fresh session unless one finishes early with context to spare: **Q1** tokenize, **Q2** Session,
**Q3** batch queries. ~23 of pylance's tests.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`bindings/python/nanolance/lance/dataset.py` (3.5k+), `src/vector_search.cpp` (2.4k) or
`bindings/python/src/nanolance_bindings.cpp` whole. Cut all output (`| tail -15`; pytest
`-o addopts=` for a summary line; `tools/pylance_suite.py ... | tail -5`). Real pylance 12.0.0 is
`import lance` in plain `python3`: measure every behaviour there first. Write nanolance's own code.

## 1. Facts (measured on pylance 12.0.0 here)

**Q1 -- `lance.tokenize`** (`tests/test_lance.py`, 6 tests): signature
`tokenize(query, *, analyzer=None, base_tokenizer=None, language=None, max_token_length=Ellipsis,
lower_case=None, stem=None, remove_stop_words=None, custom_stop_words=None, ascii_folding=None,
min_ngram_length=None, max_ngram_length=None, prefix_only=None, split_identifiers=None,
split_on_numerics=None, preserve_original=None, index_operators=None)` -> list of `FtsToken`
(`.text`, `.position`, repr `FtsToken(text="cat", position=0)`). Same option resolution as
`create_scalar_index(index_type="INVERTED")` (nanolance: `_inverted_params` / `_inverted_analyzer`
in dataset.py; the C++ `fts::Analyzer` in src/fts_tokenizer.cpp does the work). `None` = the
analyzer profile's default, except `max_token_length=None` disables the limit (omitted -> 40).
Positions are relative to the first kept token, gaps from filters kept (`"the Cats and Dogs"` ->
`[('cat', 0), ('dog', 2)]`). Errors as create_scalar_index's (`code analyzer flags require
analyzer='code'`, `unknown base tokenizer ...`). Needs a small binding that runs the analyzer on a
string with the params JSON and returns (text, position) pairs.

**Q2 -- `lance.Session`** (`tests/test_session.py` 9 tests, `test_dataset.py::test_metadata_cache_size`):
`Session(index_cache_size_bytes=None, metadata_cache_size_bytes=None, index_cache_backend=None,
metadata_cache_backend=None, store_registry=None)`; methods `size_bytes()`,
`index_cache_size_bytes()`, `is_same_as(other)`; repr `Session(index_cache_size_bytes=0,
metadata_cache_size_bytes=0)` (measure what the numbers mean). `ds.session()` returns the dataset's
session; `lance.dataset(uri, session=s)`, `write_fragments(..., session=)`,
`lance.dataset(uri, metadata_cache_size_bytes=0)`. Sizes must grow when a scan / index search fills
nanolance's caches (nanolance has caches in C++: grep `index_cache()` in src/index_files.cpp /
src/fts_search.cpp / src/vector_search.cpp, and whatever caches manifests / file metadata) and be
smaller with a zero metadata cache. A session object that owns cache budgets and reports sizes
from the C++ caches is the design; if the C++ caches are process-global, report their sizes and
keep per-session budgets as far as the tests need -- decide, and stop and ask if a real per-session
cache split is required. Backends: `index_cache_backend="moka://?capacity=N"` or a dict
`{"kind": "moka", "options": {...}}` (check the dict form on pylance); errors, copied without the
Rust source location: `index_cache_size_bytes and index_cache_backend are mutually exclusive; set
one or the other`; `index_cache_backend: Invalid user input: moka cache backend: capacity is
required; use moka://?capacity=<bytes>`; `... capacity must not be empty`;
`index_cache_backend: unknown dict key "type"; expected 'kind' or 'options'`.

**Q3 -- batch KNN** (`tests/test_vector_index.py`, 7 tests: grep `test_batch_`): `nearest={"q":
2-D array (n, dim), ...}` runs n queries; the result has a leading `query_index: int32 not null`
column, `k` rows per query in query order, each query's rows equal to the single query's
(`_assert_batch_matches_single_queries` in that file); a single-row 2-D array also gets
`query_index`; `fast_search=True` without an index -> 0 rows but the column present; a dim
mismatch error matching `256.*128`. Today nanolance refuses with "does not support batch and
multivector queries": grep that text. Multivector (a list column of vectors) stays refused unless
a target test needs it. Simplest correct design: run the existing single-query search per row and
concatenate (C++ or Python -- check where the refusal is), with prefilters, refine, deletions and
indexes exactly as for one query.

## 2. Oracles

`python3 tools/pylance_suite.py -k "<expr>" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(ignore the `tests/test_datagen.py` collection error). Q1: `-k "tokenize"`. Q2: `-k "test_session or
cache_size or metadata_cache"`. Q3: `-k "test_batch_"`.

## 3. Tests you write

`bindings/python/tests/test_py_surface.py` (add a section per phase), both engines side by side via
`require_pylance()`: token lists for many option sets (text / code / ngram analyzers, stop words,
stemming, lengths), Session behaviour and errors, batch results equal to repeated single queries
and to pylance's, with and without indexes, filters, deletions.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7. No skipped / weakened tests. C++ without warnings. Do not commit, push,
change branches, or run `--update`. Do not schedule reminders or triggers. No `pytest -n`; clean temp
dirs. Builds: `cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja
&& cp _nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) per the notes above. If context nears its limit, stop at a clean point
and report what is done and left.

## 5. Done per phase -- report (under 40 lines, sent once)

Tails of: `cd build && ctest -j8 | grep "tests passed"` (if C++ changed); the oracle `-k` run; your
test file; `python3 tools/pylance_suite.py | tail -4`; last, once:
`cd bindings/python && python3 -m pytest -o addopts= -q -rf -p no:cacheprovider tests | tail -3`.
Files changed, per-test status, decisions, questions.

**Q1 outcome (2026-10-15):** done. `lance.tokenize` / `FtsToken` over the C++ analyzer (`_fts_tokenize`
binding), ~50 option sets equal to pylance's; all 6 tokenize tests (+6, 1115). An unknown base
tokenizer is now a ValueError (`unknown base tokenizer X (...)`, as pylance) for create_scalar_index
too. Divergences: stemming / stop words outside English are NotSupported (nanolance's analyzer is
English-only); unknown language names are accepted when they do not matter (pylance raises) --
small item.

**Q2 outcome (2026-10-16):** done in Python (`bindings/python/nanolance/lance/_session.py`), no C++ change.
`lance.Session` with the pylance signature, `size_bytes()`, `index_cache_size_bytes()`,
`metadata_cache_size_bytes()`, `is_same_as()`, pylance's repr; `ds.session()`; `lance.dataset(session=)`
(budget kwargs are then ignored, as pylance does); `write_fragments(..., session=)` and
`LanceFragment.create(session=)` (a dataset destination must use its own session). Backend URIs and dicts
are validated with pylance's messages (Rust source suffixes stripped). Divergence: the sizes are
**per-session estimates over shared process-wide caches**, not pylance's per-session caches. nanolance's
C++ caches stay global. A session records each manifest (by uri and version: the manifest file's size),
each data file's footer and column metadata (by path: the bytes from the column-metadata offset to the
end of the file), and each vector or inverted index searched (by UUID: the size of `_indices/<uuid>`), once
per session. A budget admits an entry only while the ledger plus the entry fits, so 0 and 64 record nothing
(size 152, as pylance). A zero index budget still disables that handle's C++ index cache. Remote URIs get
a fixed 1024-byte estimate per entry. Tests: `test_py_surface.py` Session section (3 tests, both engines
for errors, reprs and behaviour; nanolance-only index estimates). Oracle: all 9 `test_session.py` tests and
`test_metadata_cache_size` pass (+10). Remaining failure matched by the `-k`: `test_file.py::
test_session_list_with_delimiter` (LanceFileSession listing, not Q2, failing before this change).
