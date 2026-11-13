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
NANOLANCE_RUST_KEEP=/tmp/keep NANOLANCE_RUST_KEEP_PASS=1 python tools/rust_suite.py --filter ...  # keep every round trip
                                                                        # (the small files tests/golden are written from)
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
| tests with round trips that nanolance reads back, all equal | **1,463** (`expected_pass.txt`) |
| tests where nanolance refuses some round trip | 0 |
| tests with no round trip (unit tests of one component) | 523 |
| round trips read back equal | **10,987** |
| round trips refused | 0 |
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

- **Variable-width packed structs** (`lance-encoding:packed` with utf8 or binary children, a FullZip
  `VariablePackedStruct` page), also inside a sparse list of them (6 round trips). Each row holds the children
  back to back, a variable child as a length prefix. `tests/file/test_variable_packed_struct_read.cpp`.
- **Dictionary run-length indices** with u8, u16 or u32 run lengths (`test_miniblock_bitpack`, 1 round trip).
  The lengths' width comes from the page, as for a plain RLE chunk. `tests/file/test_dict_rle_read.cpp`.
- **Sparse map keys and sparse fixed-size-list layers** (the map's key layer is all-valid and is not counted;
  a fixed-size list's layer counts its elements). `tests/file/test_sparse_page_read.cpp`.
- **Lists of fixed-size lists of structs** (`list<fixed_size_list<struct>>`) are read, whole, by range and by
  take. Writing them is refused by name.
- **The sparse page layout** (Lance 12, format 2.3). Every such page was refused; they are read now.
- **RLE chunks** with a run length of any width (u8, u16 or u32) and with framed values (zstd or lz4).
- **Level counts** far larger than their compressed bytes. RLE and bitpacked levels compress well, and a
  zero-width level encoding stores no bytes at all.
- **Same-named struct children** (Arrow allows the empty name). Each takes the field of its occurrence.
- **Format 2.3** files are read.
- **Packed structs** (`lance-encoding:packed`) in formats 2.1 and 2.2, with fixed-width children and, since
  the variable-width reads above, with `utf8` and `binary` children too. A packed struct inside another struct is
  refused by name.
- **Fixed-size lists of fixed-size lists** are read, and written at the top level (mini-block under 256 bytes of
  leaves, FullZip from 256 bytes). Under a struct or list, writing them is refused by name.
- **Lists, large lists and maps under a fixed-size list of structs** are read, whole, by row range and by take.
  They are written too (format 2.2).
- **Format 2.0 fixed-size lists of booleans** are read.
- **Dictionaries inside lists and structs** are read with any integer index over string or binary values. Writing
  them is refused unless the indices are int32.
- **Legacy blob columns** (`lance-encoding:blob`, a LargeBinary) in format 2.1 (48 round trips). Lance 2.1
  stores them as a BlobLayout over a packed (position, size) descriptor, which nanolance already decoded. The
  refusal was in the harness's shim, which refused any such column before reading and stamped 2.2 on it; Lance
  refuses the marker from 2.2 on. The shim writes 2.1 for these files now.
- **Blob v2 structs in standalone data files** (16 round trips, `tests/file/test_blob_v2_file_read.cpp`). A file's
  blob v2 column is one column: a struct marked `lance-encoding:blob` whose five descriptor children have no ids. The
  file reader counted each child as a column. It now links the children to their parent and counts the struct as one
  column.

What nanolance refuses: none of the round trips. Writing is still refused by name for the shapes that
were read but not written: a packed struct (`a packed struct is read but not written yet`), a fixed-size list of
fixed-size lists under a struct or list (`... is read but not written yet`), a fixed-size list of structs under a
struct or list (`a fixed_size_list of structs inside a struct or list is not written yet`), and a dictionary with a
non-int32 index inside a struct or list.
