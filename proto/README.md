# Lance protobuf inputs

nanolance reads and writes Lance's protobuf metadata without the protobuf runtime or generated
code:

- **Manifests, schemas, data files, deletion files and column metadata.** These messages, from
  Lance's `table.proto`, `file.proto` and `file2.proto`, are declared once in nanom's
  `nanom/formats/lance_protobuf.hpp`. nanom's proto3 codec (`nanom/protobuf.hpp`,
  `nanom/protobuf_encode.hpp`) reads and writes them. `generated/lance_minimal.pb.cpp` only
  converts between that model and nanolance's structs.
- **Page layouts** (`encodings_v2_1.proto`): parsed by `src/page_layout.cpp` and built by
  `src/data_file_writer.cpp`.

To read a field Lance adds, declare it in the nanom model with its field number and type. Fields
that are not declared are skipped on read.
