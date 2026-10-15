# Task: INVERTED index format versions 1 / 3, 256-document posting blocks, the code analyzer

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Roadmap item 5
(scalar / FTS breadth). Three phases, each a fresh session: **F1**, **F2**, **F3** (section 2).

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`src/fts_search.cpp` (2k lines), `src/fts_index_build.cpp`, `dataset.py` (3.5k+) or Lance's
`tokenizer.rs` (1.8k) whole. Cut all output (`| tail -15`; pytest with `-o addopts=` for a summary
line; `tools/pylance_suite.py ... | tail -5`; `ninja 2>&1 | grep -E "error|warning:" | head`). Only
your phase. Lance's Rust source is the reference, unpacked here:
- `.deps/rust-suite/lance-index-12.0.0/src/scalar/inverted/` (`encoding.rs`, `index/format.rs`,
  `tokenizer.rs`, `builder.rs`, `index.rs`, `impact.rs`);
- `.deps/rust-suite/lance-tokenizer-12.0.0/src/` (`code_tokenizer.rs` 191 lines,
  `word_delimiter_filter.rs` 297 lines -- read these two whole in F3, nothing else whole).
When unsure, build the same index with real pylance (`import lance` in plain `python3` is pylance
12.0.0) and compare. A probe that dumps every variant's metadata:
`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fts3/probe.py` (copy it to
your own scratch dir; it writes `ds_*` next to itself).

## 1. Facts (measured on pylance 12.0.0 here)

nanolance today builds and reads **format_version 2** only: posting blocks of 128 documents,
`posting_tail_codec = varint_delta_v1`, positions in `positions_layout = shared_stream_v2`. Python
refuses `format_version`, `block_size`, `analyzer`, `split_identifiers`, `split_on_numerics`,
`preserve_original`, `index_operators` (`_INVERTED_OPTIONS` / `_inverted_params`,
`bindings/python/nanolance/lance/dataset.py` ~1015); the native side refuses block size != 128
(`src/fts_index_build.cpp` ~561); the reader refuses format != "2" and block != "128"
(`load_inverted`, `src/fts_search.cpp` ~716).

