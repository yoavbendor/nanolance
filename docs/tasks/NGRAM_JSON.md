# Task: NGRAM and JSON scalar indexes, the ngram tokenizer for INVERTED

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap item 5
(scalar / FTS breadth). Three phases, each a fresh session: **N1** NGRAM index, **N2** JSON index,
**N3** `base_tokenizer="ngram"` for INVERTED.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`dataset.py` (3.5k+), `src/index_search.cpp`, `src/scalar_index.cpp` or Lance's `ngram.rs` (2.7k) /
`json.rs` (1.7k) whole. Cut all output (`| tail -15`; pytest `-o addopts=` for a summary line;
`tools/pylance_suite.py ... | tail -5`; `ninja 2>&1 | grep -E "error|warning:" | head`). Only your
phase. Lance's source: `.deps/rust-suite/lance-index-12.0.0/src/scalar/{ngram.rs,json.rs}`,
`.deps/rust-suite/lance-tokenizer-12.0.0/src/{ngram_tokenizer.rs,alphanum_only.rs}`. The model for
adding an index kind is the ZONEMAP / BLOOMFILTER work (`docs/tasks/ZONED_INDEXES.md`): grep
`ZONEMAP\|ZoneMap\|zonemap` in `src/scalar_index.cpp src/index_search.cpp src/index_segments.cpp
src/index_files.cpp src/index_optimize.cpp include/nanolance/scalar_index.hpp
bindings/python/nanolance/lance/dataset.py bindings/python/nanolance/lance/indices/builder.py` and
follow the same path. Real pylance is `import lance` in plain `python3`; a probe that dumps both
indexes: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/ngram/probe.py`
(copy it to your own scratch dir first).

## 1. Facts (measured on pylance 12.0.0 here)

**NGRAM** (`create_scalar_index(col, index_type="NGRAM")`, string / large_string columns):
- One file `ngram_postings.lance`: schema `tokens: uint32` (nullable) + `posting_list: binary not
  null` with field metadata `lance-encoding:compression = none`; one row per distinct token, sorted by
  token. `posting_list` = a **RoaringTreemap** serialization of the row ids (u64 LE count of 32-bit
  high keys, then per key `[u32 LE high][portable 32-bit Roaring]`) -- not the RowAddrTreeMap format
  of `serialize_treemap`. **Token 0 holds the null rows.** Example: 5 rows
  `["apple pie","Banana!",None,"","ÉCOLE ab"]` -> 11 rows, token 0 -> {2}.
- Tokenizing (`ngram.rs` 87-160): per value, `RawTokenizer` + `LowerCaser` + `AsciiFoldingFilter`
  (one token = the whole value lowered and folded), then `NgramTokenizer::all_ngrams(3, 3)` over
  characters, then `AlphaNumOnlyFilter` (keep a trigram only if every char `is_ascii_alphanumeric`).
  Token id: `ngram_to_token` (base 37; '0'-'9' -> 1-10, 'a'-'z' -> 11-36; first char * 37^2 ...).
  nanolance already has lower-case and ASCII-folding code in `src/fts_tokenizer.cpp`: reuse it.
- Manifest: `type_url` `/lance.table.NGramIndexDetails` (empty message), `index_version` 0,
  `describe_indices()[i].index_type == "NGram"`; `stats.index_stats(name)`: `index_type "NGram"`,
  `indices` / `segments` = `[{"num_ngrams": N}]`.
- Search (`ngram.rs` 506-600): `contains(col, 'x')` (and Lance's other forms it routes to
  `TextQuery::StringContains` -- grep `StringContains` in lance-index's `scalar/expression.rs`; check
  `LIKE '%x%'`) -> a needle of < 3 chars: index knows nothing (recheck all rows); any needle trigram
  missing from the index: exact empty; no trigrams after filtering: recheck all; else the
  intersection of the posting lists as **candidates to recheck** (at_most). `regexp_like` uses
  `ngram/ngram_regex.rs` -- out of scope (treat as recheck all) unless a target test needs it.
  Results always equal a scan without the index; the plan shows
  `ScalarIndexQuery: query=[contains(w, Utf8("apple"))]@w_idx(NGram)` (nanolance builds this line in
  `src/index_search.cpp` ~1012).
- Maintenance like the other scalar kinds: `optimize_indices` deltas, compaction re-cover,
  `create_index_uncommitted(index_type="NGRAM", fragment_ids=...)`, `merge_existing_index_segments`,
  `commit_existing_index_segments`, drop, empty dataset.

**JSON** (`create_scalar_index(col, IndexConfig(index_type="json", parameters={"target_index_type":
"btree", "path": "x"}))`, on a JSON column):
- Files = a normal **BTree** (`page_data.lance`: `values: <T>`, `ids: uint64 not null`;
  `page_lookup.lance`: `min, max, null_count, page_idx`, metadata `batch_size`,
  `range_partitioned = false`) over the value extracted at `path`. nanolance already writes BTree
  files: reuse the builder with the extracted column.
- Value type `<T>` (`json.rs` 531-590): the type of the **first non-null** extracted value --
  bool -> Boolean, int -> Int64, float -> Float64, string -> Utf8, array / object -> LargeBinary;
  all null -> Utf8. Later rows must be compatible (Float64 accepts ints) or the build fails with
  `JSON path '{path}' expected {T:?}, but batch row {i} has JSON type {J:?}`. Missing path = null.
- Manifest: `type_url` `/lance.index.pb.JsonIndexDetails` = `{string path = 1; Any target_details =
  2}` with `target_details` the BTree's details Any; `index_version` 0; describe `index_type "Json"`,
  details `{"path": "x", "target_details": {}}`; `stats.index_stats`: `index_type "BTree"`,
  `indices` `[{"min":..,"max":..,"num_pages":..}]` as for a BTree. Default name `<col>_idx`.
- Queries routed (`json.rs` ~300-460): `json_get_int / json_get_float / json_get_bool /
  json_get_string(col, '<path>') <op> literal` where the function's type equals the index's `<T>` and
  the path string equals the index path; plan line
  `ScalarIndexQuery: query=[Json(j = 11->x)]@j_idx(BTree)`. `json_extract` is never routed.
  nanolance evaluates these functions already (`src/expr.cpp` ~1650).

**ngram tokenizer for INVERTED** (N3): `base_tokenizer="ngram"`, `min_ngram_length` (default 3),
`max_ngram_length` (default 3), `prefix_only`; `stem` / `remove_stop_words` default false for it.
Port `ngram_tokenizer.rs` (221 lines, read whole) into `src/fts_tokenizer.cpp`'s chain. Oracle
`test_fts_ngram_tokenizer` (read it).

## 2. Phases and oracles

Run pylance's tests through the runner (it loads nanolance as `lance`):
`python3 tools/pylance_suite.py -k "<names>" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(a `tests/test_datagen.py` collection error shows with `-k`; ignore it).

