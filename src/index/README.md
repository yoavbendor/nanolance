# index: each index kind, built, looked up and described in Lance's format

<!-- layer-deps:start -->
Layer 9 of 11 (1 is the bottom, 11 the top). Uses: [base](../base/README.md), [arrow](../arrow/README.md), [file](../file/README.md), [table](../table/README.md), [expr](../expr/README.md), [scan](../scan/README.md). Used by: [dataset](../dataset/README.md), [api](../api/README.md). See [docs/LAYERS.md](../../docs/LAYERS.md).
<!-- layer-deps:end -->
Example: [examples/layers/index.cpp](../../examples/layers/index.cpp) (run with ctest -R nano_lance_example_index).

This layer builds Lance's secondary indices -- the scalar ones (BTREE, BITMAP, LABEL_LIST, ZONEMAP,
BLOOMFILTER, NGRAM, and the JSON index over a BTREE), the full-text INVERTED index, and the IVF vector
indices (IVF_FLAT, IVF_SQ, IVF_PQ, IVF_RQ, IVF_HNSW_FLAT / _SQ / _PQ) -- in the files and manifest entries
Lance writes, and answers queries from them, whoever built them. It does not decide when an index is
rebuilt or how segments are merged and committed after appends and compactions: that is the dataset layer
(`dataset/index_optimize.cpp`, `dataset/index_segments.cpp`, `dataset/dataset_ops.cpp`); the index section
of the manifest, the fragment-reuse index and MemWAL are the table layer's. It uses the file layer to read
and write the index files (they are Lance files), the scan layer to read the indexed column, the expr layer
for filters and the table layer to commit.

## Using it

- Scalar indices, `nanolance/scalar_index.hpp`: `dataset_create_scalar_index` (one of `ScalarIndexType`,
  committed as the next version), `dataset_drop_index`, `dataset_list_indices`, `dataset_count_from_index`
  and `dataset_explain_filter`. `nanolance/index_search.hpp`'s `index_candidates` answers a bound filter
  with the rows that may pass, per covered fragment; a scan must still apply the whole filter to them
  (`IndexCandidates::exact` says when it need not). Filtered scans call it through
  `lance_dataset_scan` (dataset layer); the scan layer's `_core` entry points never do.
