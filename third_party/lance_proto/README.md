# Lance v2.2 Proto Pin

`nano_lance_writer` intentionally implements a narrow Lance writer directly in
C++ instead of linking `lance-c` or the Rust Lance core.

The phase-1 reference point is:

- Repository: `https://github.com/lance-format/lance`
- Tag: `v7.0.0-rc.1`
- Required data format: `data_storage_format.file_format = "lance"`
- Required data format version: `data_storage_format.version = "2.2"`

The relevant protobuf definitions are copied from that tag when the full writer
is expanded:

- `protos/table.proto` for dataset manifests and fragments.
- `protos/file.proto` for schema fields shared by manifests.
- `protos/file2.proto` for Lance v2 data-file `ColumnMetadata`, page metadata,
  encoding locations, and the fixed `LANC` footer.
- `protos/encodings_v2_1.proto` for Lance 2.1/2.2 structural and compressive
  encoding messages, including the `has_large_chunk` field introduced for 2.2.

The first implemented subset is deliberately conservative:

- Immutable-schema create/append only.
- Lance v2.2 data files only.
- Zstd as the only general compression target.
- Flat scalar Arrow arrays first; string/binary and nested structures require
  golden validation before being enabled.
- Blob v2 metadata preservation only. Advanced external blob references remain
  a later phase.

## Golden Fixture Workflow

Golden datasets should be generated with `tests/generate_golden_lance_v22.py`.
The script writes small Arrow IPC inputs and matching Lance v2.2 directories,
then validates them through Lance and Polars when those Python packages are
available.

The writer implementation must match the generated fixtures semantically before
adding additional Arrow types.
