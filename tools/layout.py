# SPDX-License-Identifier: Apache-2.0
"""The layers of nanolance's sources, lowest first: where each file of src/ lives, and which layers each
layer may use. One table for the move into src/<layer>/ (tools/move_to_layers.py) and for the layering
check (tools/check_layers.py). A layer may use the layers below it, not those above. The layers follow
the Lance format and Lance's own crates: io, encoding, file, table, index, dataset."""

# (layer, directory under src/, what it answers)
LAYERS = [
    ("base", "base", "threads, statistics, safety limits, small numerics and helpers"),
    ("io", "io", "where bytes live: local files and object stores (S3 is a registered backend)"),
    ("arrow", "arrow", "Arrow arrays and schemas, and how they map to Lance's types"),
    ("encoding", "encoding", "how one page of one column is laid out (both directions)"),
    ("file", "file", "how a .lance file is framed: footer, column metadata, global buffers, blobs"),
    ("table", "table", "what a version is: manifest, fragments, deletions, row ids, transactions, refs, "
                       "index metadata"),
    ("expr", "expr", "what a filter means (no storage)"),
    ("scan", "scan", "reading rows of fragments, given a row selection"),
    ("index", "index", "each index kind: build, look up, describe"),
    ("dataset", "dataset", "user operations: write sessions, updates, merge_insert, compaction, index "
                           "lifecycle, query planning"),
    ("api", "api", "the C APIs and lance-c"),
]

# Every file of src/, by its layer (a path below the layer directory where it has a sub-directory).
FILES = {
    "base": ["buffer_pool.cpp", "parallel.cpp", "version.cpp", "roaring_bitmap.cpp", "float16.hpp",
             "float_text.hpp", "int128.hpp", "proto_wire.hpp", "fts_json.hpp", "shared_cache.hpp", "uuid.hpp",
             "uuid.cpp", "bytes.hpp"],
    "io": ["byte_source.cpp", "byte_sink.cpp", "external_blob.cpp", "external_blob.hpp", "s3_byte_source.cpp", "s3_byte_source.hpp", "s3_byte_sink.cpp",
           "s3_byte_sink.hpp"],
    "arrow": ["schema_mapper.cpp", "array_accessor.cpp", "arrow_slice.cpp", "blob_builder.cpp", "jsonb.cpp",
              "arrow_owned.hpp", "arrow_owned.cpp"],
    "encoding": ["fsst.cpp", "lz4_block.cpp", "repdef.cpp", "page_layout.cpp", "column_slice.cpp",
                 "lance_descriptors.hpp", "lance_column_decoder.cpp", "lance_v20_decoder.cpp",
                 "encoding_choice.cpp", "encoding_choice.hpp", "blob_v2_layout.cpp",
                 "values/bitpack.cpp", "values/bitpack.hpp", "values/general.cpp", "values/general.hpp"],
    "file": ["data_file_reader.cpp", "data_file_writer.cpp", "lance_file_writer.cpp", "blob_v2_external.cpp"],
    "table": ["manifest_reader.cpp", "manifest_writer.cpp", "deletion_vector.cpp", "row_ids.cpp",
              "base_paths.cpp", "overlays.cpp", "transaction_file.cpp", "transaction_file.hpp",
              "dataset_operation.cpp", "dataset_operation.hpp", "dataset_transaction.cpp",
              "dataset_transaction_ops.hpp", "commit_conflicts.cpp", "commit_conflicts.hpp", "mem_wal.cpp",
              "dataset_refs.cpp", "frag_reuse.cpp", "index_maintenance.cpp", "index_location.hpp",
              "index_location.cpp"],
    "expr": ["expr.cpp"],
    "scan": ["lance_table_reader.cpp"],
    "index": ["segment.hpp", "index_files.cpp", "index_files.hpp", "index_search.cpp",
              "scalar/scalar_index.cpp", "scalar/bloom_filter.cpp", "scalar/scalar_segment.hpp",
              "fts/fts_tokenizer.cpp", "fts/fts_jieba.cpp", "fts/fts_jieba_hmm_data.inc", "fts/fts_lindera.cpp",
              "fts/fts_fst.cpp", "fts/fts_fst.hpp", "fts/fts_posting.cpp", "fts/fts_posting.hpp",
              "fts/fts_search.cpp", "fts/fts_index_build.cpp", "fts/fts_document_text.hpp",
              "fts/fts_english_stem.inc", "fts/fts_snowball_env.inc", "fts/fts_tables.inc",
              "fts/inverted_segment.hpp",
              "vector/vector_search.cpp", "vector/vector_index_build.cpp", "vector/hnsw_build.cpp",
              "vector/hnsw_build.hpp", "vector/rabitq_build.hpp", "vector/vector_segment.hpp"],
    "dataset": ["dataset.cpp", "dataset_ops.cpp", "dataset_stitcher.cpp", "index_optimize.cpp",
                "index_segments.cpp", "write_session.cpp", "write_session.hpp"],
    "api": ["nano_lance_writer.cpp", "writer_cxx.cpp", "nano_lance_reader.cpp", "nano_lance_table_reader_c.cpp", "nano_lance_external_blob.cpp",
            "lance_c.cpp"],
}

# The public headers in include/nanolance/, by layer (they stay where they are; the check reads this).
HEADERS = {
    "base": ["work_stats", "read_safety", "bool_bitpack", "parallel", "buffer_pool", "roaring_bitmap", "version"],
    "io": ["byte_source", "byte_sink", "object_store", "path_safety"],
    "arrow": ["schema_mapper", "array_accessor", "column_values", "arrow_slice", "blob_builder", "jsonb"],
    "encoding": ["fsst", "lz4_block", "repdef", "page_layout", "column_slice",
                 "lance_column_decoder", "lance_v20_decoder", "encodings_io", "blob_v2_layout"],
    "file": ["data_file_reader", "data_file_writer", "lance_file_writer", "blob_v2_external"],
    "table": ["manifest_reader", "manifest_writer", "deletion_vector", "row_ids", "base_paths", "overlays",
              "dataset_commit", "dataset_transaction", "mem_wal", "dataset_refs", "frag_reuse",
              "index_maintenance"],
    "expr": ["expr"],
    "scan": ["lance_table_reader"],
    "index": ["index_search", "scalar_index", "bloom_filter", "fts_tokenizer", "fts_lindera", "fts_search", "fts_index_build",
              "vector_search", "vector_index_build"],
    "dataset": ["dataset", "dataset_ops", "dataset_stitcher", "index_optimize", "index_segments"],
    "api": ["writer", "writer_internal", "typed_writer", "nano_lance_reader", "nano_lance_writer", "nano_lance_external_blob",
            "lance_c"],
}


def module_layer():
    """Module name (a file's name without its extension) -> layer."""
    out = {}
    for layer, files in FILES.items():
        for f in files:
            out[f.rsplit("/", 1)[-1].rsplit(".", 1)[0]] = layer
    for layer, names in HEADERS.items():
        for n in names:
            out.setdefault(n, layer)
    return out


def rank():
    return {name: i for i, (name, _, _) in enumerate(LAYERS)}
