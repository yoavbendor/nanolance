# fuselance

Mount a Lance table that has a `lance.blob.v2` external-reference column as a **read-only FUSE
filesystem**. Each **distinct value** of a chosen column becomes one file; the file's contents are
the concatenation of all matching rows' blob payloads, fetched on demand.

```
fuselance <lance_table_path> --filename-col <col>
          [--blob-col <col>] [--join-blob-from <blob_table>] [--sort-col <col>]
          [--frame-col <col>]
```

## Options

| Option | Description |
|---|---|
| `--filename-col <col>` | Column whose distinct values become file names (required). |
| `--blob-col <col>` | blob.v2 struct column. Auto-detected when omitted. |
| `--join-blob-from <path>` | Read blob refs from a companion table joined on `packet_id`. |
| `--sort-col <col>` | Order blobs within each file by this column (ascending). |
| `--frame-col <col>` | Numerical column; adds `frame_<N>/` subdirectories alongside the flat files. Each `frame_<N>/` contains the same file set filtered to rows where `<col> == N`. |

## Filesystem layout with `--frame-col`

```
/tmp/fuse_<table>/
  <file1>          ← all rows (same as without --frame-col)
  <file2>
  frame_0/
    <file1>        ← only rows where frame-col == 0
    <file2>
  frame_1/
    <file1>
    <file2>
  …
```

## Column type rendering

| Type | File name format |
|---|---|
| `string` / `large_string` | literal value |
| integer (`uint8` … `int64`) | decimal text |
| `fixed_size_binary:4` | dotted-quad IPv4 (`192.168.1.1`) |
| `fixed_size_binary:16` | colon-hex IPv6 (`fe80::1`) |
| other binary widths | lowercase hex dump |

## Omitting `--filename-col`

If you provide the table path but omit `--filename-col`, fuselance reads the manifest (schema only,
no data scan) and prints the available column names so you can pick the right one:

```
fuselance packets_ipv4.lance

fuselance: --filename-col is required

Columns in 'packets_ipv4.lance':
  packet_id                (uint64)
  src                      (fixed_size_binary:4)
  dst                      (fixed_size_binary:4)
  ...
```

## Example: pcapng2lance `--decode-l2l3` output

`pcapng2lance --decode-l2l3` writes separate tables: the L1 packet table (`packets.lance`) holds
the EPB blob references, and the decoded L3 tables (`_ipv4.lance`, `_ipv6.lance`) hold IP headers.
They share `packet_id` as a join key. Use `--join-blob-from` to combine them:

```bash
# Convert a pcap with L2/L3 decode
pcapng2lance --decode-l2l3 capture.pcap packets.lance

# Mount IPv4 sources — each src IP becomes one file containing all its EPBs
fuselance packets_ipv4.lance --filename-col src --join-blob-from packets.lance
```

```
ls /tmp/fuse_packets_ipv4/
  10.0.0.1    192.168.1.100   172.16.0.5

cat /tmp/fuse_packets_ipv4/10.0.0.1 | wc -c   # total bytes of all EPBs from 10.0.0.1
```

Opening a file with tshark will produce a format error (missing SHB/IDB preamble) because the file
contains raw EPB block bytes — exactly the expected behaviour for a blob-only stream.

## Quick demo (no pcap needed)

`demo/gen_demo.sh` builds a self-contained dataset: three text files — `letters.txt`
(`abc…xyz` repeated), `capital_letters.txt` (`ABC…XYZ`), and `numerals.txt`
(`0123456789`) — referenced by an **interleaved** blob.v2 column (rows cycle
letters → capital → numerals, and each file is split into several scattered
segments). Grouping by the `name` column reassembles each file in order:

```bash
# 1. generate source files + a native .lance (needs pyarrow + pylance for step 1)
examples/fuselance/demo/gen_demo.sh ./build/arrowipc2lance

# 2. mount and browse
fuselance /tmp/fuselance_demo/demo.lance --filename-col name
ls  /tmp/fuse_demo            # capital_letters  letters  numerals
cat /tmp/fuse_demo/letters    # abcdefghijklmnopqrstuvwxyzabc…  (1040 bytes)
```

The demo goes through Arrow IPC + `arrowipc2lance` rather than writing a Lance
dataset with pylance directly: nanolance reads the Lance 2.2 manifest layout, but
current pylance emits a newer manifest variant nanolance does not decode. The
native `arrowipc2lance` tool produces a dataset nanolance (and fuselance) reads.

## Building

fuse3 is resolved in order:

1. **System package** (`libfuse3-dev` / `fuse3-devel`) — picked up automatically via pkg-config.
2. **Built from source** — if no system package is found, CMake fetches libfuse 3.16.2 from GitHub
   and builds it as a static library using meson + ninja. No `sudo` required.
   ```bash
   pip install meson ninja   # once, no sudo needed
   cmake -B build -DNANOLANCE_BUILD_FUSE_EXAMPLE=ON
   cmake --build build -t fuselance
   ```
3. **Neither** — cmake prints a status message and skips `fuselance` gracefully (unit tests still build).

```bash
cmake -B build -DNANOLANCE_BUILD_FUSE_EXAMPLE=ON
cmake --build build -t fuselance
```

## Unmounting

```bash
fusermount3 -u /tmp/fuse_<basename>
# or Ctrl-C on the foreground process
```
