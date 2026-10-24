# Task: full-text search over list<string> columns (row and list-element documents)

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). ROADMAP "Index plan
for AI users", Wave 1, item 1. Two sessions: **L1** (row granularity), then **L2** (list elements).
Do only the phase you are given.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k:
- Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'` windows of at most ~120 lines. Big
  ones: `src/index/fts/fts_index_build.cpp`, `src/index/fts/fts_search.cpp` (4.7k lines between the fts_* files),
  `bindings/python/nanolance/lance/dataset.py` (3.5k+), and Lance's Rust (read only what is named).
- Cut output: `| tail -15` for pytest (add `-o addopts=`), `| tail -5` for `tools/pylance_suite.py`,
  `ninja 2>&1 | grep -E "error|warning:" | head`.

## 1. Goal

pylance builds and searches INVERTED indexes on `list<string>`, `list<large_string>`,
`large_list<string>` and `large_list<large_string>` columns. nanolance refuses them today:
`an INVERTED index needs a string column; text is not one` (`src/index/fts/fts_index_build.cpp` about line 715),
`full-text search needs a string column` (`src/index/fts/fts_search.cpp` about line 1274), and
`full-text search over list elements` (`bindings/python/nanolance/lance/query.py` about line 50).
Target: pylance's `test_fts_on_list` (L1) and the four `test_fts_on_list_elements[*]` (L2), in
`.deps/pylance-tests/v12.0.0/tests/test_scalar_index.py` (lines 1755-1900): read them first.

## 2. Facts (measured on pylance 12.0.0 -- do not re-derive)

Re-run any time: `python3 /tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/ftslist/measure.py lance OUT.lance`
(and with `nanolance` as the first argument for nanolance's output). Data: `id` 0-2, `tags` =
`[["alpha beta","gamma alpha",None,"","delta"], ["beta","gamma"], None]`.

**Row granularity (L1)** -- the default, `document_granularity` absent or `ROW`:
- One document per row; a null row (row 2) has no document. The row's elements are tokenized in order
  as one token stream: positions continue across elements with no gap (phrase `"beta gamma"` matches
  row 0, where "beta" ends element 0 and "gamma" starts element 1). Null and empty elements add no
  tokens. Row 0 has `_num_tokens` 5, row 1 has 2; `total_tokens` 7.
- Files and metadata are the same as for a string column (`part_0_docs.lance` = `_rowid`,
  `_num_tokens`; same `params`, `format_version` "2"). Index name `tags_idx`.
- Results: `MatchQuery("alpha", "tags")` -> row 0, score 0.850555. `PhraseQuery("beta gamma", "tags",
  document_granularity=ROW)` -> rows 1 (0.442166) and 0 (0.310249). No `_doc_index` column.
- An unindexed list column (and rows appended after the index) is searched flat: `test_fts_on_list`
  appends the same 5 rows and expects 6 hits for "lance".

**List-element granularity (L2)** -- `document_granularity=DocumentGranularity.LIST_ELEMENT`:
- One document per element, null and empty elements included (with 0 tokens); a null row has none.
  `part_0_docs.lance` has a third column `_doc_index_0: uint32 not null` (the element's position in the
  list): rows `(0,2,0) (0,2,1) (0,0,2) (0,0,3) (0,1,4) (1,1,0) (1,1,1)` as `(_rowid, _num_tokens,
  _doc_index_0)`. Positions restart at 0 in each element. Same `params`.
- The layout is Lance's INVERTED version 3 for this granularity (lance-index 12.0.0
  `src/scalar/inverted/index/inverted_index.rs` `index_version`, about line 59; the docs schema in
  `src/scalar/inverted/index/format.rs` about line 88). Lance's sources are under
  `/root/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/lance-index-12.0.0/`.
- The manifest records it: `InvertedIndexDetails.document_granularity` = field 14, enum `ROW = 0`,
  `LIST_ELEMENT = 1` (`protos/index_old.proto` line 113). nanolance already decodes field 14 for
  `describe_indices` (`dataset.py` about line 1795). Default index name `tags_list_element_idx`.
- Results carry `_doc_index: list<uint32>` (one entry per hit, the element position), one result row
  per matching element: `MatchQuery("alpha", ..., LIST_ELEMENT)` -> `(0,[0],0.825462)`,
  `(0,[1],0.825462)`; `PhraseQuery("alpha beta", ..., LIST_ELEMENT)` -> `(0,[0],1.650924)`;
  `PhraseQuery("beta gamma", ..., LIST_ELEMENT)` -> no rows.
