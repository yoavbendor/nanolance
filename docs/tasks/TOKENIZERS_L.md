# Task: the lindera tokenizer for INVERTED indexes (ROADMAP item 11, phase L)

Status: **done** (2026-10-22, Opus, not delegated). High-risk branch
`claude/nanolance-optimization-review-h3rxi7-highrisk`. ICU (phase I) stays refused by name.

## What Lance 12 does

`base_tokenizer = "lindera/<model>"` resolves `<model>` under `LANCE_LANGUAGE_MODEL_HOME` (lance-index
`tokenizer/lindera.rs`). A directory that does not exist is `ValueError` ("Invalid user input: Invalid directory
path"). If it holds `config.yml`, Lindera's `TokenizerBuilder::from_file` reads it; otherwise
`TokenizerBuilder::new()` reads `LINDERA_CONFIG_PATH`, or builds from an empty config (which has no dictionary
and fails). Every failure there is Lance's `Error::io` (`OSError`). lance-tokenizer `lindera.rs` turns Lindera's
tokens into Lance tokens: the text is the token's surface, the position is Lindera's `position` (the token's
index, whitespace tokens skipped and not counted). Lance's usual filters (lower case, length, stop words, stem,
ascii folding) apply afterwards.

Lindera 3.0.7 (lindera, lindera-dictionary), as ported in `src/fts_lindera.cpp`:
- config: a YAML mapping (serde_yaml_ng, then JSON). `segmenter.dictionary` (a path, relative to the working
  directory, or `file://`; `embedded://` fails: Lance builds Lindera without embedded dictionaries),
  `segmenter.user_dictionary` (`.csv` or `.bin`), `segmenter.mode` (`normal`, `decompose`, or
  `{decompose: {the four penalty fields}}`), `segmenter.keep_whitespace` (default false),
  `character_filters` (`mapping` implemented), `token_filters`.
- dictionary directory: `metadata.json` (serde, all fields required), `char_def.bin` and `unk.bin` (rkyv 0.8
  archives: root at the end, 32-bit relative pointers; strings inline up to 8 bytes padded with 0xFF, else
  out of line with the offset taken from the string's own field), `matrix.mtx` (`-1, forward, backward`, costs at
  `forward + backward * forward_size`; an older untransposed layout too), `dict.da` (a daachorse 2.1 bytewise
  automaton: 12-byte states `base, fail, output_pos << 8 | check`, child `base ^ byte`; 12-byte outputs
  `value, length, parent`), `dict.vals` (10-byte entries: id, cost, left id, right id), `dict.wordsidx` /
  `dict.words` (details; read, not used).
- user dictionaries: a CSV is built as `builder/user_dictionary.rs` builds it (rows stable-sorted by surface,
  entries grouped per surface in byte order, 3-field rows take the metadata's default cost and ids, longer rows
  carry their own; `flexible_csv` false means a row of another length is an error); a `.bin` is an rkyv archive
  of the prefix dictionary (5-bit entry counts).
- segmentation: sentences end after `\n`, `\t`, `。` or `、`; per sentence the forward Viterbi of `viterbi.rs`
  `set_text`, edges added in Lindera's order (per start: user dictionary entries, then system ones, each newest
  match first; then unknown words per character category, grouped as `char.def` says; in normal mode an unknown
  word is not started inside the previous one), the strict `<` keeps the first of equal-cost left edges, `i32`
  sums saturate, `Edge::num_chars` is the byte length / 3 (decompose penalties use it).

## Not implemented (refused, `NotSupportedError`)

Token filters (any `token_filters` entry) and the character filters `japanese_iteration_mark`, `regex`,
`unicode_normalize`. Each would be a port of its own; none is in Lance's tests.

## Tests

- pylance's suite: the 7 `test_*lindera*` tests in `test_scalar_index.py` (+7 expected passes, 1267). Their configs
  name `./python/tests/models/...`: Lance runs its suite from its `python/` directory, so
  `tools/pylance_suite.py` links `python -> .` in the suite's directory.
- `bindings/python/tests/test_fts_lindera.py` (12 tests) with `tests/lindera_dict.py`, a writer for Lindera 3
  dictionaries, user `.bin` files and the daachorse and rkyv formats (it re-encodes pylance's fixture
  `char_def.bin` byte for byte). Random dictionaries (random words, costs, connection costs with many zeros,
  so ties are common), random text with spaces, tabs, punctuation and other scripts, normal and decompose modes,
  with and without positions (term and phrase queries), CSV (both shapes) and `.bin` user dictionaries, YAML
  quoting / comments / flow mappings, `file://` URIs, keep_whitespace, chained mapping filters, and the error
  cases: results and scores equal across both engines and both engines' indexes. Mutating the tie rule (`<` to
  `<=`) fails 10 of the 12; pylance's own 7 still pass with it.
