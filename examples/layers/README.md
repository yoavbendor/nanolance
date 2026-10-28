# Layer examples

One small program per layer. Each uses the layer's public entry points (named in `src/<layer>/README.md`), checks its
own results and prints a few lines. ctest builds and runs them, so they cannot go stale.

| Number | Layer | File | What it shows |
|---|---|---|---|
| 1 | base | [base.cpp](base.cpp) | bool packing, least significant bit first, and the thread pool (`parallel::for_each`) |
| 2 | io | [io.cpp](io.cpp) | write bytes through the local file backend, read a range back, list and remove the file |
| 3 | arrow | [arrow.cpp](arrow.cpp) | build a two-column Arrow batch with nanoarrow, map its schema to Lance's types, fill column values |
| 4 | encoding | [encoding.cpp](encoding.cpp) | bit-pack a page of int32 values in blocks and decode it back |
| 5 | file | [file.cpp](file.cpp) | write a one-column `.lance` file, read its footer and schema, decode the column |
| 6 | table | [table.cpp](table.cpp) | commit two versions (CREATE, then APPEND) and read the manifests back |
| 7 | expr | [expr.cpp](expr.cpp) | parse, bind and filter with a predicate; evaluate a value expression |
| 8 | scan | [scan.cpp](scan.cpp) | read a row range and a list of rows from a standalone `.lance` file |
| 9 | index | [index.cpp](index.cpp) | build a BTree scalar index, list it, count a filter from the index alone |
| 10 | dataset | [dataset.cpp](dataset.cpp) | delete a row as a new version, then read the latest version back |
| 11 | api | [api.cpp](api.cpp) | write and read a dataset through the public C API (`nano_lance_writer_*`, `nano_lance_table_*`) |

## Build and run

    cmake --build build
    ctest --test-dir build -L example --output-on-failure

One example: `ctest --test-dir build -R nano_lance_example_<layer> --output-on-failure`.

The examples are built with the tests (`NANOLANCE_BUILD_TESTS`), are labelled `example` and `layer:<layer>`, and do not
depend on `NANOLANCE_BUILD_EXAMPLES`. Each writes under a fresh directory in the temp directory and removes it at the
end. An example includes only its own layer's headers, the layers below it and the standard library
(`tools/check_layers.py` checks this). The encoding example also reads one module from `src/`: the bit-packing module
has no public header yet.
