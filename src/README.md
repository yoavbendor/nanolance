# nanolance sources, by layer

Each directory below is one layer, lowest first. A layer may use the layers below it and never one above it
(an `#include` of a higher layer's header is a layering violation). The layers follow the Lance format and
Lance's own crates (lance-io, lance-encoding, lance-file, lance-table, lance-index, lance), so each part of
the format has one home here.

`tools/layout.py` is the source of truth: which layer every file of `src/` and every header of
`include/nanolance/` belongs to. `tools/check_layers.py` checks the includes against it (CI runs it);
`tools/layers_allow.txt` lists the violations that remain, to be removed, never added to.

| Layer | Directory | The question it answers | Files | Lines |
|---|---|---|---|---|
| base | `src/base/` | threads, statistics, safety limits, small numerics and helpers | 9 | 1,352 |
| io | `src/io/` | where bytes live: local files and object stores (S3 is a registered backend) | 6 | 1,056 |
| arrow | `src/arrow/` | Arrow arrays and schemas, and how they map to Lance's types | 5 | 3,491 |
| encoding | `src/encoding/` | how one page of one column is laid out (both directions) | 8 | 9,921 |
| file | `src/file/` | how a .lance file is framed: footer, column metadata, global buffers, blobs | 4 | 5,033 |
| table | `src/table/` | what a version is: manifest, fragments, deletions, row ids, transactions, refs, index metadata | 18 | 8,730 |
| expr | `src/expr/` | what a filter means (no storage) | 1 | 5,392 |
| scan | `src/scan/` | reading rows of fragments, given a row selection | 1 | 3,967 |
| index | `src/index/` (`fts/`, `scalar/`, `vector/`) | each index kind: build, look up, describe | 24 | 35,406 |
| dataset | `src/dataset/` | user operations: write sessions, updates, merge_insert, compaction, index lifecycle, query planning | 8 | 8,286 |
| api | `src/api/` | the C APIs and lance-c | 4 | 5,441 |

The index line counts include about 18k lines of generated tables (`index/fts/*.inc`: Unicode and stop-word
tables, the snowball stemmer, jieba's HMM model).

Public headers stay in `include/nanolance/` (the C APIs `nano_lance_reader.h`, `nano_lance_writer.h`
and `lance_c.h` among them); private headers live beside their sources and are included by their path below
`src/`, e.g. `#include "index/fts/fts_fst.hpp"`.