- Without a granularity in the query, the one index on the column decides it. Errors (Python
  `ValueError`; drop Lance's `, /home/runner/...` suffix):
  - both indexes exist: `Invalid user input: FTS query for field 'tags' is ambiguous because Row and
    ListElement indexes coexist: 'tags_idx' (Row), 'tags_list_element_idx' (ListElement); specify
    document_granularity`;
  - only the element index exists and the query asks for ROW: `Invalid user input: FTS query for field
    'tags' requested Row document granularity, but the existing FTS index uses a different granularity:
    'tags_list_element_idx' (ListElement)`.
- Flat (unindexed) search honours the granularity too (lance-index
  `src/scalar/inverted/index/flat_search.rs` about lines 600-660 and 890-930).

## 3. Where things are in nanolance

- Build: `src/index/fts/fts_index_build.cpp` (the column-type check about line 715; the docs writer: `grep -n
  "_num_tokens" src/index/fts/fts_index_build.cpp`). Search: `src/index/fts/fts_search.cpp` (flat search over unindexed
  fragments and the column check about line 1274). Tokenizer: `src/index/fts/fts_tokenizer.cpp`.
- Python: `bindings/python/nanolance/lance/query.py` (`DocumentGranularity`, `_granularity`, the
  MatchQuery / PhraseQuery constructors), `dataset.py` (`create_scalar_index` and its parameters about
  line 1337; `_fts_query_json`; the `_score` column). `bindings/python/src/nanolance_bindings.cpp` passes
  the query JSON to C++.
- Existing tests to follow: `bindings/python/tests/test_fts.py`, `test_fts_phrase.py`.

## 4. Phases

- **L1**: list columns at row granularity: build (index files equal to pylance's row for row: docs,
  tokens, postings, positions), indexed and flat search, phrase queries, append then query.
  `test_fts_on_list` passes. Stop there.
- **L2**: list-element granularity: `document_granularity` in `create_scalar_index`, `MatchQuery`,
  `PhraseQuery` and `IndexConfig` parameters; the v3 docs layout; field 14 in the manifest; `_doc_index`
  in results; the two errors; flat search; prefilter. The four `test_fts_on_list_elements[*]` pass.
  Also in L2 (found in L1, string columns too): a phrase query over rows no index covers (an unindexed
  column, or rows appended after the index) fails with "position is not found but required for phrase
  queries", where pylance answers it. Flat search must tokenize with positions for a phrase query, and
  the planner must accept unindexed phrase columns; compare with pylance on a string column and a list
  column, indexed + appended rows.

## 5. Tests you write

`bindings/python/tests/test_fts_lists.py` (style of `test_fts.py`, `require_pylance()`), each case on
both engines: (a) the index files each engine builds are equal row for row (docs, tokens; postings and
positions decoded); (b) each engine searches the other's index with the same rows, `_doc_index` and
scores (scores bit for bit, as for string columns); (c) after an append and after `optimize_indices`,
the same results; (d) all four list types; nulls, empty strings and empty lists.

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. C++
without warnings (CI uses `-Werror`). Indexes nanolance builds on string columns today must not change
(byte for byte). FTS on string columns must not get slower: run `python3 tools/bench_fts.py --rows
100000 --work /tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fl/bench
--out /tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fl/after.json` once
at the end and report its build and search lines (the reviewer compares). Do not commit, push, change
branches, or run `--update`. Scratch files only in
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fl/`. Builds: `cd build &&
ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`. Run
each full suite once, at the end, one at a time; never two at once; no sleep loops; kill by PID.

## 7. Stop and ask (question first in the report)

- your index files differ from pylance's in a way the Rust source does not explain;
- supporting lists needs a change to how string-column indexes are built or scored;
- a score differs from pylance's beyond the last bit.

## 8. Done and report

Tails of: `cd build && ctest -j8 | tail -3`; `python3 -m pytest -o addopts= -q
bindings/python/tests/test_fts_lists.py | tail -2`; the target pylance tests run directly with pytest
in `.deps/pylance-tests/v12.0.0/tests` (see `tools/pylance_suite.py --help` for how it points pylance's
tests at nanolance); `python3 tools/pylance_suite.py | tail -4`; the benchmark lines; last, once: `cd
bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -3`. Report under
40 lines: questions first, then files, per-test status, tails, decisions.

**L1 outcome (2026-10-19):** done in one Haiku session; reviewed. A list row is its non-null elements
joined by one space, then tokenized as one text (Lance's `materialize_string_list`, builder.rs line 1562:
the same join, empty elements included); index files equal pylance's row for row, scores bit for bit
in both directions. String-column FTS unchanged in speed (interleaved A/B, 100k documents: build 0.99x,
searches within run-to-run noise). `test_fts_on_list` passes; the flat phrase gap moved to L2.

**L2 outcome (2026-10-19):** done in one Haiku session (about 300k tokens, over budget); reviewed.
List-element documents (one per element, null and empty ones included; the v3 docs layout with
`_doc_index_0`; `InvertedIndexDetails.document_granularity`), `_doc_index` in results, pylance's three
granularity errors, element-aware flat search, and phrase queries over rows no index covers (string and
list columns). Element documents beside unindexed ones are scored with the combined statistics, as
pylance does (measured; row documents keep the index's). All four `test_fts_on_list_elements[*]` pass.
Review: string-column indexes byte-identical before and after (with and without positions); search
instructions equal (callgrind, 168.7M vs 167.8M per run) and wall times equal on longer interleaved runs.
Left open (ROADMAP small items): `IndexConfig(parameters=...)` is ignored for INVERTED builds other than
list-element ones (pre-existing); staged distributed builds over list elements are refused;
`infer_index_segment_details` would drop field 14.
