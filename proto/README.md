# Lance Protobuf Inputs

This directory documents the protobuf source set used by the direct C++ writer.

The current phase checks in a minimal `.pb` scaffold for the specific manifest,
data-file, metadata, and column-metadata round-trip tests needed by steps 1-4.
The full writer phase should replace or extend that scaffold with nanopb output
generated from the pinned Lance `v7.0.0-rc.1` files:

- `table.proto`
- `file.proto`
- `file2.proto`
- `encodings_v2_1.proto`

Generated code must remain static and must not link against the standard
protobuf runtime.
