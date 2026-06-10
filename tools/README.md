# nanolance tools

## `nlance2table` — dump a Lance dataset to CSV / NDJSON

A small reader-side companion that prints the rows of a **nanolance-written** Lance dataset as text. It is
deliberately scoped to what nanolance itself writes (not arbitrary Lance encodings/compressions), reusing
the same reader the library's parity test uses (`nano_lance::lance_table_read_dataset`). It doubles as a
showcase of how little code it takes to consume the reader: read the dataset into Arrow batches, walk an
`ArrowArrayView`, emit text.

```
nlance2table [-f csv|ndjson] [-n N] [-o FILE] <dataset.lance>
```

- `-f, --format` — `csv` (default) or `ndjson` (one JSON object per line; `jsonl` accepted as an alias).
- `-n, --limit N` — print only the first **N** rows. This is a *print cap*, not a read optimization: the
  whole dataset is still read (the reader loads all committed batches up front).
- `-o, --output FILE` — write to a file instead of stdout.

Rendering rules (format-agnostic):

- Integers/floats print as their value; `bool` as `true`/`false`.
- `string` prints as text (quoted in NDJSON).
- `fixed_size_binary` / `binary` print as **lowercase hex** with no separators — the tool is semantic-free
  (it does not know a column is a MAC or an IP; e.g. a MAC is `01005e060601`, an IPv4 `c0a80102`). Pretty
  printing (dotted/colon) belongs in whatever consumes the text.
- Nested `struct` columns (e.g. the `lance.blob.v2` `payload_ref`) flatten to **dotted** column names in
  CSV (`payload_ref.uri`) and **nested objects** in NDJSON.
- Nulls are an empty CSV field / JSON `null`.

### Example: dump the pcapng2lance L1 table

```
pcapng2lance capture.pcapng out.lance
nlance2table -f csv -n 5 out.lance
# packet_id,interface_id,ts_raw,...,payload_ref.uri,payload_ref.position,payload_ref.size
# 0,0,828268046938,...,file:///.../capture.pcapng,148,2066
```

### Validating L2/L3/L4 decode against Wireshark

Because nlance2table emits plain text keyed by `packet_id`, the per-PDU tables from `--decode-l2l3` can be
diffed field-for-field against `tshark` on the **same** capture — an independent check that the nanotins
decode matches an authoritative dissector:

```
pcapng2lance --decode-l2l3 capture.pcapng out.lance
nlance2table -f ndjson out_ipv4.lance     # our IPv4 rows, hex src/dst
tshark -r capture.pcapng -T fields -e frame.number -e ip.src -e ip.dst -e ip.proto
# normalize tshark's dotted IPs to hex, join on packet_id == frame.number-1, compare
```

This is exactly what `examples/pcapng2lance/tests/test_nlance2table_tshark.py` automates (it skips when
`tshark` is not installed). `test_nlance2table.py` is the dependency-free shape/format smoke.
