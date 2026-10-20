# Next week (written 2026-10-17)

Where things stand, and what is left, in one place. Each item names its size (one delegated Haiku
session, or several, or Opus work) and where its notes are. The detailed history stays in
`docs/ROADMAP.md` and the task specs in `docs/tasks/`.

## Where nanolance stands

- pylance 12.0.0's own suite against `nanolance.lance`: **1,200 expected passes** (1,171 at the
  start of this week). `tools/pylance_suite.py` checks them; `--update` records new ones.
- nanolance's own tests: ctest 64 / 64; pytest `bindings/python/tests` 3,386 passed, 20 skipped,
  1 xfailed (2026-10-17).
- Performance against Rust Lance and pylance: see "Performance" below (benchmarks re-run
  2026-10-17).

## Correctness: known divergences (decide or fix)

These return an answer pylance would not. None is silent data corruption; each is recorded so
a user report can be matched to it.

| what | nanolance | pylance | size | notes |
|---|---|---|---|---|
| Index caches | process-wide, keyed by file | per dataset session | 1-2 sessions (thread a session through the index loaders) | a fresh dataset object reports cache hits where pylance reports misses; ROADMAP "Scan statistics" |
| `io_stats_incremental()` | process-wide counters, per-object deltas | per object | 1 session | reads by other dataset objects between two calls are included |
| Filters with literals that do not fit (`i16 > 40000`, `i32 = 7.0`) | return rows | coercion error | design call | EXPR_TYPES reviewer decision (1): kept on purpose |
| Mixed-unit duration arithmetic | refused | month_day_nano interval | small if wanted | EXPR_TYPES decision (3) |
| `arrow_cast(2.5, 'Duration(..)')` | error | empty result | -- | kept on purpose |
| Struct field names | case-insensitive (`s.X` works) | refused | small | older divergence, EXPR_TYPES B notes |
| Fuzzy FTS, huge fuzziness | expands to every term within k | `Levenshtein automaton exceeds size limit of 10000 states` | port the automaton bound | ROADMAP "Fuzzy FTS, the rest" |
| Fuzzy FTS, accented query tokens | char-level distance | a rule not yet pinned down | measure first | same |
| FTS / phrase cache hit-miss counts | right direction, other numbers | per page | small | ScanStatistics `indices_loaded`, `index_comparisons` are 0 |

## Index plan for AI users (2026-10-19, revised)

The plan, with each item's failing pylance tests, the Opus step, the Haiku sessions and the
done-criteria, is in `docs/ROADMAP.md` ("Index plan for AI users"). Next, Wave 1, in order, each
briefed by Opus and implemented by Haiku:

0. **First, T1**: refuse manifests whose feature flags nanolance does not support, on open and on write.
   Nothing checks them today, so a newer dataset can read as empty. 1 session.
1. Full-text search on list columns: 5 tests, 1 session.
2. Index build progress callbacks: 5 tests, 1 session. Also make the hybrid refusal pylance's exact message.
3. Multivector search: 4 tests, 3 sessions (M1 flat, M2 search, M3 build) after Opus measures the format.
4. Deferred index remap and the Fragment Reuse Index: 4 tests, 1-2 sessions.

T4 (Arrow dictionary columns on write) is a one-session filler. Meanwhile Opus researches Wave 2:
object-store reads, branches and clones (T3), IVF_RQ, the commit conflict rules (T2), binary IVF. Corrections to earlier
summaries: CJK segmentation is not done. Hybrid search is refused by pylance too, so it is LanceDB's
feature. Compaction already remaps indexes; the FRI only serves deferred remap.

## Features left, smallest first

| item | pylance tests | size | notes |
|---|---|---|---|
| Write data storage versions 2.1 / 2.0 | 85 | format research (Opus) + sessions | the largest group of pylance failures; see the suite breakdown below |
| `create_scalar_index(IndexConfig(index_type="inverted", parameters={...}))` ignores the parameters for INVERTED builds (only list-element builds read them); found in FTS_LISTS L2 | -- | small | string-column builds made this way change, so test against pylance |
| Staged (distributed) INVERTED builds over list elements are refused; `infer_index_segment_details` would drop `document_granularity` (field 14) | -- | small | FTS_LISTS L2 |
| `Compaction.plan`, `lance.debug.format_fragment`, `lance.util.KMeans`, `lance.pydantic.Vector` | 3-4 each | small each | |
| lance-c prepared fuzzy FTS (`lance_dataset_prepare_fts_match_query` with fuzziness) | -- | small | the fuzzy query exists |
| Expression leftovers: `l[1 + 1]`, projecting a struct element `ls[1]`, `array_element(...)`, `ts - arrow_cast(ts, 'Timestamp(..)')` (unit dropped), arrow_cast of a column to Duration, `coalesce(ts, ts_ms)` (coarser unit), `d32 - ts`, `dec + i32 * 1` (strict xfail), comparisons as projections keep old typing | ~0 directly | 1 session | `docs/tasks/EXPR_TYPES.md` outcomes A1-C3 |
| LABEL_LIST exact counts from the index; ScalarIndexQuery metrics in the count plan | -- | small | ROADMAP "Count from an exact scalar-index mask" |
| Validate a fuzzy token's dictionary scan cost (~0.15 s per token on 1M terms) -- an FST walk | -- | 1 session | perf, not correctness |
| Stemming and stop words for languages other than English | several | 1-2 sessions | the 18 language names are validated already |
| External blob URIs under a registered base path | 1 | 1 session, Opus review | touches the external-blob writer |
| IVF on binary vectors (IVF_FLAT hamming, `IvfModel` hamming, `hamming_clustering_for_ivf_partition`, multi-segment) | 4 | several sessions | the vector path is float-only; pylance's format is measured in ROADMAP "Binary vectors" |
| IVF_RQ (RaBitQ) | 6 | format research (Opus) + 2-3 sessions | the most important search feature left |
| icu / jieba / lindera tokenizers | 15 | large (external libraries, dictionaries) | CJK users only |
| MSVC: build the Windows job once | -- | run `ci-platforms.yml` by hand | the portable 128-bit path is tested on Linux only |

