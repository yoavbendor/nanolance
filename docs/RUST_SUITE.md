# Lance's Rust encoding tests, read back by nanolance

Lance's Rust crates test their own encoder against their own decoder. `lance-encoding`'s tests
encode data and decode it again: every type, null pattern, page size, compression setting and
structural layout the Lance authors thought worth covering. They decode it whole, by row ranges and
by row indices. `lance-file`'s tests write real files with Lance's `FileWriter`.

`tools/rust_suite.py` runs those tests unchanged, with one addition. Every round trip and every file
they write is also read by nanolance and compared with what the test expects. Every layout the Rust
writer can produce becomes a read test for nanolance, compared exactly as the Rust decoder is.

```sh
python tools/rust_suite.py                      # fetch (once), build, run; compare with the list
python tools/rust_suite.py --update             # ... and rewrite the list of tests that pass
python tools/rust_suite.py --filter bitpack     # only tests whose name contains "bitpack"
python tools/rust_suite.py --crates lance-file  # one crate
NANOLANCE_RUST_KEEP=/tmp/keep python tools/rust_suite.py --filter ...   # keep each mismatching file
```

The work happens in these steps:

- The crates are the ones pylance 12.0.0 is built from: `lance-encoding` and `lance-file` 12.0.0.
  The script downloads them from crates.io into `.deps/rust-suite/`. They are not copied into this
  repository.
- It adds two modules to each crate's `testing` module (`tests/rust_suite/*.rs`) and one call where
  the tests check their result. It also links the test binary against `libnanolance_rust_shim`
  (`tests/rust_suite/nanolance_rust_shim.cpp`, built with CMake).
- In lance-encoding, each round trip's pages are the Rust encoder's own bytes. The hook wraps them
  into a Lance 2.2 file, adding the schema, the column metadata and the footer as Lance's writer
  lays them out. nanolance then reads that file the same three ways the test reads it: whole, by
  row ranges and by row indices.
- In lance-file, the hook reads every file the tests write with `write_lance_file` as it is.
- Results are compared the way the test compares the Rust decoder's: the Arrow type, then the values.

CI runs it nightly (`.github/workflows/rust-suite.yml`). The shim exports its two functions and
nothing else. The test binary links zstd and lz4 of its own, and when the shim also exported
nanolance's copies, calls could bind across the two versions. One of Lance's compression tests
crashed on a CI runner that way, though it never calls nanolance. The runner refuses a shim that
exports those symbols. When the test binary does crash, the runner runs the tests again one at a
time and names the test that died. It also runs when the suite itself changes.

## Outcomes

Per round trip, and per test as the worst of its round trips:

| outcome | meaning |
|---|---|
| pass | read back equal, as the Rust decoder must |
| type | equal values, a different Arrow type (for example, another name for a list's item) |
| refused | nanolance declined with an error naming what it does not read |
| mismatch | nanolance returned different rows. This is a wrong result, and it is never acceptable |

A run fails when a test listed in `tests/rust_suite/expected_pass.txt` stops passing. It also fails
on any mismatch that `tests/rust_suite/known_mismatch.txt` does not name. That file is empty.

## Results

`bench/results/rust_suite.json` holds the latest summary. From the run on this machine, against the
Lance 12.0.0 crates:

| | |
|---|---|
| tests run (lance-encoding and lance-file 12.0.0) | 1,986; each also passes in Rust |
| tests with round trips that nanolance reads back, all equal | **1,254** (`expected_pass.txt`) |
| tests where nanolance refuses some round trip | 209 |
| tests with no round trip (unit tests of one component) | 523 |
| round trips read back equal | **10,337** |
| round trips refused | 650 (each under its first reason, below) |
| round trips with different rows | **0** |

The runs so far found three bugs in nanolance, all fixed and pinned by tests that do not need Rust:

- **A wrong result.** `test_miniblock_bitpack` found it. Rust Lance picks each page's layout on its
  own, so a column of 1024 threes followed by 1024 eights is two constant pages with different
  values. nanolance planned a column from its first page and read every row as 3. pylance's own
  writer produces the same shape. The column is now decoded page by page when its pages need plans
  of their own (`test_pylance_compat.py::test_constant_pages_with_different_values`).
- **Format 2.1 was refused.** The file footer holds the major version and then the minor, and
  nanolance read them swapped. That was invisible while only 2.2 was accepted. Formats 2.1 and 2.2
  now read (`test_pylance_written_shapes_read_back` covers both).
- **A fixed-width constant too wide to inline was refused.** Its value is a one-buffer scalar.

Bare file names now also work: `LanceFileReader("x.lance")` used to look for `data/x.lance`.

The later runs found and fixed these, each pinned by a test that does not need Rust:

- **The sparse page layout** (Lance 12, format 2.3). Every such page was refused; they are read now.
- **RLE chunks** with a run length of any width (u8, u16 or u32) and with framed values (zstd or lz4).
- **Level counts** far larger than their compressed bytes. RLE and bitpacked levels compress well, and a
  zero-width level encoding stores no bytes at all.
- **Same-named struct children** (Arrow allows the empty name). Each takes the field of its occurrence.
- **Format 2.3** files are read.
- **Packed structs** (`lance-encoding:packed`) in formats 2.1 and 2.2, when their children are fixed-width.
  A packed struct inside another struct is refused by name.
- **Fixed-size lists of fixed-size lists** are read. Writing them is refused by name.
- **Format 2.0 fixed-size lists of booleans** are read.
- **Dictionaries inside lists and structs** are read with any integer index.

What nanolance refuses, by round trips (each refused round trip is counted under its first reason):

- **Lists or maps under a fixed-size list** (576: 432 under `values`, 144 under `props`). nanolance's
  schema mapping refuses them by name and does not decode them yet. The suite's own tests of these
  shapes pass in Rust, so this is nanolance's nested decoder that is missing, not a case the suite
  shows Lance failing.
- **Legacy blob columns** (`lance-encoding:blob`) outside format 2.0 (48). The harness refuses them
  there; nanolance reads its own blob layout, not the one Lance writes.
- **Several fields in one column** (20): files with more columns than leaf fields. This is the
  layout of blob v2 structs.
- **Variable-width packed structs** (`VariablePackedStruct`, 2 round trips). Their utf8 and
  blob v2 descriptor children are not read yet.
- **Sparse pages with a fixed-size-list layer** (1). The sparse layout is read, but this layer is not.
- A few single cases: a fixed-size list of structs inside a struct or list (write refusal, 1), a nested
  column decoded to a different shape than its schema (1), and a dict-RLE run count mismatch (1).
