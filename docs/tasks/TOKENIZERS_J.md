# Task: the jieba tokenizer for INVERTED indexes (ROADMAP item 11, phase J)

Status: **done** (2026-10-22, Opus; a delegated start was stopped and replaced). High-risk branch
`claude/nanolance-optimization-review-h3rxi7-highrisk`. Item 11 is icu / jieba / lindera; this phase is jieba
only. ICU (phase I) and Lindera (phase L) are not part of it: keep refusing them by name.

## 0. Context budget (read this first)

Budget about 200k tokens; aim for 120k. Never read a large file whole: `grep -n`, then `sed -n 'A,Bp'` windows of
at most ~120 lines. Big ones: `bindings/python/nanolance/lance/dataset.py` (5k+), `src/fts_index_build.cpp`,
`src/fts_search.cpp`, jieba-rs `src/lib.rs` (68 KB). Cut output: `| tail -15` for pytest (add `-o addopts=`).

## 1. Targets

`.deps/pylance-tests/v12.0.0/tests/test_scalar_index.py`: `test_jieba_tokenizer`,
`test_jieba_invalid_user_dict_tokenizer`, `test_jieba_invalid_main_dict_tokenizer`, `test_jieba_user_dict_tokenizer`
(and any other test there that uses `base_tokenizer="jieba..."`: grep). Their models are in
`.deps/pylance-tests/v12.0.0/tests/models/jieba/` (`LANCE_LANGUAGE_MODEL_HOME` points there).

## 2. Sources (read-only copies -- port from these, never from memory)

`/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/crates/` (re-download with
`curl -sSfL https://static.crates.io/crates/<name>/<name>-<version>.crate | tar -xz` if gone):
- `jieba-rs/jieba-rs-0.10.0/src/lib.rs`: `with_dict` / `load_dict` / `add_word` (dictionary parsing, the frequency
  total, `log` of probabilities), `dag`, `calc` (the route: ties and the float arithmetic exactly), `cut_internal`
  (the block regexes `RE_HAN_DEFAULT` / `RE_SKIP_DEFAULT` -- line ~125 -- and what happens to skipped text),
  `cut_dag_hmm`, `cut_for_search` and `tokenize(.., TokenizeMode::Search, hmm = true)` (the 2- and 3-character
  grams, token start / end in characters).
- `jieba-rs/jieba-rs-0.10.0/src/hmm.rs`: Viterbi (`MIN_FLOAT`, `ALLOWED_PREV_STATUS`, the order states are tried
  in, how ties are broken), `RE_SKIP`.
- `jieba-macros/jieba-macros-0.10.0/src/hmm.model`: the built-in HMM (initial, transition, emission
  probabilities) that jieba uses when no model is set (Lance sets none), and `src/lib.rs` (how it is parsed).
