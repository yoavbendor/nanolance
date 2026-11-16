# ndjson2lance

A mixed stream of JSON-lines records goes in; one clean Lance table per record shape comes out.
Logs of several kinds in one pipe (`http_request`, `db_query`, `metric`, ...) each get their own
table, with a schema inferred from their first rows, and a catalog describes the tables.

## Build

Needs the tool's dependencies fetched at configure time: simdjson_arrow v0.1.5 (which brings
simdjson 5.x) and CLI11 (from the top-level build, `NANOLANCE_BUILD_TOOLS=ON`).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNANOLANCE_BUILD_NDJSON2LANCE=ON
ninja -C build ndjson2lance
```

The option is off by default. `NANOLANCE_BUILD_EXAMPLES` is not needed.

## Quick start

```
python3 examples/ndjson2lance/tests/gen_logs.py --seed 1 | ./build/examples/ndjson2lance/ndjson2lance out/
```

`out/` must not exist or must be empty. It then holds one `<table>.lance` per record shape,
`_rejects.lance` (lines that could not be stored, with the reason) and `_catalog.lance` (one row
per table). Read a table back with pylance or with nanolance's Python package:

```python
import lance, nanolance, pyarrow as pa
print(lance.dataset("out/_catalog.lance").to_table().to_pylist())
rows = lance.dataset("out/t01_buckets_detail_event.lance").to_table()     # pylance
rows = pa.table(nanolance.read_table("out/t01_buckets_detail_event.lance"))  # nanolance
```

Route by one field instead of by the set of keys with `--route-key event`.

## Flags

| flag | default | meaning |
|---|---|---|
| `out_dir` | required | output directory; must not exist or be empty |
| `-i, --input FILE` | stdin | read FILE instead of stdin |
| `--route-key FIELD` | (none) | route by this top-level field's value, not by the set of keys |
| `--learn-rows N` | 100 | rows a schema is learned from before it can lock |
| `--learn-max-rows N` | 10000 | most rows learned before a schema locks as it is |
| `--flush-rows N` | 32768 | locked tables: rows staged per flush |
| `--flush-mb N` | 8 | locked tables: MB of staged text per flush |
| `--table-mb N` | 32 | per table: the writer's `max_pending_bytes` |
| `--threads N` | 1 | threads for reading large flushes |
| `--max-line-mb N` | 16 | longest accepted line |
| `--strict` | off | reject a line with a key the locked schema does not have |
| `--one-version` | off | publish each table as one version at the end, not one per flush |
| `--compress` | off | zstd-compress string columns |

Exit status: 0 on success; 1 when a table, the rejects file or the catalog could not be written,
or the input stopped early (a line longer than `--max-line-mb`, a read error); 2 when the summary's
line accounting does not balance (a bug).

## Design notes

- Reading: blocks of whole lines (16 MB, or the longest line if larger); the tail after the last
  newline is carried into the next block. A UTF-8 BOM on line 1 is removed, CRLF is accepted, and
  blank lines are counted and skipped.
- Routing: each line is parsed alone with simdjson On-Demand. Non-objects, malformed lines,
  trailing content and repeated top-level keys go to `_rejects.lance` with no table.
- A table is named by its route value, or `t<NN>_<first three keys>` by key set; names are made
  safe for file names and kept unique (`_rejects` and `_catalog` are reserved).
- Learning: the first `--learn-rows` lines are staged and inferred with `simdjson_arrow`. While the
  schema still has null-typed leaves, inference runs again at each further `--learn-rows` step,
  up to `--learn-max-rows`, and then the schema locks as it is.
- A line that cannot be inferred with the rest (a type that changes within the learned rows) is
  found by bisection and rejected.
- Locked: the schema is exported, the table is created with the first dump (the inferred batches),
  and later lines are read through the same schema in flushes. A flush that fails to read is split
  in halves until each bad line stands alone, so good lines keep their order.
- A key the locked schema lacks is counted in `new_key_lines` (skipped on read) or rejected with
  `--strict`.
- Each flush is a visible version (`--one-version` defers all of them to one). A flush's rows are
  held in the writer until it publishes, so a flush that fails midway leaves nothing visible.
- Every non-blank line ends up in a table, in `_rejects.lance`, or counted as unwritable (its table
  failed); the summary asserts that.
- Memory: about tables x (`--table-mb` + `--flush-mb`), plus the learned rows of tables still
  learning.

## Known limits

- A top-level key with a dot is stored with `.` replaced by `_` (`user.id` becomes `user_id`),
  because pylance 12 cannot read a top-level column name with a dot. A collision gets `_2`, `_3`, ...
  The catalog's schema keeps the key as `source_name`. Nested dots are unchanged.