## Browser: the weblance demo (shipped 2026-10-17)

- **Live demo:** https://github.com/yoavbendor/weblance (public; GitHub Pages from `main`, root):
  a map of 25,000 COCO images by their SigLIP2 embeddings; a click fetches the 12 nearest rows'
  captions and JPEGs live from `jrmiller/coco-2017-siglip2-embeddings` on Hugging Face (one 48 GB
  Lance data file) with HTTP Range requests, through nanolance compiled to WebAssembly (433 KB
  gzipped) in six Web Workers. Verified in headless Chromium against the real dataset: 12 images,
  3.66 MB fetched (0.008% of the file). The weblance repo holds only the built module, the page,
  the map data (`tools/make_map.py` rebuilds it) and the license notices; nanolance stays private.
- **How:** every READ path goes through `io::ByteSource` (`include/nanolance/byte_source.hpp`):
  data files (pread, cached file end, coalesced column metadata, small-read block), manifests and
  version discovery, deletion files, tags, index-segment checks. `wasm/weblance_glue.cpp`
  registers an https source (synchronous XHR in a worker, single-range `Range` headers -- no CORS
  preflight -- with the redirected CDN URL cached per file) and a Hugging Face tree-API lister;
  `wasm/build_weblance.sh` builds the module; `tests/test_byte_source_remote.cpp` reads a dataset
  through a `mem://` store after deleting the local directory. The I/O rework measured level or
  faster natively (wide scans 2x, takes 15-37%, a stat storm of ~800 stats per take removed).

- **Explore page (2026-10-18):** `explore.html` opens any public Hugging Face Lance image dataset
  (presets: MNIST, CIFAR-10, Oxford Pets, Food-101, COCO): it finds the table, picks the image /
  vector / label columns, samples up to 5,000 vectors (`wl_take_vectors`), lays them out with PCA in
  a worker and colors them by class. Six `lance-format` datasets (2.0 and 2.1) match pylance through
  the module; MNIST and Food-101 pass the page's real flow (`wasm/test/run_explore_test.cjs hf`).
  Brief and outcome: `docs/tasks/WEBLANCE_EXPLORE.md`.

**Open (the demo works; these make it better):**

