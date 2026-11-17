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

The option is off by default. `NANOLANCE_BUILD_EXAMPLES` is not needed. Writing to `s3://` needs
`-DNANOLANCE_ENABLE_S3=ON` as well (it needs the libcurl development headers); a build without it
refuses an `s3://` output at start.

## Quick start

Run from the repository root; the paths are relative to it.

```
python3 examples/ndjson2lance/tests/gen_logs.py --seed 1 | ./build/examples/ndjson2lance/ndjson2lance out/
```

`out/` must not exist or must be empty. It then holds one `<table>.lance` per record shape,
`_rejects.lance` (lines that could not be stored, with the reason) and `_catalog.lance` (one row
per table). Read a table back with pylance or with nanolance's Python package (`pip install -e
bindings/python` first):

```python
import lance, nanolance, pyarrow as pa
print(lance.dataset("out/_catalog.lance").to_table().to_pylist())
rows = lance.dataset("out/t01_buckets_detail_event.lance").to_table()     # pylance
rows = pa.table(nanolance.read_table("out/t01_buckets_detail_event.lance"))  # nanolance
```

Route by one field instead of by the set of keys with `--route-key event`.

## Writing to S3

Give an `s3://` prefix instead of a directory. The tables, `_rejects.lance` and `_catalog.lance` are
written under it, as they are into a directory. The prefix must hold no objects yet (a run into a
prefix that has any is refused with exit 1, and nothing is written).

```
cat logs.ndjson | ndjson2lance s3://BUCKET/results/ -i - --route-key T
```

