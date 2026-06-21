# fuselance

Mount a Lance table that has a `lance.blob.v2` external-reference column as a **read-only FUSE
filesystem**. Each row becomes a file: the file name comes from a column you pick, and the file's
contents are the row's blob payload, fetched on demand from its external URI via
`nano_lance_fetch_external_blob`.

```
fuselance <lance_table_path> --filename-col <col> [--blob-col <col>] [--sort-col <col>]
```

- Mounts at `/tmp/fuse_<basename>` (e.g. `packets.lance` → `/tmp/fuse_packets`).
- `--filename-col` — column used for file names. String or integer columns are supported
  (integers are rendered as decimal text). Duplicate names get `_N` suffixes.
- `--blob-col` — the blob.v2 struct column. Auto-detected when omitted (first struct column with
  `uri`/`blob_uri` + `position` + `size` children).
- `--sort-col` — order files by this column (strings lexicographically, integers numerically,
  ascending). Defaults to the table's row order.
- The mount is read-only: browsing (`readdir`) and reading (`read`/range reads) only; writes return
  `EACCES`.

Unmount with `fusermount3 -u /tmp/fuse_<basename>` or Ctrl-C on the foreground process.

## Building

Requires `libfuse3-dev` (Debian/Ubuntu) or `fuse3-devel` (RPM). Built by default in a standalone
nanolance build; toggle with `-DNANOLANCE_BUILD_FUSE_EXAMPLE=ON/OFF`. If libfuse3 is not found the
example skips itself with a status message rather than failing the build.

```bash
cmake -B build -DNANOLANCE_BUILD_FUSE_EXAMPLE=ON
cmake --build build -t fuselance
```

## Notes

The table is read with `nano_lance::lance_table_read_dataset` (writer-parity), so the dataset must
be one written by nanolance (e.g. via `arrowipc2lance` or the `pcapng2lance` example). Only the
per-row `(name, uri, position, size)` metadata is held in memory while mounted; the payload bytes
are fetched lazily on each `read`.