Lance's rules (`index/format.rs`, `tokenizer.rs`):
- `format_version` accepts `1 | 2 | 3 | "1" | "v1" | "V1" | ...`; anything else -> ValueError
  `unsupported FTS format version {v}, expected 1, 2, or 3`. `block_size` is 128 or 256, else a
  ValueError naming `block_size` (copy Lance's `invalid_block_size_message`).
- Default format: 2 with block 128, **3 with block 256**, 3 with the code analyzer. format 1 / 2 with
  block 256 -> `FTS format_version={1|2} is incompatible with block_size=256; use format_version=3`.
- The manifest index's `index_version` (what `describe_indices()[i].segments[0].index_version` shows)
  = the format version (1, 2 or 3).
- Files are the same four (`metadata.lance`, `part_N_{docs,invert,tokens}.lance`) for every version.
  `metadata.lance` keys: `format_version`, `partitions`, `params`, `posting_block_size`,
  `posting_tail_codec`, `token_set_format = fst`, plus `positions_layout` / `positions_codec` only when
  `with_position` and format >= 2. `invert.lance`'s schema metadata repeats `format_version`,
  `posting_block_size` and (format >= 2) `posting_tail_codec`.
- **V3 with block 128 is byte-identical to V2** except `format_version = "3"` (docs / tokens files
  equal, invert differs only in its metadata) -- verified.
- **V1** = `posting_tail_codec = fixed32_v1` (tail block stores doc ids / frequencies as plain u32;
  nanolance's decoder already has `TailCodec::Fixed32`, `src/fts_posting.cpp` ~111 -- but the reader
  looks for the string `"fixed32"`, pylance writes `"fixed32_v1"`: fix it). With `with_position`, V1
  has **no** `positions_layout`: `invert.lance` has an extra column
  `_position: list<list<large_binary>>` -- per posting list, per document, a compressed position
  block (Lance: `COMPRESSED_POSITION_COL` / `_position` in `index.rs` / `builder.rs`; grep
  `compress_positions\|POSITION_COL` there). nanolance currently reports that layout as unsupported
  (`positions_error`); F1 reads and writes it.
- **Block 256** (`encoding.rs` 66-330): full blocks are `BitPacker8x` (8 lanes x 32, same scheme as
  the 4x one in `src/fts_posting.cpp`, 8 interleaved lanes instead of 4): doc ids as a sorted block
  `[u32 first doc][u8 bits][packed deltas]`, then frequencies **patched FOR**
  `[u8 width][u8 exception count][packed body][exceptions: (u8 index, varint high bits)...]`
  (`compress_pfor_block_with` -- copy its width choice exactly so bytes match). **No 4-byte block max
  score prefix** on any 256 block, including the remainder (`posting_block_score_prefix_len`). The
  tail (remainder < 256) uses the tail codec as today. Impact skip data (`_impacts`) uses the same
  entry encoding for every block size, one entry per posting block and one per 32 blocks
  (`impact.rs` top; nanolance's `encode_impact_entry` in `src/fts_index_build.cpp` ~100-130).
- Code analyzer (`analyzer="code"`, or `base_tokenizer="code"`): params JSON written into `params`:
  `"base_tokenizer":"code"`, `"stem":false`, `"remove_stop_words":false`, `"lower_case":true`,
  `"ascii_folding":true`, and four extra keys (nanolance already writes them, as false):
  `"split_identifiers"` (default false), `"split_on_numerics"` (default true), `"preserve_original"`
  (default true), `"index_operators"` (default false). Pipeline (`tokenizer.rs` ~1081):
  `CodeLexTokenizer(index_operators)` then, only if `split_identifiers`,
  `WordDelimiterFilter(preserve_original, split_on_numerics)`, then the usual filters (lower case,
  ascii folding, max token length). Errors (copy the messages): flags without `analyzer='code'`
  -> `code analyzer flags require analyzer='code'`; code with format 1 / 2 ->
  `base_tokenizer='code' requires FTS format_version=3, got 2`; `base_tokenizer='code'` with
  `analyzer='text'` and the other two in `RawInvertedIndexParams::resolve` (~286-335). `stats`
  `index_stats(...)["indices"][0]["params"]` has no `"analyzer"` key, has the four flags. The index
  details proto (`InvertedIndexDetails`) carries a `code_config` for code indexes (`tokenizer.rs`
  ~420; find nanolance's writer with `grep -n "InvertedIndexDetails" src/*.cpp`) -- compare the
  manifest bytes with pylance's.

## 2. Phases

- **F1 -- format_version 1 / 2 / 3 at block 128.** Python options `format_version`, `block_size`
  (only 128 here; 256 -> stop at the F2 boundary with a clear NotSupported until F2), validation and
  messages; `index_version` in the manifest; build V1 (fixed32 tail, legacy `_position`) and V3/128;
  read pylance's V1 (with and without positions, phrase queries on it) and V3/128. Oracle:
  `test_describe_indices`, `test_create_inverted_index_rejects_invalid_format_version`.
- **F2 -- block 256.** BitPacker8x + patched FOR, read and write, byte-identical to pylance on the
  same data (one partition: compare `part_0_invert.lance` columns). Oracle:
  `test_create_scalar_index_fts_block_size`, `test_code_analyzer_does_not_split_identifiers_by_default[256]`
  passes only after F3.
- **F3 -- code analyzer.** Tokenizer in `src/fts_tokenizer.cpp` (C++ port of the two Rust files;
  same token texts and positions), options, errors, params, details proto; query-side tokenization
  through the same analyzer. Oracle: all 8 `test_code_analyzer_*` (grep `def test_code_analyzer`
  in `.deps/pylance-tests/v12.0.0/tests/test_scalar_index.py`).

Run pylance's tests through the runner (it loads nanolance as `lance`):
`python3 tools/pylance_suite.py -k "describe_indices or format_version or fts_block_size or code_analyzer" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(a `tests/test_datagen.py` collection error shows with `-k`; ignore it).

## 3. Tests you write

Extend `bindings/python/tests/test_fts.py` (grep its existing pylance-comparison helpers and follow
them; if there is no such file use the FTS test file that exists): per phase, each engine builds,
the other searches (match, phrase where positions exist, AND/OR), same rows and scores within 1e-5;
metadata keys / values equal; index files byte-equal where section 1 says they are. Code analyzer:
token lists of the examples in `code_tokenizer.rs` / `word_delimiter_filter.rs` doc tests equal.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7 (grep the headings). No skipped / weakened tests. C++ without warnings.
Indexes nanolance builds today (format 2, block 128) must stay byte-identical. Do not commit, push,
change branches, or run `--update`. Builds: `cd build && ninja`, then
`cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) if pylance's files differ from section 1, if V1's legacy position
encoding needs more than ~150 lines, if 256-block bytes cannot be matched after comparing with
pylance's, or if the code analyzer needs Unicode tables nanolance lacks (`is_alphanumeric` on
non-ASCII: check what `src/fts_tokenizer.cpp` uses for the simple tokenizer first and reuse it).

## 5. Done per phase -- report (under 40 lines, sent once)

Tails of: `cd build && ctest -j8 | tail -3`; the oracle `-k` run; your test file;
`python3 tools/pylance_suite.py | tail -4` ("all N expected tests passed"); last, once, quiet:
`cd bindings/python && python3 -m pytest -o addopts= -q -x -p no:cacheprovider tests | tail -3`.
Files changed, per-test status, decisions, questions.

**F1 outcome (2026-10-15):** done. `format_version` 1 / 2 / 3 and `block_size` options with Lance's
checks and messages; V1 (fixed32_v1 tail, legacy per-document `_position`) and V3/128 built and read,
phrase queries on V1 and V3 positions; manifest `index_version` = format; `describe_indices` details
for INVERTED; format-2 builds byte-identical to before. Notes for F2/F3: cross-engine checks compare
decoded columns and metadata (the Lance file writer's bytes differ from nanolance's even for format 2);
pylance's `tokens.lance` token order varies between multi-fragment builds, so index-content
comparisons use one fragment. Reviewer: the six `test_describe_indices[*]` pass every FTS assertion
and then need JSON and NGRAM indexes (separate roadmap items). +1 pylance test (1036).
