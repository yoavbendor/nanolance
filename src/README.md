# nanolance sources, by layer

Each directory below is one layer, lowest first. A layer may use the layers below it and never one above it
(an `#include` of a higher layer's header is a layering violation). The layers follow the Lance format and
Lance's own crates (lance-io, lance-encoding, lance-file, lance-table, lance-index, lance), so each part of
the format has one home here.

`tools/layout.py` is the source of truth: which layer every file of `src/` and every header of
`include/nanolance/` belongs to. `tools/check_layers.py` checks the includes against it (CI runs it);
`tools/layers_allow.txt` lists the violations that remain, to be removed, never added to.

| Layer | Directory | The question it answers |
|---|---|---|
| base | `src/base/` | threads, statistics, safety limits, small numerics and helpers |
| io | `src/io/` | where bytes live: local files and object stores (S3 is a registered backend) |
| arrow | `src/arrow/` | Arrow arrays and schemas, and how they map to Lance's types |
| encoding | `src/encoding/` | how one page of one column is laid out (both directions) |
| file | `src/file/` | how a .lance file is framed: footer, column metadata, global buffers, blobs |
| table | `src/table/` | what a version is: manifest, fragments, deletions, row ids, transactions, refs, index metadata |
| expr | `src/expr/` | what a filter means (no storage) |
| scan | `src/scan/` | reading rows of fragments, given a row selection |
| index | `src/index/` (`fts/`, `scalar/`, `vector/`) | each index kind: build, look up, describe |
| dataset | `src/dataset/` | user operations: write sessions, updates, merge_insert, compaction, index lifecycle, query planning |
| api | `src/api/` | the C APIs and lance-c |

A lower layer reaches a higher one only through something the higher one hands it. The scan layer, for
one, does not know the indexes: a `LanceScanRequest` carries the index selector as a function pointer.
`lance_dataset_scan` and `LanceTableStream::open_request` are defined in the dataset layer
(`dataset/dataset_scan.cpp`) and fill it in, so filtered scans use the scalar indexes by default.
`lance_dataset_scan_core` and `open_request_core` are the scan layer's own and consult only a selector the
request names. A reader that calls only the `_core` entry points links no index code (about 1 MB less in a
Release build).

`python3 tools/check_layers.py` prints each layer's files and lines. About 18k of the index layer's lines are
generated tables (`index/fts/*.inc`: Unicode and stop-word tables, the snowball stemmer, jieba's HMM model).

Public headers stay in `include/nanolance/` (the C APIs `nano_lance_reader.h`, `nano_lance_writer.h`
and `lance_c.h` among them); private headers live beside their sources and are included by their path below
`src/`, e.g. `#include "index/fts/fts_fst.hpp"`.
