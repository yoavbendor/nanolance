# Next week (written 2026-10-17)

Where things stand, and what is left, in one place. Each item names its size (one delegated Haiku
session, or several, or Opus work) and where its notes are. The detailed history stays in
`docs/ROADMAP.md` and the task specs in `docs/tasks/`.

## Where nanolance stands

- pylance 12.0.0's own suite against `nanolance.lance`: **1,200 expected passes** (1,171 at the
  start of this week). `tools/pylance_suite.py` checks them; `--update` records new ones.
- nanolance's own tests: ctest 63 / 63; pytest `bindings/python/tests` 3,386 passed, 20 skipped,
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

## Features left, smallest first

| item | pylance tests | size | notes |
|---|---|---|---|
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

## Flaky tests to watch

- `tests/test_index_optimize.py::test_optimize_after_append[nanolance]` (~1 run in 15-90): IVF_PQ
  distances after pylance's vs nanolance's `optimize_indices` differ (ROADMAP).
- `test_hnsw.py::test_new_kinds_match_pylance[True-dot-IVF_SQ]`: failed once with two full runs
  at once; passes alone.

## Performance

(Filled in from the 2026-10-17 benchmark run below.)

## How to work (what worked this week)

- One Haiku session per item, with pylance's behaviour measured first and written into its prompt
  or spec; a stop rule for anything touching shared planning code; Opus reviews the diff, re-runs
  the checks, answers questions, commits. Haiku sessions run ~100-200k tokens.
- Review catches that mattered this week: a count memo keyed by filter only (stale after
  `delete()`), a reader path that silently returned a short dictionary column, `__int128` breaking
  MSVC, a full-text change that needed an A/B timing to show it was not slower.
- Run one full pytest at a time; two at once caused one spurious failure.