- Full-text search, `nanolance/fts_search.hpp`: `dataset_create_inverted_index`, `dataset_full_text_search`
  with an `FtsQuery` (built, or parsed from pylance's JSON by `parse_fts_query`). The analyzer is
  `nanolance/fts_tokenizer.hpp`'s `fts::Analyzer`; jieba and lindera models are directories under
  `fts::language_model_home()` (`LANCE_LANGUAGE_MODEL_HOME`), never downloaded by nanolance.
- Vector search, `nanolance/vector_search.hpp`: `dataset_create_vector_index`, `dataset_nearest` with a
  `NearestQuery`, and the readers `read_vector_index_model`, `read_vector_index_partition`,
  `vector_index_details`.
- Segments (internal, `index/segment.hpp` with `index/scalar/scalar_segment.hpp`,
  `index/fts/inverted_segment.hpp`, `index/vector/vector_segment.hpp`): a `SegmentTarget` with `out` set
  builds one segment over chosen fragments and hands its `IndexMetadata` back without committing; the
  dataset layer's distributed builds and `optimize_indices` are made of these. A vector build can be
  asked to train its model only (`VectorSegmentInputs::trained`) or to reuse one (`model`).
- Every function reports failure by returning false with a message in `error`; nothing throws, except a
  progress callback the caller supplies (an exception from it stops the build, nothing committed).
  Searches may run concurrently; loaded index files are cached process-wide (`index_cache`), keyed by
  path, size and modification time, so files rewritten in place are reloaded. A build must not run
  concurrently with another commit to the same dataset without the table layer's conflict handling.

```cpp
std::uint64_t v = 0;
std::string error;
nano_lance::dataset_create_scalar_index(path, "id", nano_lance::ScalarIndexType::BTree, {}, v, error);
nano_lance::NearestQuery q;
q.column = "vec";
q.key = query_vector;
q.k = 10;
nano_lance::NearestResult hits;
nano_lance::dataset_nearest(path, q, hits, error);  // hits.row_ids are row addresses, nearest first
```

## Modules

| module | what it holds | tests |
|---|---|---|
| `segment` | `SegmentTarget` and the build-progress `stage` helper shared by every builder | `nano_lance_index_segment` |
| `index_files` | reading and writing an index's Lance files; the bloom filter's value bytes and parameters | `nano_lance_index_files` |
| `index_search` | answering a filter from BTREE, BITMAP, LABEL_LIST, ZONEMAP, BLOOMFILTER, NGRAM and JSON segments | `nano_lance_index_candidates` |
| `scalar/scalar_index` | building the scalar indices; list, drop, count from index, explain | `nano_lance_index_candidates`, `nano_lance_scalar_segment` |
| `scalar/bloom_filter` | the split-block bloom filter (SBBF), XXH64, Lance's sizing | `nano_lance_bloom_filter` |
| `scalar/scalar_segment` | the scalar part of a segment build; NGRAM postings for merges | `nano_lance_scalar_segment` |
| `fts/fts_tokenizer` | Lance's analyzer: simple / whitespace / raw / code / ngram base tokenizers and the token filters | `nano_lance_fts_tokenizer`, `nano_lance_fts_multiscript` |
| `fts/fts_jieba` | jieba-rs search-mode segmentation with its HMM | `nano_lance_fts_jieba` |
| `fts/fts_lindera` | Lindera 3's config, dictionaries and segmenter | `nano_lance_fts_lindera` |
| `fts/fts_fst` | the token dictionary's FST | `nano_lance_fts_fst` |
| `fts/fts_posting` | posting-list blocks (bitpacked doc ids, frequencies, positions) | `nano_lance_fts_fst` |
| `fts/fts_document_text` | a row's document: a string, or a list's elements joined | `nano_lance_fts_document_text` |
| `fts/fts_index_build` | building an INVERTED index (one partition) | `nano_lance_fts_search`, `nano_lance_inverted_segment` (no header of its own) |
| `fts/fts_search` | BM25 search: match, phrase, boolean, boost, multi-match, fuzzy | `nano_lance_fts_search` |
| `fts/inverted_segment` | the INVERTED part of a segment build; params and rows of a segment | `nano_lance_inverted_segment` |
| `vector/vector_index_build` | IVF k-means, PQ / SQ / RQ quantizers, index.idx and auxiliary.idx | `nano_lance_vector_search`, `nano_lance_vector_segment` (no header of its own) |
| `vector/vector_search` | IVF probing, distance tables, HNSW search, RaBitQ estimator, exact search of the rest | `nano_lance_vector_search` |
| `vector/hnsw_build` | one partition's HNSW graph, as lance-index's HnswBuilder builds it | `nano_lance_hnsw_build` |
| `vector/rabitq_build` | RaBitQ's fast rotation, shared by build and search | `nano_lance_rabitq_build` |
| `vector/vector_segment` | the vector part of a segment build; a segment's trained model | `nano_lance_vector_segment` |

## Spec coverage

Against the Lance format specification at https://lance.org/format/ (as of Lance 12.0.0).

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `index.storage.directory` | [Index Storage](https://lance.org/format/index/) | done | `src/index/scalar/scalar_index.cpp::create_scalar_index`, `src/index/index_search.cpp::index_dir_of` | `nano_lance_scalar_segment`, `nano_lance_fts_search`, `bindings/python/tests/test_scalar_index.py::test_pylance_lists_and_validates_nanolance_indexes` | unit+interop | `_indices/<uuid>/` under the base the entry names |
| `index.segment.metadata` | [Creating and Updating Index Segments](https://lance.org/format/index/) | partial | `src/index/scalar/scalar_index.cpp::make_index_metadata`, `src/index/fts/fts_index_build.cpp::make_index_metadata`, `src/index/vector/vector_index_build.cpp::make_index_metadata` | `nano_lance_inverted_segment`, `nano_lance_scalar_segment`, `bindings/python/tests/test_scalar_index.py::test_describe_indices_and_explain_plan` | unit+interop | uuid, fields, name, dataset_version, fragment_bitmap, details, index_version, created_at, files; never covering_fields, clustering_id or metadata |
| `index.segment.coverage` | [Basic Concepts](https://lance.org/format/index/) | done | `src/index/index_search.cpp::index_candidates`, `src/index/fts/fts_search.cpp::dataset_full_text_search`, `src/index/vector/vector_search.cpp::dataset_nearest` | `nano_lance_fts_search`, `nano_lance_vector_search`, `bindings/python/tests/test_scalar_index.py::test_rows_appended_after_the_index_are_found`, `bindings/python/tests/test_vector_search.py::test_deleted_rows_and_unindexed_fragments` | unit+interop | every segment of an index searched, fragments no segment covers scanned |
| `index.segment.build` | [Creating and Updating Index Segments](https://lance.org/format/index/) | done | `src/index/scalar/scalar_index.cpp::build_scalar_segment`, `src/index/fts/fts_index_build.cpp::build_inverted_segment`, `src/index/vector/vector_index_build.cpp::build_vector_segment` | `nano_lance_scalar_segment`, `nano_lance_inverted_segment`, `nano_lance_vector_segment`, `bindings/python/tests/test_distributed_index.py::test_segments_cross_built` | unit+interop | a segment over chosen fragments, uncommitted; the dataset layer commits |
| `index.compat.type` | [Index Compatibility](https://lance.org/format/index/) | partial | `src/index/index_search.cpp::index_candidates`, `src/index/vector/vector_search.cpp::is_vector_index_url` | `nano_lance_index_candidates` | unit | a segment whose details type is unknown is not used and its fragments are scanned; no test commits an unknown type |
| `index.compat.version` | [Index Compatibility](https://lance.org/format/index/) | missing | | | - | IndexMetadata.index_version is never checked before a segment is read |
| `index.covering_fields` | [Serving carried columns](https://lance.org/format/index/) | missing | | | - | no carried columns are written, read or served; covering_fields are ignored |
| `index.deleted_rows` | [Handling deleted and invalidated rows](https://lance.org/format/index/) | done | `src/index/index_search.cpp::index_candidates`, `src/index/vector/vector_search.cpp::dataset_nearest` | `bindings/python/tests/test_vector_search.py::test_deleted_rows_and_unindexed_fragments`, `bindings/python/tests/test_count_from_index.py::test_counts_after_deletes_updates_and_whole_fragment_delete`, `bindings/python/tests/test_fts.py::test_unindexed_fragments_and_deletions` | unit+interop | deleted rows and fragments no longer in the version are filtered out |
| `index.updated_columns` | [Handling deleted and invalidated rows](https://lance.org/format/index/) | done | `src/index/index_search.cpp::index_candidates`, `src/table/index_maintenance.cpp::drop_fragments_from_indices` | `bindings/python/tests/test_index_preservation.py::test_update_and_merge_insert_keep_every_index`, `bindings/python/tests/test_index_preservation.py::test_column_changes` | unit+interop | rows outside the current fragment_bitmap are not answered from the index |
| `index.overlays` | [Handling deleted and invalidated rows](https://lance.org/format/index/) | done | `src/index/index_search.cpp::index_candidates`, `src/index/fts/fts_search.cpp::fragments_stale_for_index`, `src/index/vector/vector_search.cpp::fragments_stale_for_index` | `bindings/python/tests/test_data_overlay.py::test_stale_scalar_index_is_not_trusted`, `bindings/python/tests/test_data_overlay.py::test_stale_vector_index_is_not_trusted`, `bindings/python/tests/test_zoned_indexes.py::test_zonemap_under_a_data_overlay` | unit+interop | a fragment overlaid since the segment's dataset_version is scanned instead (whole fragment) |
| `index.remap.rewrite` | [Compaction and remapping](https://lance.org/format/index/) | partial | `src/dataset/dataset_ops.cpp::drop_fragments_from_indices`, `src/dataset/index_optimize.cpp::dataset_optimize_indices` | `bindings/python/tests/test_index_preservation.py::test_compaction_reindexes_the_rewritten_rows`, `bindings/python/tests/test_index_preservation.py::test_compaction_without_reindex_narrows_coverage` | unit+interop | compaction drops the rewritten fragments from coverage and rebuilds a segment over the new ones; row addresses inside existing segment files are never rewritten |
| `index.remap.frag_reuse` | [Compaction and remapping](https://lance.org/format/index/) | partial | `src/index/index_search.cpp::remap_rows` | `bindings/python/tests/test_frag_reuse.py::test_scalar_index_answers_through_frag_reuse`, `bindings/python/tests/test_compaction_index_coverage.py::test_remap_row_addrs` | unit+interop | scalar answers are remapped through the fragment reuse index in memory; FTS and vector searches do not consult it |
| `index.stable_row_ids` | [Stable Row ID for Index](https://lance.org/format/index/) | done | `src/index/scalar/scalar_index.cpp::create_scalar_index`, `src/index/index_search.cpp::to_addresses` | `bindings/python/tests/test_stable_row_ids.py::test_indexes_match_pylance`, `bindings/python/tests/test_zoned_indexes.py::test_zonemap_survives_compaction_with_stable_row_ids` | unit+interop | indices of a stable-row-id dataset hold row ids, translated at query time |
| `index.btree.files` | [BTree Index](https://lance.org/format/index/scalar/btree/) | done | `src/index/scalar/scalar_index.cpp::write_btree` | `nano_lance_scalar_segment`, `bindings/python/tests/test_scalar_index.py::test_btree_contents_equal_pylances` | unit+interop | page_lookup.lance and page_data.lance of 4096-row pages; written at index_version 0 holding row addresses, as Lance 12 writes them |
| `index.btree.queries` | [BTree Index](https://lance.org/format/index/scalar/btree/) | done | `src/index/index_search.cpp::search_btree`, `src/index/index_search.cpp::prune_by_bounds` | `nano_lance_index_candidates`, `bindings/python/tests/test_scalar_index.py::test_pylance_answers_from_nanolance_indexes`, `bindings/python/tests/test_scalar_index.py::test_nanolance_answers_from_either_builders_indexes` | unit+interop | equals, range, IN, IS NULL; exact |
| `index.btree.rowid_lookup` | [BTree Index: Stable Row ID Reverse Lookup](https://lance.org/format/index/scalar/btree/) | missing | | | - | a BTREE on the logical `_rowid` field (-3) is neither built nor used |
| `index.bitmap.files` | [Bitmap Index](https://lance.org/format/index/scalar/bitmap/) | done | `src/index/scalar/scalar_index.cpp::write_bitmap` | `bindings/python/tests/test_scalar_index.py::test_bitmap_contents_equal_pylances` | interop | bitmap_page_lookup.lance, one RowAddrTreeMap per key, the null key first |
| `index.bitmap.queries` | [Bitmap Index](https://lance.org/format/index/scalar/bitmap/) | done | `src/index/index_search.cpp::search_bitmap` | `nano_lance_index_candidates`, `bindings/python/tests/test_scalar_index.py::test_pylance_answers_from_nanolance_indexes` | unit+interop | equals, range, IN, IS NULL; exact |
| `index.label_list.files` | [Label List Index](https://lance.org/format/index/scalar/label_list/) | done | `src/index/scalar/scalar_index.cpp::write_bitmap` | `bindings/python/tests/test_scalar_index.py::test_bitmap_contents_equal_pylances` | interop | a bitmap over the elements plus the null lists' buffer |
| `index.label_list.queries` | [Label List Index](https://lance.org/format/index/scalar/label_list/) | done | `src/index/index_search.cpp::search_bitmap` | `bindings/python/tests/test_scalar_index.py::test_pylance_answers_from_nanolance_indexes` | interop | array_has_any, array_has_all, array_contains, IS NULL |
| `index.zonemap.files` | [Zone Map Index](https://lance.org/format/index/scalar/zonemap/) | partial | `src/index/scalar/scalar_index.cpp::write_zonemap`, `src/index/index_search.cpp::load_zonemap` | `bindings/python/tests/test_zoned_indexes.py::test_zonemap_files_match_pylance`, `bindings/python/tests/test_zoned_indexes.py::test_zonemap_nan_and_null_statistics_match_pylance` | interop | version 0 statistics and the null bitmap buffer; version 1 (negative_nan_count) is neither written nor read; ZoneMapIndexDetails is written empty (no rows_per_zone, use_seeds, has_null_bitmap) |
| `index.zonemap.queries` | [Zone Map Index](https://lance.org/format/index/scalar/zonemap/) | partial | `src/index/index_search.cpp::search_zonemap` | `bindings/python/tests/test_zoned_indexes.py::test_zonemap_pylance_index_read_by_nanolance`, `bindings/python/tests/test_count_from_index.py::test_inexact_index_counts_correct_and_scan` | interop | equals, range, IN, IS NULL as candidate zones (AtMost); IS NULL never uses the null bitmap for an exact answer |
| `index.zone_write_seeds` | [Zone Write Seeds](https://lance.org/format/index/scalar/zone_write_seeds/) | missing | | | - | no seeds are written in data files or harvested by builds; FLAG_ZONE_WRITE_SEEDS unsupported |
| `index.bloomfilter.files` | [Bloom Filter Index](https://lance.org/format/index/scalar/bloom_filter/) | done | `src/index/scalar/scalar_index.cpp::write_bloomfilter`, `src/index/scalar/bloom_filter.cpp::log2_bytes_for` | `nano_lance_bloom_filter`, `bindings/python/tests/test_zoned_indexes.py::test_bloomfilter_files_match_pylance`, `bindings/python/tests/test_zoned_indexes.py::test_bloomfilter_parameters_match_pylance` | unit+interop | SBBF blocks, XXH64 seed 0, the eight salts, sizing, null bitmap |
| `index.bloomfilter.queries` | [Bloom Filter Index](https://lance.org/format/index/scalar/bloom_filter/) | done | `src/index/index_search.cpp::search_bloomfilter`, `src/index/scalar/bloom_filter.cpp::may_contain` | `bindings/python/tests/test_zoned_indexes.py::test_bloomfilter_pylance_index_read_by_nanolance` | interop | equals, IN (AtMost); IS NULL by has_null |
| `index.ngram.files` | [N-gram Index](https://lance.org/format/index/scalar/ngram/) | done | `src/index/scalar/scalar_index.cpp::write_ngram`, `src/index/fts/fts_tokenizer.cpp::ngram_tokens` | `nano_lance_scalar_segment`, `bindings/python/tests/test_ngram_json_index.py::test_ngram_tokens_and_postings_equal_pylance` | unit+interop | ngram_postings.lance: hashed trigrams of the folded, lower-cased text |
| `index.ngram.contains` | [N-gram Index](https://lance.org/format/index/scalar/ngram/) | done | `src/index/index_search.cpp::search_ngram` | `bindings/python/tests/test_ngram_json_index.py::test_ngram_after_deletes_and_appends` | interop | contains(col, 'x'), AtMost |
| `index.ngram.regex_like` | [N-gram Index](https://lance.org/format/index/scalar/ngram/) | missing | | | - | regexp_like / regexp_match and infix LIKE are not routed to the index (they scan) |
| `index.json` | [JSON Index](https://lance.org/format/index/scalar/json/) | partial | `src/index/scalar/scalar_index.cpp::extract_json_values`, `src/index/index_search.cpp::search_json` | `bindings/python/tests/test_ngram_json_index.py::test_json_index_pages_equal_pylance`, `bindings/python/tests/test_ngram_json_index.py::test_json_index_routes_each_operator_as_pylance`, `bindings/python/tests/test_ngram_json_index.py::test_each_engine_reads_the_others_json_index` | interop | a BTREE target only; json_get_* routing; other target index types are not built or searched |
| `index.fts.details` | [Full Text Search Index: Index Details](https://lance.org/format/index/scalar/fts/) | done | `src/index/fts/fts_index_build.cpp::inverted_details` | `nano_lance_inverted_segment`, `bindings/python/tests/test_fts.py::test_index_settings_match_pylance_build` | unit+interop | InvertedIndexDetails with code_config, block_size, granularity, posting_format_version |
| `index.fts.files` | [Full Text Search Index: Storage Layout](https://lance.org/format/index/scalar/fts/) | done | `src/index/fts/fts_index_build.cpp::dataset_create_inverted_index`, `src/index/fts/fts_search.cpp::load_inverted` | `nano_lance_fts_search`, `bindings/python/tests/test_fts.py::test_index_matches_pylance_build`, `bindings/python/tests/test_fts.py::test_format_version_builds_match_pylance`, `bindings/python/tests/test_fts.py::test_block_256_builds_match_pylance` | unit+interop | tokens, docs (total_tokens), invert (posting_block_size, positions), metadata (partitions, params); formats 1 to 3, blocks of 128 and 256 |
| `index.fts.partitions` | [Full Text Search Index: Storage Layout](https://lance.org/format/index/scalar/fts/) | done | `src/index/fts/fts_search.cpp::load_partition` | `bindings/python/tests/test_distributed_index.py::test_inverted_legacy_partial_build`, `bindings/python/tests/test_distributed_index.py::test_inverted_segments_score_as_one_index` | interop | every partition searched and scored together; nanolance's own build writes one partition |
| `index.fts.tokenizers` | [Full Text Search Index: Tokenizers](https://lance.org/format/index/scalar/fts/) | partial | `src/index/fts/fts_tokenizer.cpp::init` | `nano_lance_fts_tokenizer`, `bindings/python/tests/test_fts.py::test_ngram_tokenizer_index_matches_pylance`, `bindings/python/tests/test_fts.py::test_code_analyzer_index_matches_pylance` | unit+interop | simple, whitespace, raw, ngram, code; icu and icu/split are refused |
| `index.fts.jieba` | [Full Text Search Index: Jieba Tokenizer](https://lance.org/format/index/scalar/fts/) | done | `src/index/fts/fts_jieba.cpp::tokenize_search` | `nano_lance_fts_jieba`, `bindings/python/tests/test_fts_jieba.py::test_jieba_index_built_and_searched_by_both_engines` | unit+interop | with models the tests write themselves (dict.txt, config.json with user dictionaries); no downloaded model is tested |
| `index.fts.lindera` | [Full Text Search Index: Lindera Tokenizer](https://lance.org/format/index/scalar/fts/) | partial | `src/index/fts/fts_lindera.cpp::tokenize` | `nano_lance_fts_lindera`, `bindings/python/tests/test_fts_lindera.py::test_lindera_index_built_and_searched_by_both_engines`, `bindings/python/tests/test_fts_lindera.py::test_lindera_user_dictionaries` | unit+interop | the C++ test covers config and dictionary refusals only; tokenizing is tested from Python with dictionaries it generates; token filters and character filters other than mapping are refused |
| `index.fts.filters` | [Full Text Search Index: Token Filters](https://lance.org/format/index/scalar/fts/) | partial | `src/index/fts/fts_tokenizer.cpp::emit` | `nano_lance_fts_tokenizer`, `bindings/python/tests/test_fts.py::test_index_settings_match_pylance_build`, `bindings/python/tests/test_fts.py::test_language_names_are_validated_as_pylance_does` | unit+interop | remove-long, lower-case, ascii folding; stemming and stop words in English only (the other 17 languages are refused) |
| `index.fts.documents` | [Full Text Search Index: Document Type](https://lance.org/format/index/scalar/fts/) | partial | `src/index/fts/fts_document_text.hpp::row_text` | `nano_lance_fts_document_text`, `bindings/python/tests/test_fts_lists.py::test_row_index_files_match_pylance`, `bindings/python/tests/test_fts_lists.py::test_element_index_files_match_pylance` | unit+interop | text and list of text, row and list-element granularity; JSON documents (path,type,value triplets) are refused |
| `index.fts.training` | [Full Text Search Index: Training Process](https://lance.org/format/index/scalar/fts/) | partial | `src/index/fts/fts_index_build.cpp::dataset_create_inverted_index` | `nano_lance_fts_search`, `bindings/python/tests/test_distributed_index.py::test_inverted_legacy_partial_build` | unit+interop | one in-memory partition, no spill to part files and no LANCE_FTS_* settings; the distributed flow's per-fragment parts and metadata merge are the dataset layer's |
| `index.fts.queries` | [Full Text Search Index: Accelerated Queries](https://lance.org/format/index/scalar/fts/) | partial | `src/index/fts/fts_search.cpp::dataset_full_text_search`, `src/index/fts/fts_search.cpp::parse_fts_query` | `nano_lance_fts_search`, `bindings/python/tests/test_fts.py::test_query_objects`, `bindings/python/tests/test_fts_phrase.py::test_nanolance_index_searched_as_pylance_index`, `bindings/python/tests/test_fts.py::test_fuzzy_rows_and_scores_match_pylance` | unit+interop | match, phrase, boolean, multi_match, boost, fuzzy; the contains_tokens SQL function is not supported |
| `index.fmindex` | [FM-Index](https://lance.org/format/index/scalar/fmindex/) | missing | | | - | not built or read |
| `index.minhash_lsh` | [MinHash LSH Index](https://lance.org/format/index/scalar/minhash_lsh/) | missing | | | - | not built or read |
| `index.rtree` | [R-Tree Index](https://lance.org/format/index/scalar/rtree/) | missing | | | - | not built or read; no spatial predicates |
| `index.vector.ivf` | [Vector Indices: Index File](https://lance.org/format/index/vector/) | done | `src/index/vector/vector_index_build.cpp::dataset_create_vector_index`, `src/index/vector/vector_search.cpp::load_ivf` | `nano_lance_vector_search`, `nano_lance_vector_segment`, `bindings/python/tests/test_vector_search.py::test_pylance_searches_nanolances_index`, `bindings/python/tests/test_vector_search.py::test_vector_index_reader_matches_pylance` | unit+interop | index.idx with lance:index, lance:ivf (centroids tensor, offsets, lengths, loss), k-means as Lance trains it |
| `index.vector.flat` | [Vector Indices: FLAT](https://lance.org/format/index/vector/) | done | `src/index/vector/vector_search.cpp::dataset_nearest` | `nano_lance_vector_search`, `bindings/python/tests/test_vector_search.py::test_matches_pylance_from_its_index`, `bindings/python/tests/test_binary_ivf.py::test_nanolance_searches_pylance_hamming_index_like_pylance` | unit+interop | IVF_FLAT over float32 and Hamming over uint8 vectors |
| `index.vector.pq` | [Vector Indices: PQ](https://lance.org/format/index/vector/) | done | `src/index/vector/vector_index_build.cpp::create_vector_index`, `src/index/vector/vector_search.cpp::dataset_nearest` | `nano_lance_vector_search`, `nano_lance_vector_segment`, `bindings/python/tests/test_vector_search.py::test_pylance_searches_nanolances_index` | unit+interop | 8 and 4 bits, transposed or not, codebook tensor in auxiliary.idx |
| `index.vector.sq` | [Vector Indices: SQ](https://lance.org/format/index/vector/) | done | `src/index/vector/vector_index_build.cpp::create_vector_index` | `nano_lance_vector_search`, `bindings/python/tests/test_hnsw.py::test_nanolance_builds_read_by_pylance`, `bindings/python/tests/test_hnsw.py::test_new_kinds_match_pylance` | unit+interop | 8-bit codes and their bounds |
| `index.vector.hnsw` | [Vector Indices: HNSW](https://lance.org/format/index/vector/) | done | `src/index/vector/hnsw_build.cpp::build`, `src/index/vector/vector_search.cpp::dataset_nearest` | `nano_lance_hnsw_build`, `nano_lance_vector_search`, `bindings/python/tests/test_hnsw.py::test_matches_pylance_from_its_index`, `bindings/python/tests/test_hnsw.py::test_pylance_searches_nanolances_index` | unit+interop | IVF_HNSW_FLAT / _SQ / _PQ: lance:hnsw entry points, params and level offsets, graph rows |
| `index.vector.rq` | [Vector Indices: RQ](https://lance.org/format/index/vector/) | partial | `src/index/vector/vector_search.cpp::load_rq_metadata`, `src/index/vector/vector_index_build.cpp::write_rq_auxiliary` | `nano_lance_rabitq_build`, `bindings/python/tests/test_ivf_rq.py::test_matches_pylance_from_its_index`, `bindings/python/tests/test_ivf_rq.py::test_nanolance_build_searched_by_pylance_as_nanolance` | unit+interop | 1 to 9 bits, blocked ex-codes, the raw-query estimator and the fast rotation; a dense rotation matrix, the residual-query estimator and the legacy __ex_codes column are refused |
| `index.vector.carried` | [Vector Indices: Auxiliary File](https://lance.org/format/index/vector/) | missing | | | - | no carried columns or covering_field_ids in auxiliary.idx |
| `index.vector.queries` | [Vector Indices](https://lance.org/format/index/vector/) | done | `src/index/vector/vector_search.cpp::dataset_nearest` | `nano_lance_vector_search`, `bindings/python/tests/test_vector_search.py::test_filters_before_and_after_the_search`, `bindings/python/tests/test_multivector.py::test_ivf_flat_xtr_matches_pylance` | unit+interop | l2, cosine, dot, hamming; nprobes, refine, pre- and post-filters, multivectors, fast_search |
| `index.op.create` | [Creating and Updating Index Segments](https://lance.org/format/index/) | done | `src/index/scalar/scalar_index.cpp::dataset_create_scalar_index`, `src/index/fts/fts_index_build.cpp::dataset_create_inverted_index`, `src/index/vector/vector_index_build.cpp::dataset_create_vector_index` | `nano_lance_index_candidates`, `nano_lance_fts_search`, `nano_lance_vector_search`, `bindings/python/tests/test_scalar_index.py::test_names_replace_and_refusals` | unit+interop | one segment over every fragment, committed; replace drops the old segments |
| `index.op.optimize` | [Creating and Updating Index Segments](https://lance.org/format/index/) | done | `src/dataset/index_optimize.cpp::dataset_optimize_indices` | `bindings/python/tests/test_index_optimize.py::test_optimize_after_append`, `bindings/python/tests/test_index_optimize.py::test_vector_model_kept_or_retrained`, `bindings/python/tests/test_index_optimize.py::test_optimize_plans_on_stored_sizes_as_lance`, `bindings/python/tests/test_zoned_indexes.py::test_zonemap_maintenance_matches_pylance`, `bindings/python/tests/test_hnsw.py::test_built_kinds_take_appended_rows_by_optimize` | interop | appended rows folded into a new segment with the old one's parameters or model (the dataset layer); a vector index plans splits and joins on every segment's stored partition sizes plus the new rows, as Lance does: a delta segment when there are none, a merge of every segment when there are |
| `index.op.merge_segments` | [Basic Concepts](https://lance.org/format/index/) | done | `src/dataset/index_segments.cpp::dataset_merge_index_segments` | `nano_lance_inverted_segment`, `nano_lance_scalar_segment`, `bindings/python/tests/test_distributed_index.py::test_scalar_segments_merged_and_committed`, `bindings/python/tests/test_distributed_index.py::test_vector_segments_with_a_shared_model` | unit+interop | segments over disjoint fragments committed as one index (the dataset layer) |
| `index.op.drop` | [Index Storage](https://lance.org/format/index/) | done | `src/index/scalar/scalar_index.cpp::dataset_drop_index` | `bindings/python/tests/test_ngram_json_index.py::test_json_index_drop_then_scan`, `bindings/python/tests/test_ngram_json_index.py::test_ngram_drop_and_empty_dataset` | interop | every segment removed from the manifest; the files stay |

## Known gaps

- **Index versions and carried columns.** A reader is meant to skip a segment whose `index_version` it does
  not know, and to serve carried columns only after checking the segment's storage. nanolance checks
  neither and writes neither carried columns nor `covering_field_ids`: all its readers assume the layouts
  Lance 12 writes. Adding a per-type version table to `index_search.cpp`, `fts_search.cpp` and
  `vector_search.cpp` is small and matters as soon as Lance writes a newer layout (BTREE / BITMAP version
  1, ZONEMAP version 1 are already specified).
- **ZoneMap version 1 and write seeds.** The zonemap reader takes only the 7-column version 0 file, writes
  `ZoneMapIndexDetails` empty, and answers IS NULL from null counts even when the null bitmap is there.
  Zone write seeds (statistics in data files' global buffers) are not implemented at all; the file layer
  would write them and the builder harvest them.
- **Index kinds not implemented**: FM-index, MinHash LSH, R-Tree, the BTREE `_rowid` reverse lookup, JSON
  indices over targets other than BTREE. Each is a new builder and search; the R-Tree also needs spatial
  predicates in the expr layer.
- **NGRAM** answers `contains` only; deriving trigram conditions from regular expressions and infix LIKE
  patterns would let it prune those scans.
- **FTS**: no ICU tokenizer (it needs ICU4X segmentation data), stemming and stop words in English only,
  no JSON documents, no `contains_tokens` SQL function, and the builder holds the whole index in memory as
  one partition (fine for the sizes nanolance targets; Lance spills to part files).
- **IVF_RQ** reads and writes only the fast rotation with the raw-query estimator, which is what Lance 12
  writes by default; older indexes with a dense rotation matrix or residual-query factors are refused.
- **Remapping**: compaction rebuilds segments rather than rewriting their row addresses, and only the
  scalar search remaps through a fragment reuse index; FTS and vector answers over a deferred compaction
  rely on the dataset layer narrowing coverage.
- **Module tests**: `fts_index_build` and `vector_index_build` have no header of their own (their
  interfaces are `fts_search.hpp`, `vector_search.hpp` and the segment headers), so no test can include
  them; `nano_lance_fts_search`, `nano_lance_inverted_segment`, `nano_lance_vector_search` and
  `nano_lance_vector_segment` exercise them.
