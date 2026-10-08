# Full-text indexes: Lance 12's INVERTED index

What nanolance reads, searches and builds. These facts come from pylance 12.0.0 and the
`lance-index` / `lance-tokenizer` / `fst` / `lance-bitpacking` 12.0.0 crates, and were checked
against pylance:
- A numpy model, working from the index files alone, reproduced pylance's `full_text_query` scores
  bit for bit.
- nanolance's search returns pylance's rows and scores bit for bit (tests/test_fts.py).
- An index nanolance builds holds what pylance's and LanceDB's builds hold for the same data.

## Analyzer

The index's `params` (below) name the analyzer. Lance's default, and LanceDB's, is:
- `simple` splitting: runs of Unicode alphanumerics (Rust's `char::is_alphanumeric`);
- words of 40 bytes or more dropped;
- lower-casing;
- the English Snowball stemmer (frostem, Snowball 3.1.1);
- English stop words removed, after stemming (the `stop-words` crate's list, which includes single
  letters);
- ASCII folding.

Positions count every word, dropped ones included. `whitespace` splits on ASCII whitespace; `raw`
keeps the whole text as one token. A query is tokenized the same way. So `"quickly"` finds
`quick`, and `résumé` is stemmed and then folded to `resume`, which the query `resume` (stemmed to
`resum`) does not find.

nanolance's analyzer (`src/fts_tokenizer.cpp`) produces Lance's tokens byte for byte. Its Unicode
tables and stemmer are generated from the Rust crates (`tools/fts_tables`,
`tools/fts_stem_translate.py`). `tests/golden/fts_tokenizer` holds 3,000 lines tokenized by Lance.
It supports `simple`, `whitespace` and `raw` with English. Other settings are refused: `icu`,
`ngram`, `code`, `jieba/*`, `lindera/*`, other languages, and JSON documents.

## Files

An index is a directory `_indices/<uuid>/` of Lance files (format 2.2). Each column below is
non-nullable.

**`metadata.lance`** has one column, `deleted_fragments: binary`, with one row: a Roaring bitmap,
empty on a new index. Its schema metadata:

| key | value |
|---|---|
| `partitions` | the partition ids, `[0]` (Lance numbers them by build worker, e.g. `[1]`) |
| `params` | the analyzer as JSON (`lance_tokenizer`, `base_tokenizer`, `language`, `with_position`, `max_token_length`, `lower_case`, `stem`, `remove_stop_words`, `custom_stop_words`, `ascii_folding`, ngram lengths, `prefix_only`, `block_size`, and four code-tokenizer flags) |
| `format_version` | `2` |
| `token_set_format` | `fst` |
| `posting_block_size` | `128` |
| `posting_tail_codec` | `varint_delta_v1` |

Each partition `N` has three files:

**`part_N_tokens.lance`**, one row:
- `_token_fst_bytes: large_binary`: an `fst` crate (0.4.7) Map from token to token id. Token ids
  are dense, in order of first appearance.
- `_token_next_id: uint32`: the token count.
- `_token_total_length: uint64`: the tokens' total bytes.

The "arrow" token set (`_token: utf8`, `_token_id: uint32`) is read too.

**`part_N_docs.lance`** has one row per document: `_rowid: uint64` (the row address) and
`_num_tokens: uint32`. Its schema metadata holds `total_tokens`. A row without tokens (null, empty,
only stop words) is not a document. A document's id is its row in this file.

**`part_N_invert.lance`** has one row per token id:
- `_posting: list<large_binary>`: one entry per block.
- `_max_score: float`.
- `_length: uint32`: the document count.
- `_impacts: list<large_binary>`.

Its schema metadata repeats `format_version`, `posting_block_size` and `posting_tail_codec`.

### Posting blocks

A posting list is its (document id, frequency) pairs in ascending document order, 128 to a block.
Each block starts with an f32 (little-endian): the block's best score.

- A **full block** follows it with:
  1. the first document id (u32);
  2. a bit width (u8) and the document id deltas, BitPacker4x-packed: the delta from the previous
     id, the first one 0;
  3. a bit width (u8) and the frequencies, BitPacker4x-packed.
- The **last, partial block** follows it with varints: the document ids as deltas (the first one
  absolute), then the frequencies.

BitPacker4x (lance-bitpacking) lays out 128 values as four interleaved lanes. Value `i` belongs to
lane `i % 4`. Each lane is packed LSB first into 32-bit words, and word `w` of lane `j` is stored
as u32 number `4w + j`. A block of width `b` takes `16b` bytes.

**Scores in the files.** These use the partition's own statistics: `N` its documents, `avgdl` its
`total_tokens / N` in f32, and `n` the posting list's length.
- A block's score is `max(f / (f + K1 (1 - B + B dl / avgdl))) * idf(n, N) * (K1 + 1)`.
- `_max_score` is the best block score.

**Impacts.** There is one entry per block, then one per 32 blocks (after all the per-block ones).
An entry encodes its documents' (frequency, length) frontier:
1. For each frequency, keep its shortest document.
2. Keep only the pairs no other pair beats on both counts.
3. Quantize the lengths: below 8 as they are, otherwise 3 mantissa bits and a shift.
4. Merge pairs whose quantized lengths are equal.

The entry is written as the varints `last document id` and `pair count`, then per pair
`(frequency delta - 1) << 1 | explicit`, plus a byte for the length delta when it is not 1.
pylance's WAND search prunes by these entries, so they must be exact. nanolance writes them as
Lance does. It does not read them: it scores exhaustively, and stops early where that is exact
(below).

## Search

BM25 with `K1 = 1.2` and `B = 0.75`, in f32, in Lance's operation order:

    idf(n, N)       = ln((N - n + 0.5) / (n + 0.5) + 1)
    norm(dl)        = K1 * (1 - B + B * dl / avgdl)
    doc_weight(f)   = (K1 + 1) * f / (f + norm(dl))
    score           = sum over the query's tokens (repeats included) of idf * doc_weight

`N`, `n` and `avgdl` are summed over every partition (and segment) of the index.

- **Rows the index covers** score with the index's statistics. Neither deletions nor filters change
  them.
- **Rows in fragments the index does not cover** are tokenized on the fly. They score with the
  index's statistics plus their own: `N` plus their documents, `n` plus their documents with the
  token, total tokens plus theirs. Only live rows count, and with `prefilter` only rows passing
  the filter. `fast_search` leaves them out.
- **Deleted rows**, and rows of fragments no longer in the dataset, never come back.
- **Filters**: `prefilter=True` filters before the search. Otherwise the best `limit` rows are
  found, and then filtered.
- **Order**: best score first. Lance's order among equal scores is arbitrary; nanolance's is by row
  id.
- **A column without an index** is searched as Lance's flat search does it:
  - the `simple` tokenizer alone (no lower-casing, stemming, stop words, folding or length limit);
  - the documents' own statistics;
  - rows in scan order, or best first with a limit.

  A plain string query when no column has an index is an error, as in pylance.

### Queries

| query | rows | score |
|---|---|---|
| a string | `MultiMatch` over every indexed column (or `columns`) | |
| `MatchQuery(text, column, boost, operator)` | rows with any token (`OR`) or every token (`AND`) | the sum, times `boost` |
| `MultiMatchQuery(text, columns, boosts)` | any column matching | the best column's (boosted) score |
| `BoostQuery(positive, negative, negative_boost)` | the positive query's | `positive - negative_boost * negative` where both match |
| `BooleanQuery(MUST / SHOULD / MUST_NOT)` | every MUST (else any SHOULD), no MUST_NOT | the MUST and matching SHOULD scores summed |
| `PhraseQuery(text, column, slop)`, or a string in double quotes | rows with the terms in order, at most `slop` apart | the sum over the terms |

Not supported: `fuzziness` other than 0.

A phrase query needs an index built with positions (`with_position=True`); without them nanolance
gives pylance's error. As in Lance, query positions count from the first term kept (a stop word
leaves a gap). With slop 0 every term must sit at its place relative to a common start; with slop,
the terms in query order may each stand up to `slop` past the one before (Lance's `wand.rs`
`check_positions`). Rows the index does not cover use Lance's flat check (`flat_search.rs`
`phrase_matches_positions`). A quoted string over several indexed columns is pylance's error: the
column must be given.