- `lance-tokenizer/lance-tokenizer-12.0.0/src/jieba.rs`: how jieba's tokens become Lance tokens (offsets, `position =
  token.start` in characters, `position_length`).
- `lance-index/lance-index-12.0.0/src/scalar/inverted/tokenizer/jieba.rs`: `config.json` (`main`, `users`),
  `dict.txt` by default, the errors; and `tokenizer.rs` / `document_tokenizer.rs` next to it: how
  `base_tokenizer = "jieba/<model>"` resolves `<model>` under `LANCE_LANGUAGE_MODEL_HOME` (and its default when
  unset), which filters (lower_case, stem, stop words, ascii folding, max_token_length) apply after jieba.
- Licences: jieba-rs and jieba-macros are MIT (`Cargo.toml`). Copy their licence text and copyright lines from the
  crates' files (if a crate ships no LICENSE file, from the `repository` its Cargo.toml names, at the tag of that
  version) into `THIRD-PARTY-LICENSES.md`, never from memory.

## 3. Measure first (pylance 12.0.0 is the oracle)

Probes in `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/j-work/`:
1. With jieba-rs's own full dictionary (`jieba-rs-0.10.0/src/data/dict.txt`, 5 MB) as a model directory
   (`<dir>/jieba/full/dict.txt`), pylance's index over a few thousand mixed Chinese / ASCII / punctuation / emoji /
   number sentences: the token list of each document (pylance's `tokenize` helpers, or read its index's
   `part_0_tokens.lance` and the posting lists), and full_text_query results with scores.
2. The same with the tests' tiny dictionary (8 entries: the HMM decides most words).
3. What pylance writes in the index metadata (params JSON) for `base_tokenizer="jieba/default"`, and where it looks
   for the model at search time.

## 4. Implementation

- C++ (`src/fts_tokenizer.cpp` and a new `src/fts_jieba.cpp`): the dictionary (a trie or hash of prefixes: jieba-rs
  uses a double-array trie; any structure giving the same DAG is fine), the DAG, the route, the HMM Viterbi, search
  mode, the block regexes written as character-class code (no `<regex>`). Every float operation in the order
  jieba-rs does it (`f64`).
- The built-in HMM model: embedded zstd-compressed (nanolance already links zstd), parsed on first use, behind a
  CMake option `NANOLANCE_WITH_JIEBA` (default ON; the wasm build sets it OFF: there a jieba index is refused by
  name). Report the size the embedded model adds to the Python extension.
- Models: `base_tokenizer="jieba/<name>"` loads `<LANCE_LANGUAGE_MODEL_HOME>/jieba/<name>/` (config.json or
  dict.txt) when an index is built and when it is searched (as Lance does: measure 3.3); errors as pylance raises them
  (the targets: OSError for a missing user or main dictionary).
- Both directions: nanolance searches a jieba index pylance built, pylance searches one nanolance built, results and
  scores equal.

## 5. Tests you write

`bindings/python/tests/test_fts_jieba.py` (`require_pylance()`): 3.1 and 3.2 through both engines (token lists equal
document by document; queries' rows and scores equal to 1e-6), each engine searching the other's index, user
dictionaries, the error cases. Point `LANCE_LANGUAGE_MODEL_HOME` at a temporary directory you build in the test
(copy the 5 MB dictionary in only if the test can find the crate copy; otherwise use a dictionary the test writes:
no test may download).

## 6. Rules

`AGENTS.md` sections 5 and 7. No skipped or weakened tests; no `except Exception` around failures. The local native
build has `-DNANOLANCE_WERROR=ON`: delete helpers you stop using. Check the wasm build still configures and builds
with jieba OFF (`source /home/user/emsdk/emsdk_env.sh` if present; else say so in the report). Do not add fields to
Python classes that mirror pylance's. Do not commit, push, change branches, or run `--update`. **Every other
tokenizer's output must stay byte-identical** (the existing FTS tests must pass unchanged). Builds:
`cd build && ninja`, then `cd bindings/python && python3 -m pip install -q -e .` (always `python3 -m pip`). Run each
full suite once, at the end; the bindings suite with `| grep -E "^(FAILED|ERROR)|passed|failed" | tail -15`.

## 7. Stop and ask (question first in the report)

- a token list differs from pylance's and the jieba-rs source does not explain it;
- the embedded model would add more than 1 MB to the Python extension;
- a pylance test that passes today fails.

## 8. Done and report

Tails of: `cd build && ctest -j4 | tail -3`; your new tests; the targets through nanolance (`cd
/home/user/nanolance/.deps/pylance-tests/v12.0.0 && python3 -m pytest -o addopts= -q -p
nanolance.lance._pytest_plugin <target ids> | tail -5`); `python3 tools/pylance_suite.py | tail -4` (add the passing
targets to `tests/pylance_suite/expected_pass.txt` by hand); last, once, the full bindings suite. Report under 40
lines: questions first, then files, per-target status, tails, sizes, decisions.

**J outcome (2026-10-22): done.** `src/fts_jieba.cpp` ports jieba-rs 0.10 (dictionary loading, the DAG and
route, cut_dag_hmm, the HMM Viterbi with jieba-macros's built-in model, cut_internal, cut_for_search) with its
comparisons and float order; `base_tokenizer="jieba"` / `"jieba/<model>"` loads `<LANCE_LANGUAGE_MODEL_HOME or the
local data dir>/lance/language_models/jieba/<model>` (dict.txt, or config.json's main and users), cached by
file stamps; each token is emitted at its first character's offset, through Lance's filters. The HMM model is
embedded zstd-compressed (170 KB; `tools/fts_jieba_hmm.py`, checked byte-equal to a fresh packing),
`NANOLANCE_WITH_JIEBA` (ON; OFF for wasm). Measured against pylance: per-document token sets equal for 60
documents x {jieba-rs's full 5 MB dictionary, the tests' 8-word one} x {filters on, off}; 1112 + 800 query
answers (term and phrase, both engines, each searching the other's index) equal. The 4 pylance targets pass
(expected list 1259). **pylance 12 aborts the process** (a 96 GiB allocation) on some jieba queries, on its own
index and on nanolance's alike (4 of 200 random queries, e.g. 习以成风, 不食烟火: search-mode tokens step back
in position); nanolance answers them. The tests take pylance's answers in a child process for that reason.