Credentials come from the AWS environment chain (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`,
`AWS_SESSION_TOKEN`, `AWS_REGION`, profiles). For an S3-compatible store such as MinIO, or for moto in
the tests, give its endpoint:

```
ndjson2lance s3://BUCKET/results/ -i logs.ndjson --s3-endpoint http://127.0.0.1:9000 --s3-allow-http
```

Each flush of a table is one commit, so a large input makes many S3 requests. `--one-version` publishes
each table once, at the end, which is far fewer requests. A run that fails part way leaves its objects
under the prefix; the next run needs a new prefix.

## Flags

| flag | default | meaning |
|---|---|---|
| `out_dir` | required | output directory, or an `s3://bucket/prefix`; must not exist or be empty (an `s3://` prefix must hold no objects) |
| `-i, --input FILE` | stdin | read FILE instead of stdin; `-i -` reads stdin explicitly |
| `--route-key FIELD` | (none) | route by this top-level field's value, not by the set of keys |
| `--learn-rows N` | 100 | rows a schema is learned from before it can lock |
| `--learn-max-rows N` | 10000 | most rows learned before a schema locks as it is |
| `--flush-rows N` | 32768 | locked tables: rows staged per flush |
| `--flush-mb N` | 8 | locked tables: MB of staged text per flush |
| `--table-mb N` | 32 | per table: the writer's `max_pending_bytes` |
| `--threads N` | 1 | threads for reading large flushes |
| `--max-line-mb N` | 16 | longest accepted line |
| `--strict` | off | reject a line with a key the locked schema does not have, at the top level or nested (a nested one gets the reason `new nested key after lock: a.b`) |
| `--one-version` | off | publish each table as one version at the end, not one per flush |
| `--no-compress` | off | keep string columns uncompressed; by default they are zstd-compressed |
| `--no-merge` | off | reject a line of several JSON objects instead of merging them into one record |
| `--s3-endpoint URL` | (AWS) | `s3://` output: endpoint of an S3-compatible store (MinIO, moto) |
| `--s3-region REGION` | (AWS env) | `s3://` output: region |
| `--s3-allow-http` | off | `s3://` output: allow an `http://` endpoint |

Exit status: 0 on success; 1 when a table, the rejects file or the catalog could not be written,
or the input stopped early (a line longer than `--max-line-mb`, a read error); 2 when the summary's
line accounting does not balance (a bug).

## Logs with a prefix object

Some loggers write a prefix object and then a payload object on one line, with no separator:

```
{"time":"2026-01-01T00:00:00.000001Z","host":"node-1","app":"svc 42","clock":{"name":"mono","t":12.5},"line":""}{"kind":"cpu","ver":1,"dur":120,"seq":1001,"items":[{"vals":[3,4],"name":"a"},{"vals":[0,0],"name":"b"}]}
{"time":"2026-01-01T00:00:00.000002Z","host":"node-1","app":"svc 42","clock":{"name":"mono","t":12.6},"line":""}{"kind":"disk","ver":1,"dur":80,"seq":1002,"items":[{"vals":[0,0,0,51200,40,900]}]}
```

Each such line is one record. Its keys are the prefix's and the payload's together. Two kinds of
payload can share top-level keys (here `kind`, `ver`, `dur`, `seq`, `items`), and then key routing
puts them in one table, where a nested key of one kind (`items.name`) is not in the schema of the
other. Route by the kind instead:

```
ndjson2lance out/ -i logs.ndjson --route-key kind
```

```
table                                rows    rejects   new_keys   new_nested   unwritable  status
cpu                                     1          0          0            0            0  ok
disk                                    1          0          0            0            0  ok
```

Each table has the prefix columns `time`, `host`, `app`, `clock`, `line` and the payload columns.
`--no-merge` restores the old behaviour, where such a line is rejected with `trailing content after
the record`.

## Performance

On the 1 GB mixed stream: 6.823 s and 147.9 MB peak RSS, against 38.388 s and 2,191.3 MB for
`pyarrow.json` with pylance. Output is 170.1 MB against 122.7 MB. Details and the other runs are in
[bench/ndjson/README.md](../../bench/ndjson/README.md).

## Design notes

- Reading: blocks of whole lines (16 MB, or the longest line if larger); the tail after the last
  newline is carried into the next block. A UTF-8 BOM on line 1 is removed, CRLF is accepted, and
  blank lines are counted and skipped.
- Routing: each line is parsed with simdjson On-Demand. A line of several JSON objects back to
  back (a prefix object, then the payload) is one record: the keys of every object are merged
  in input order, and a key in two of them is rejected. Non-objects, malformed lines, other
  trailing content and repeated top-level keys go to `_rejects.lance` with no table. The
  rejects keep the line as it came, and the table keeps the merged record.
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
- A top-level key the locked schema lacks is counted in `new_key_lines` (skipped on read) or rejected
  with `--strict`. A nested key the locked schema lacks (inside a struct, or a list of structs) is
  counted in `new_nested_key_lines` (skipped on read), and rejected with `--strict`. The flush is first
  read with simdjson_arrow's `error_on_unknown_fields`; only a flush that fails that read is checked
  line by line, so ordinary flushes cost one read.
- Each flush is a visible version (`--one-version` defers all of them to one). A flush's rows are
  held in the writer until it publishes, so a flush that fails midway leaves nothing visible.
- Every non-blank line ends up in a table, in `_rejects.lance`, or counted as unwritable (its table
  failed); the summary asserts that.
- Memory: about tables x (`--table-mb` + `--flush-mb`), plus the learned rows of tables still
  learning.

## Known limits

- A line with other trailing content after an object (`{"a":1}[1]`) is rejected with simdjson's
  `INCOMPLETE_ARRAY_OR_OBJECT` reason: simdjson reports it before the object is read, so the
  `trailing content after the record` reason is given only for a following `{` (with
  `--no-merge`) or other bytes simdjson reads past the object (such as `}`).
- A nested key that appears after the schema locked is skipped on read (a `R` under `v`, say): the
  record keeps its other values, and `new_nested_key_lines` counts the record.
- A top-level key with a dot is stored with `.` replaced by `_` (`user.id` becomes `user_id`),
  because pylance 12 cannot read a top-level column name with a dot. A collision gets `_2`, `_3`, ...
  The catalog's schema keeps the key as `source_name`. Nested dots are unchanged.