- **N1 NGRAM:** `test_ngram_index`, `test_ngram_fts`, `test_index_take_batch_size`,
  `test_scalar_index_with_nulls`, `test_create_index_empty_dataset`, `test_optimize_no_new_data`,
  `test_drop_index`, `test_ngram_segment_merge_and_commit_from_python`,
  `test_optimize_indices_second_call_is_noop` (some use other index kinds too; read each before
  starting). Byte-level: same token rows and identical `posting_list` blobs as pylance on the same
  data (one fragment), both engines search each other's index.
- **N2 JSON:** `test_describe_indices` (6 cases; it also builds NGRAM, so after N1). The
  `test_json_index*` tests use `memory://` (object stores are last on the roadmap): write
  equivalent tests on `tmp_path` instead.
- **N3:** `test_fts_ngram_tokenizer`.

## 3. Tests you write

`bindings/python/tests/test_ngram_json_index.py` (new; follow `test_zoned_indexes.py`'s style):
each engine builds, the other searches; same rows with and without the index; posting blobs /
BTree pages equal; describe / stats equal; deletions, appends + optimize, nulls, empty strings,
non-ASCII. N3 cases go in `test_fts.py`.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7 (grep the headings). No skipped / weakened tests. C++ without warnings.
Existing indexes stay byte-identical. Do not commit, push, change branches, or run `--update`.
Builds: `cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) if pylance's files differ from section 1, if routing a filter to the
index needs changes to the expression evaluator beyond recognizing the call shape, or if a target
test needs `regexp_like` index support.

## 5. Done per phase -- report (under 40 lines, sent once)

Tails of: `cd build && ctest -j8 | grep "tests passed"`; the oracle `-k` run; your test file;
`python3 tools/pylance_suite.py | tail -4` ("all N expected tests passed"); last, once, quiet:
`cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -1`.
Files changed, per-test status, decisions, questions.

**N1 investigation (2026-10-15), no code changed -- reviewer's answers:**
- N1 is split: **N1a** = RoaringTreemap encoder, tokenizer, build, search, `contains` routing, explain
  line, describe / stats, drop, empty dataset, and the oracle tests among those; **N1b** = optimize
  deltas, compaction re-cover, `create_index_uncommitted` / merge / commit segments,
  `test_optimize_indices_second_call_is_noop`.
- **Approved:** a new `expr::Predicate::Test::Contains` (include/nanolance/expr.hpp ~58), recognized
  only from `contains(<string column>, '<literal>')` in the Condition walk (src/expr.cpp ~2980-3050,
  `HasAny` / `Has` are the precedent), with `filter()`, `to_string()` (~3189), `predicate_node`
  (~3080) and the switch arms in src/index_search.cpp (~407, ~443, ~626, ~790). Row-evaluation
  semantics unchanged.
- `LIKE '%x%'` and `regexp_like` are **not** routed in N1: Lance sends them through
  `TextQuery::Regex` and `ngram/ngram_regex.rs` (`visit_like`, lance-index `scalar/expression.rs`
  ~970). A later item.
- Known pieces: `write_file` / `struct_schema` (src/index_files.cpp 183-197) drop child field metadata,
  so `posting_list` needs a schema carrying `lance-encoding:compression = none`; the inner bitmaps can
  use `roaring::encode` (roaring_bitmap.hpp:20); export a lower-case + ASCII-fold helper from
  `Analyzer::emit` (src/fts_tokenizer.cpp 395-424) and include non-ASCII cases in the byte test (its
  `kLower` table may differ from Rust's `to_lowercase` on rare characters: report any difference).

**N1a outcome (2026-10-15):** done. NGRAM build / search / `contains` routing / plan line / describe /
stats / drop / empty dataset; token rows and posting bytes equal pylance's (nulls, empty strings,
non-ASCII; a sweep of every code point U+0080..U+10FFFF found no lower-case table difference).
Reviewer: `drop_index` of a missing name now raises pylance's `RuntimeError("Lance index not found:
name=...")`. +5 pylance tests (1050). **N1b** next: `test_optimize_no_new_data`,
`test_create_index_empty_dataset` (both reach `optimize_indices` on NGRAM),
`test_optimize_indices_second_call_is_noop`, `test_ngram_segment_merge_and_commit_from_python`.

**N1b outcome (2026-10-15):** done. NGRAM `optimize_indices` (merge by default, a delta with
`num_indices_to_merge=0`; replaced segments' postings kept, deleted rows included, as Lance's
`open_and_merge_segments`), compaction re-cover, distributed segments; describe / stats / posting rows
equal pylance's after the same steps, with and without stable row ids. Reviewer: after compaction
nanolance re-indexes the rewritten fragments for every kind while pylance remaps and leaves them
unindexed -- the documented divergence stands (answers equal a scan either way); the "kind nanolance
cannot build" refusal is now tested with a pylance IVF_RQ index. +4 pylance tests (1054). N2 next.
