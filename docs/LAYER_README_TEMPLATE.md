# How a layer's README is written

Every layer in `tools/layout.py` has `src/<layer>/README.md`. It says what the layer is for, how to use it
correctly, and which parts of the Lance specification it implements, each with where it is implemented and
the test that proves it. `python3 tools/check_spec.py` checks the last part against the code and the tests (it
runs in CI); the rest is for people.

The sections, in this order:

```markdown
# <layer>: <one line: what this layer is>

<Two to five sentences: what it does, what it does not do (which layer does), and which layers it uses.>

## Using it

- The entry points a caller uses, with the header each is declared in.
- What a caller must do, and must not do (for example: "call lance_dataset_scan; the _core functions are for
  builds without the index layer").
- Thread safety, ownership, and error reporting, where they matter.
- A short example when one helps.

## Modules

| module | what it holds | tests |
|---|---|---|
| `values/rle` | run-length values and levels, both directions | `nano_lance_encoding_rle` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0).

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `encoding.rle` | [Encoding Strategy](https://lance.org/format/file/encoding/) | done | `src/encoding/values/rle.cpp::decode` | `nano_lance_encoding_rle`, `bindings/python/tests/test_write_encoding_matrix.py` | unit+interop | |

## Known gaps

What the table marks missing or partial, in prose: what it would take, and whether it matters.
```

## The table

- **ID**: `<layer>.<area>.<item>`, unique in the repository. Keep IDs stable: other documents may cite them.
- **Spec**: a link to the section of https://lance.org/format/ the row is about. A layer the specification does
  not cover names the reference it follows instead, as `ref: <what>` (`ref: pylance 12.0.0 LanceDataset.scanner`,
  `ref: DataFusion SQL LIKE`).
- **Status**: `done` (all of the item), `partial` (say what is missing in Notes), `missing`, or `n/a` (does
  not apply to nanolance; say why).
- **Implementation**: `path::symbol` from the repository root, one or more, backticked. The checker requires
  the file to exist and the symbol to appear in it.
- **Tests**: ctest names (`nano_lance_rle`) or pytest ids (`bindings/python/tests/test_x.py::test_y`, or the
  file alone), backticked. Every `done` and `partial` row names at least one.
- **Verified by**: `unit` (nanolance's own tests), `interop` (files pylance writes are read, or files nanolance
  writes are read by pylance, and the results compared), `unit+interop`, or `-` for missing / n/a rows. Only
  interop shows conformance to the specification as Lance implements it: say `unit` when that is all there is.

One row per item a reader of the specification would check off: an encoding, a page layout, a file
structure, a manifest field group, a transaction type and its conflict rules, an index type's files, a
version feature. Not one row per field; not one row per page.

## Tests

The tests of a layer's modules live in `tests/<layer>/` and carry the ctest label `layer:<layer>`, so
`ctest -L layer:table` runs one layer. A test that crosses layers through the public API (write with the C
writer, read with the reader) lives in `tests/integration/`. The checker requires every module of a layer to
have a test in its folder that includes it, or a line in `tools/tests_allow.txt` that says why not; that file
only shrinks.
