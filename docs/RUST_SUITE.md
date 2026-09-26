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

CI runs it nightly (`.github/workflows/rust-suite.yml`). It also runs when the suite itself changes.

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

`bench/results/rust_suite.json` holds the latest summary. From the run on this machine:

| | |
|---|---|
| tests run (lance-encoding 1,809, lance-file 177) | 1,986; each also passes in Rust |
| tests with round trips that nanolance reads back, all equal | **859** (`expected_pass.txt`) |
| tests where nanolance refuses some round trip | 604 |
| tests with no round trip (unit tests of one component) | 523 |
| round trips read back equal | **8,309** |
| round trips refused | 2,678, of which 1,128 are format 2.0 |
| round trips with different rows | **0** |

By layout, nanolance reads 2,739 of lance-encoding's 2,926 round trips in the u16 mini-block
layout, 2,789 of 3,456 in the u32 layout and 2,763 of 3,451 in the sparse layout. Of the files
lance-file writes, it reads 13 of 14 in format 2.1 and 5 of 7 in 2.2.

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

What nanolance refuses, by round trips (each refused round trip is counted under its first reason):

- **Fixed-size lists of structs or of booleans** (1,160). nanolance's schema mapping takes
  fixed-size lists of numbers, decimals and temporal types.
- **Format 2.0** (853): lance-encoding's `Array` test encoding, and 10 lance-file files. Format
  2.3 is also refused (5 files).
- **Several fields in one column** (138 in the structural formats): packed structs
  (`lance-encoding:packed`) and Lance's blob struct.
- **Arrow dictionary types** (178). nanolance's schema mapping refuses the type rather than return
  the values as another type.
- **Lance's blob layout** for `lance-encoding:blob` binary columns (48 in the structural formats).
  nanolance reads its own blob layout, not the one Lance writes.
- **Empty structs** (48).
- A tail of layouts nanolance does not decode yet. Each is named in the report: some FullZip
  fixed-size lists, a few RLE and dictionary-RLE mini-blocks whose runs span chunks, and page
  layouts it does not recognize.