#### Positions on disk

Index format 2 with positions adds two columns to `part_<n>_invert.lance`, one row per token:
`_compressed_position` (large_binary) and `_position_block_offset` (list<uint32>), the byte
offset of each 128-document posting block in the stream. The metadata names the layout
(`positions_layout` = `shared_stream_v2`) and codec (`positions_codec`). For a block, each
document's positions are deltas (the first one absolute), concatenated over the block's documents.
With `packed_delta_v1` they go in groups of 128 (`[bit width][BitPacker4x bits]`, as posting
blocks are) and the tail as varints; `varint_doc_delta_v2` is varints only. nanolance reads both
and writes `packed_delta_v1`, as Lance 12 does. Other layouts (older per-document positions) are
refused for phrases only; plain matches still work.

### How nanolance searches (`src/fts_search.cpp`)

- An index's token map, document lengths and decoded posting lists are cached. The cache is keyed
  by the files' size and modification time.
- A match scores every posting into a dense per-document array, then reads it back in row order.
- A phrase is a match with every term required. Before the rows are read back, each candidate's
  positions are checked; a term's positions are decoded once and cached with its posting list.
- When only the best `limit` rows of a match matter, it keeps a heap of them. For one word, it
  walks the posting list's runs of 128 in the order of their best document weight, and stops at
  the first run that cannot beat the rows kept. That bound is exact in f32, because rounding is
  monotone.

## Building (nanolance)

`create_scalar_index(column, "INVERTED" | "FTS", name=None, replace=True, **params)`
(`src/fts_index_build.cpp`):
1. Scan the column's live rows with their addresses, fragment by fragment.
2. Tokenize them in parallel.
3. Give token ids in order of first appearance.
4. Write one partition, `part_0`, with the files above. The block scores, `_max_score` and impacts
   are Lance's. The FST is built by a port of the `fst` crate's builder (`src/fts_fst.cpp`),
   checked byte for byte against the crate (`tests/golden/fts_fst`).
5. Commit the manifest entry: an `InvertedIndexDetails` (base tokenizer; the language as a JSON
   string, `"English"`; the analyzer flags; `block_size` 128; `posting_format_version` 2), with
   index version 2.

For the same data, the files hold what pylance's and LanceDB's builds hold, byte for byte. Two
things can differ: the partition number, and document order when Lance's workers take fragments
out of order. Integer columns are bit-packed as Lance's are, and carry a `nanolance:packing` tag
in their field metadata. pylance and LanceDB list and search the index as their own, and pylance's
`optimize_indices` merges new rows into it.

With `with_position=True` the index also stores positions, as Lance 12 writes them
(`packed_delta_v1`), with the same contents as pylance's build; pylance's phrase queries use it.

Not built: posting blocks of 256 (index format v3), and the tokenizers listed above.