| item | why | size |
|---|---|---|
| ~18 HTTP requests per image on a click (223 for 12) | every column of every row is its own page lookup; the six workers hide it, a real browser feels it | 1 session: batch the page-metadata reads of one take, larger blocks for the image column |
| Measure in a real browser (not through this session's proxy, which costs seconds per request) | the page's numbers are what a viewer sees | the user, 5 minutes |
| Each take window costs ~16 requests whatever its size (10 windows of 500 rows: ~160; 50 windows of 100: ~800, too slow) | so the explore sample is 10 windows, and a class-sorted table (Food-101) shows ~14 of 101 classes | same session as the row above: fewer page lookups per take |
| Explore page: caption-region labels and the images-only grid are built but untested; discovery lists Lance's `__manifest` dirs | rough edges | small |
| Each of the six workers opens the dataset (~20 requests each) | share one open's metadata, or open in one worker and post it | small |
| Similar-image search in the page (the dataset has an IVF_HNSW_SQ index; nanolance searches it) | the next demo step: click an image, find its neighbours by vector, reading only index partitions | 1-2 sessions |
| C++ exceptions are off in the WASM build (out of memory aborts) | acceptable for the demo | measure `-fexceptions` size |
| Remote paths joined with `/` get a backslash on Windows | remote reads are POSIX / Emscripten only | small |
| clang-18 fails on `fts_json.hpp` (incomplete `json::Value` in a pair) in `dataset_refs.cpp`, at older commits too | GCC and Emscripten's clang are fine | small |

## Flaky tests to watch

- `tests/test_index_optimize.py::test_optimize_after_append[nanolance]` (~1 run in 15-90): IVF_PQ
  distances after pylance's vs nanolance's `optimize_indices` differ (ROADMAP).
- `test_hnsw.py::test_new_kinds_match_pylance[True-dot-IVF_SQ]`: failed once with two full runs
  at once; passes alone.

## Performance

Re-run 2026-10-17 on this 4-core container (`bench/results/*.json`, `docs/BENCHMARKS.md`,
`bench/results/report.html`). Ratios are the other engine's time over nanolance's: above 1 means
nanolance is faster. "1c" pins both to one core; "mt" is all cores; "py" is through Python
(nanolance.lance against pylance).

**Against Rust Lance, 27 data types (geometric means):**

| | 1 core | all cores | Python |
|---|---|---|---|
| write | 1.45x | 1.71x | 1.86x |
| read (files written by Rust) | 1.84x | 3.78x | 3.34x |

**Against pylance, indexes (geometric means):** scalar index builds and queries 2.09x, vector
indexes 2.86x (IVF_FLAT, IVF_PQ, IVF_HNSW_SQ; recall equal), full-text 2.61x.

**Where Rust Lance is faster** (ratio below 1), the work for next week's performance pass:

| shape | 1-core write | all-core write | 1-core read | what is known |
|---|---|---|---|---|
| map<string, int64> | 0.67 | 0.84 | 0.85 | nested write: four threads no faster than one; ~20 ms/write of fresh page faults; most time outside the rep/def serializer and page build (ROADMAP perf notes) |
| list<string> | 1.20 | 0.66 | 1.08 | same nested-write path |
| list<struct> | 0.78 | 0.94 | 0.63 | nested write and read |
| list<list<int32>> | 1.06 | 1.22 | 0.73 (mt 0.98) | nested read |
| struct_mixed | 0.76 | 0.91 | 1.11 | |
| string_long / string_urls / binary_blobs | 0.77-0.93 | 0.96-1.36 | > 1 | single-core string writes |
| vector_f32x128 | 0.94 | 1.17 | 1.35 | |
| bool_flags | 0.78 | 2.14 | 5.05 | single-core boolean write |
| string_lowcard | 1.40 | 1.00 | 0.99 | |
| BTREE build on numeric columns | -- | 0.85-0.90 (pylance) | -- | bimodal run to run: 0.95 s or 1.45 s (pylance steady 1.2-1.3 s); find the slow mode |

**Checked against older builds, so the numbers above are not regressions.** Today's ratios are
lower than the committed 26 September run (single-core reads 3.7x then, 1.8x now), but Rust itself
is ~1.4x slower on this machine today, and the 26 September build (`f852c5a`, worktree
`/home/user/nl-baseline`) and today's measure the same on the same machine: int64 / float64 /
string reads within noise, long-string writes level. Two things to measure properly: small-column
single-core writes look ~1-2 ms slower than on 26 September (8-11 ms vs 6-9 ms for 2M int32;
likely the per-commit transaction file, which Lance writes too), and a full-text
change this week was A/B-timed level. Benchmark on a quiet machine before and after each
optimization; compare engines within one run (ratios), never across days.

## pylance's suite: what the 287 remaining failures are

`COLUMNS=400 python3 tools/pylance_suite.py`, failures grouped by message (2026-10-17):

| failures | cause | size |
|---|---|---|
| 85 | writing `data_storage_version` 2.0 / 2.1 (nanolance writes 2.2; reads all three) -- 72 of them in test_blob.py, parametrized over versions | **the biggest single lever**: a writer for 2.1 (and 2.0) layouts; format research first (Opus), then sessions |
| 20 | icu / jieba / lindera tokenizers | large |
| 17 | `mem_wal` (`initialize_mem_wal`, ...) | a subsystem; tier 4 |
| 5 | IVF over binary vectors (hamming metric) | several sessions |
| 5 | IVF_RQ | format research + sessions |
| 5 | `lance.lance._ObjectStoreRegistry` (custom object stores) | small-medium |
| 4 | table branches | medium |
| 4 | `Compaction.plan` (the compaction planner as an API) | small: the planner exists |
| 4 | full-text search over list columns | 1 session |
| 3 each | `lance.util.KMeans`, `lance.pydantic.Vector`, `lance.debug.format_fragment`, `LanceFragment.update_columns` of a blob column | small each |
| ~120 | assertion mismatches and one-offs | need grouping by test |

## How to work (what worked this week)

- One Haiku session per item, with pylance's behaviour measured first and written into its prompt
  or spec; a stop rule for anything touching shared planning code; Opus reviews the diff, re-runs
  the checks, answers questions, commits. Haiku sessions run ~100-200k tokens.
- Review catches that mattered this week: a count memo keyed by filter only (stale after
  `delete()`), a reader path that silently returned a short dictionary column, `__int128` breaking
  MSVC, a full-text change that needed an A/B timing to show it was not slower.
- Run one full pytest at a time; two at once caused one spurious failure.
- Compaction with `max_bytes_per_file`: pylance cuts files at batch granularity (`test_optimize_max_bytes` wants 2 fragments, nanolance writes 8); changes eager compaction output, decide first.
- `list_indices()` / `describe_indices()`: pylance sorts by name and reports each index's own version; nanolance keeps manifest order and reports the dataset version.
- `compact_files(defer_index_remap=True)` with stable row ids is refused; FRI details over 200 KB (external) are refused when writing.

