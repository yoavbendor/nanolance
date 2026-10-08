// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// liblance_c's vector and full-text search, end to end through lance-c's C API: a dataset written
// with lance_dataset_write, IVF_FLAT / IVF_PQ / INVERTED indexes built with lance-c's index calls,
// and every search compared, row for row, with nanolance's own search (which test_vector_search.py
// and test_fts.py check against pylance): the rows, their order, `_distance` / `_score`, the column
// order Lance returns (the columns asked for, the value, then the row id columns), offset and
// limit, filters, prepared full-text contexts and their coverage modes, and the errors lance-c
// documents.

#include <nanoarrow/nanoarrow.h>

#include "lance/lance.h"
#include "nanolance/fts_search.hpp"
#include "nanolance/vector_search.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string last_error() {
    const char* m = lance_last_error_message();
    std::string s = m != nullptr ? m : "";
    lance_free_string(m);
    return s;
}

constexpr int kDim = 8;
const char* kWords[] = {"apple", "pear", "plum", "fig", "kiwi", "lime", "date", "grape", "melon", "peach"};

/// `n` rows from `start`: id, text, vec (around 6 centres).
void make_batch(int64_t start, int64_t n, ArrowSchema& schema, ArrowArray& array) {
    ArrowSchemaInit(&schema);
    ArrowSchemaSetTypeStruct(&schema, 3);
    ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(schema.children[0], "id");
    ArrowSchemaSetType(schema.children[1], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(schema.children[1], "text");
    ArrowSchemaSetTypeFixedSize(schema.children[2], NANOARROW_TYPE_FIXED_SIZE_LIST, kDim);
    ArrowSchemaSetName(schema.children[2], "vec");
    ArrowSchemaSetType(schema.children[2]->children[0], NANOARROW_TYPE_FLOAT);
    ArrowArrayInitFromSchema(&array, &schema, nullptr);
    ArrowArrayStartAppending(&array);
    std::mt19937_64 rng(static_cast<uint64_t>(start) + 7U);
    std::normal_distribution<float> noise(0.0F, 1.0F);
    for (int64_t i = start; i < start + n; ++i) {
        ArrowArrayAppendInt(array.children[0], i);
        std::string text;
        const int words = 2 + static_cast<int>(rng() % 6);
        for (int w = 0; w < words; ++w) {
            text += std::string(w == 0 ? "" : " ") + kWords[rng() % 10];
        }
        ArrowArrayAppendString(array.children[1], ArrowCharView(text.c_str()));
        const int centre = static_cast<int>(i % 6);
        for (int d = 0; d < kDim; ++d) {
            ArrowArrayAppendDouble(array.children[2]->children[0], (d == centre ? 4.0 : 0.0) + noise(rng));
        }
        ArrowArrayFinishElement(array.children[2]);
        ArrowArrayFinishElement(&array);
    }
    ArrowArrayFinishBuildingDefault(&array, nullptr);
}

bool write(const std::string& uri, int64_t start, int64_t n, int32_t mode) {
    ArrowSchema schema;
    ArrowArray array;
    make_batch(start, n, schema, array);
    ArrowArrayStream stream;
    ArrowBasicArrayStreamInit(&stream, &schema, 1);
    ArrowBasicArrayStreamSetArray(&stream, 0, &array);
    ArrowSchema schema2;
    ArrowSchemaDeepCopy(&schema, &schema2);
    LanceDataset* out = nullptr;
    const int32_t rc = lance_dataset_write(uri.c_str(), &schema2, &stream, mode, nullptr, &out);
    schema2.release(&schema2);
    if (out != nullptr) {
        lance_dataset_close(out);
    }
    return rc == 0;
}

struct Result {
    std::vector<std::string> names;
    std::vector<int64_t> ids;
    std::vector<float> values;
    std::vector<uint64_t> row_ids;
    bool ok = false;
    std::string error;
};

Result collect(LanceScanner* scanner, const char* value_name) {
    Result r;
    ArrowArrayStream stream{};
    if (lance_scanner_to_arrow_stream(scanner, &stream) != 0) {
        r.error = last_error();
        return r;
    }
    ArrowSchema schema;
    stream.get_schema(&stream, &schema);
    int id_col = -1, value_col = -1, rowid_col = -1;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        r.names.emplace_back(schema.children[i]->name);
        if (r.names.back() == "id") id_col = static_cast<int>(i);
        if (r.names.back() == value_name) value_col = static_cast<int>(i);
        if (r.names.back() == "_rowid") rowid_col = static_cast<int>(i);
    }
    while (true) {
        ArrowArray batch;
        if (stream.get_next(&stream, &batch) != 0 || batch.release == nullptr) {
            break;
        }
        ArrowArrayView view;
        ArrowArrayViewInitFromSchema(&view, &schema, nullptr);
        ArrowArrayViewSetArray(&view, &batch, nullptr);
        for (int64_t row = 0; row < batch.length; ++row) {
            if (id_col >= 0) r.ids.push_back(ArrowArrayViewGetIntUnsafe(view.children[id_col], row));
            if (value_col >= 0) r.values.push_back(static_cast<float>(ArrowArrayViewGetDoubleUnsafe(view.children[value_col], row)));
            if (rowid_col >= 0) r.row_ids.push_back(ArrowArrayViewGetUIntUnsafe(view.children[rowid_col], row));
        }
        ArrowArrayViewReset(&view);
        batch.release(&batch);
    }
    schema.release(&schema);
    stream.release(&stream);
    r.ok = true;
    return r;
}

/// The ids of rows by address (ids are written in row order, 0.. in fragment 0, ...).
std::vector<int64_t> ids_of(const std::string& uri, const std::vector<uint64_t>& addrs) {
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    const char* cols[] = {"id", nullptr};
    ArrowArrayStream stream{};
    lance_dataset_take_rows(ds, addrs.data(), addrs.size(), cols, &stream);
    std::vector<int64_t> out;
    ArrowSchema schema;
    stream.get_schema(&stream, &schema);
    while (true) {
        ArrowArray batch;
        if (stream.get_next(&stream, &batch) != 0 || batch.release == nullptr) break;
        ArrowArrayView view;
        ArrowArrayViewInitFromSchema(&view, &schema, nullptr);
        ArrowArrayViewSetArray(&view, &batch, nullptr);
        for (int64_t row = 0; row < batch.length; ++row) out.push_back(ArrowArrayViewGetIntUnsafe(view.children[0], row));
        ArrowArrayViewReset(&view);
        batch.release(&batch);
    }
    schema.release(&schema);
    stream.release(&stream);
    lance_dataset_close(ds);
    return out;
}

std::vector<float> query_vector(int seed) {
    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    std::normal_distribution<float> noise(0.0F, 1.0F);
    std::vector<float> q(kDim);
    for (auto& x : q) x = noise(rng) * 2.0F;
    return q;
}

void test_nearest(const std::string& uri, const char* index_name) {
    for (int seed = 0; seed < 8; ++seed) {
        const auto q = query_vector(seed);
        for (const bool with_filter : {false, true}) {
            nano_lance::NearestQuery core;
            core.column = "vec";
            core.key = q;
            core.k = 15;
            core.minimum_nprobes = 2;
            core.maximum_nprobes = 2;
            core.refine_factor = seed % 2 == 0 ? std::optional<uint32_t>{} : std::optional<uint32_t>{3};
            if (seed % 3 == 0) core.ef = 60;  // the HNSW beam (no effect on IVF_FLAT / IVF_PQ)
            if (seed % 3 == 1) core.query_parallelism = 1;
            if (with_filter) core.filter = "id % 3 = 1";
            core.prefilter = with_filter && seed % 4 < 2;
            nano_lance::NearestResult want;
            std::string error;
            const bool found = nano_lance::dataset_nearest(uri, core, want, error);
            check(found, "core nearest: " + error);

            LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
            const char* cols[] = {"id", nullptr};
            LanceScanner* sc = lance_scanner_new(ds, cols, with_filter ? "id % 3 = 1" : nullptr);
            check(lance_scanner_nearest(sc, "vec", q.data(), q.size(), LANCE_DTYPE_FLOAT32, 15) == 0, "nearest");
            check(lance_scanner_set_nprobes(sc, 2) == 0, "nprobes");
            if (core.refine_factor) check(lance_scanner_set_refine_factor(sc, 3) == 0, "refine");
            if (core.ef) check(lance_scanner_set_ef(sc, 60) == 0, "ef");
            if (core.query_parallelism == 1) check(lance_scanner_set_query_parallelism(sc, 1) == 0, "parallelism");
            check(lance_scanner_set_prefilter(sc, core.prefilter) == 0, "prefilter");
            check(lance_scanner_with_row_id(sc, true) == 0, "with_row_id");
            const auto got = collect(sc, "_distance");
            lance_scanner_close(sc);
            lance_dataset_close(ds);
            check(got.ok, std::string(index_name) + " nearest: " + got.error);
            check(got.names == std::vector<std::string>{"id", "_distance", "_rowid"},
                  std::string(index_name) + ": columns id, _distance, _rowid");
            // Unfiltered: 15 rows. A pre-filter over 2 probed partitions may find fewer, and a post-filter
            // may leave none of the 15 nearest (rows cluster by id % 6), as in Lance.
            check(with_filter ? (!core.prefilter || !want.row_ids.empty()) : want.row_ids.size() == 15U,
                  std::string(index_name) + " nearest finds rows: seed " + std::to_string(seed) + " filter " +
                      std::to_string(with_filter) + " prefilter " + std::to_string(core.prefilter) + " got " +
                      std::to_string(want.row_ids.size()));
            check(got.row_ids == want.row_ids, std::string(index_name) + ": the rows and their order");
            check(got.values == want.distances, std::string(index_name) + ": the distances");
            check(got.ids == ids_of(uri, want.row_ids), std::string(index_name) + ": the ids are the rows'");
        }
    }
    // Offset and limit apply to the k nearest; the projection may leave out the value.
    const auto q = query_vector(99);
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    LanceScanner* all = lance_scanner_new(ds, nullptr, nullptr);
    lance_scanner_nearest(all, "vec", q.data(), q.size(), LANCE_DTYPE_FLOAT32, 10);
    const auto full = collect(all, "_distance");
    lance_scanner_close(all);
    check(full.names == std::vector<std::string>{"id", "text", "vec", "_distance"}, "all columns, then _distance");
    LanceScanner* page = lance_scanner_new(ds, nullptr, nullptr);
    lance_scanner_nearest(page, "vec", q.data(), q.size(), LANCE_DTYPE_FLOAT32, 10);
    lance_scanner_set_offset(page, 3);
    lance_scanner_set_limit(page, 4);
    const auto part = collect(page, "_distance");
    lance_scanner_close(page);
    check(part.ids.size() == 4 && std::equal(part.ids.begin(), part.ids.end(), full.ids.begin() + 3),
          "offset 3, limit 4 of the 10 nearest");
    // float64 queries are converted.
    std::vector<double> q64(q.begin(), q.end());
    LanceScanner* d64 = lance_scanner_new(ds, nullptr, nullptr);
    lance_scanner_nearest(d64, "vec", q64.data(), q64.size(), LANCE_DTYPE_FLOAT64, 10);
    check(collect(d64, "_distance").ids == full.ids, "a float64 query");
    lance_scanner_close(d64);
    lance_dataset_close(ds);
}

void test_fts(const std::string& uri) {
    const char* queries[] = {"apple", "plum fig", "melon peach grape", "kiwi"};
    for (const char* text : queries) {
        nano_lance::FtsSearchRequest core;
        core.query.kind = nano_lance::FtsQuery::Kind::MultiMatch;
        core.query.text = text;
        core.limit = 25;
        nano_lance::FtsSearchResult want;
        std::string error;
        check(nano_lance::dataset_full_text_search(uri, core, want, error), "core fts: " + error);

        LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
        LanceScanner* sc = lance_scanner_new(ds, nullptr, nullptr);
        const char* cols[] = {"text", nullptr};
        check(lance_scanner_full_text_search(sc, text, cols, 0) == 0, "full_text_search");
        lance_scanner_set_limit(sc, 25);
        lance_scanner_with_row_id(sc, true);
        const auto got = collect(sc, "_score");
        lance_scanner_close(sc);
        check(got.ok, std::string("fts: ") + got.error);
        check(got.names == std::vector<std::string>{"id", "text", "vec", "_score", "_rowid"},
              "columns, _score, _rowid");
        check(!want.row_ids.empty(), std::string("fts finds rows: ") + text);
        check(got.row_ids == want.row_ids && got.values == want.scores, std::string("fts rows and scores: ") + text);

        // A prepared Match context (OR and AND), every fragment indexed.
        for (const int32_t op : {LANCE_FTS_MATCH_OPERATOR_OR, LANCE_FTS_MATCH_OPERATOR_AND}) {
            nano_lance::FtsSearchRequest m;
            m.query.kind = nano_lance::FtsQuery::Kind::Match;
            m.query.text = text;
            m.query.columns = {"text"};
            m.query.and_operator = op == LANCE_FTS_MATCH_OPERATOR_AND;
            nano_lance::FtsSearchResult mw;
            check(nano_lance::dataset_full_text_search(uri, m, mw, error), "core match: " + error);
            LanceFtsQueryContext* ctx =
                lance_dataset_prepare_fts_match_query(ds, "text", text, op, 0, LANCE_FTS_COVERAGE_STRICT);
            check(ctx != nullptr, "prepare match: " + last_error());
            LanceScanner* cs = lance_scanner_new(ds, nullptr, nullptr);
            check(lance_scanner_set_fts_query_context(cs, ctx) == 0, "set context");
            lance_fts_query_context_close(ctx);  // the scanner keeps its own
            lance_scanner_with_row_id(cs, true);
            const auto cg = collect(cs, "_score");
            lance_scanner_close(cs);
            check(op == LANCE_FTS_MATCH_OPERATOR_AND || !mw.row_ids.empty(), "match finds rows");
            check(cg.row_ids == mw.row_ids && cg.values == mw.scores, std::string("context rows and scores: ") + text);
        }
        lance_dataset_close(ds);
    }
}

void test_errors(const std::string& uri) {
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    const auto q = query_vector(1);
    LanceScanner* sc = lance_scanner_new(ds, nullptr, nullptr);
    check(lance_scanner_set_nprobes(sc, 0) != 0 && lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT, "nprobes 0");
    check(lance_scanner_set_maximum_nprobes(sc, 4) == 0 && lance_scanner_set_minimum_nprobes(sc, 5) != 0,
          "minimum over maximum");
    check(lance_scanner_nearest(sc, "vec", q.data(), q.size(), LANCE_DTYPE_FLOAT32, 0) != 0, "k 0");
    check(lance_scanner_nearest(sc, "vec", q.data(), q.size(), LANCE_DTYPE_FLOAT32, 5) == 0, "nearest");
    check(lance_scanner_full_text_search(sc, "apple", nullptr, 0) != 0 &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "full_text_search after nearest");
    check(lance_scanner_set_metric(sc, LANCE_METRIC_HAMMING) != 0 &&
              lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
          "hamming");
    check(lance_scanner_set_approx_mode(sc, LANCE_APPROX_MODE_ACCURATE) == 0 && lance_scanner_set_ef(sc, 64) == 0 &&
              lance_scanner_set_query_parallelism(sc, 2) == 0,
          "settings without an effect on IVF");
    check(lance_scanner_set_query_parallelism(sc, -2) != 0 && lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "query_parallelism -2");
    check(lance_scanner_nearest_multivector(sc, "vec", q.data(), kDim, 1, LANCE_DTYPE_FLOAT32, 1) != 0,
          "multivector");
    lance_scanner_close(sc);

    LanceScanner* fs = lance_scanner_new(ds, nullptr, nullptr);
    check(lance_scanner_full_text_search(fs, "apple", nullptr, 2) != 0 &&
              lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
          "fuzzy");
    lance_scanner_close(fs);
    check(lance_dataset_prepare_fts_match_query(ds, "id", "x", 0, 0, LANCE_FTS_COVERAGE_STRICT) == nullptr &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "no FTS index on id");
    check(lance_dataset_prepare_fts_phrase_query(ds, "text", "apple pear", 0, LANCE_FTS_COVERAGE_STRICT) == nullptr &&
              last_error().find("positions") != std::string::npos,
          "phrase without positions");
    check(lance_dataset_prepare_fts_match_query(ds, "text", "x", 7, 0, 0) == nullptr, "bad operator");

    LanceVectorIndexParams p{};
    p.index_type = LANCE_INDEX_IVF_HNSW_SQ;
    p.num_partitions = 2;
    check(lance_dataset_create_vector_index(ds, "vec", "h", &p, true) != 0 &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT && last_error().find("hnsw_m") != std::string::npos,
          "IVF_HNSW_SQ without hnsw_m");
    p.hnsw_m = 8;
    p.num_bits = 4;
    check(lance_dataset_create_vector_index(ds, "vec", "h", &p, true) != 0 &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "4-bit SQ");
    p.num_bits = 0;
    p.hnsw_ef_construction = 4;
    check(lance_dataset_create_vector_index(ds, "vec", "h", &p, true) != 0 &&
              last_error().find("ef_construction") != std::string::npos,
          "ef_construction under m");
    p.index_type = LANCE_INDEX_IVF_HNSW_PQ;
    check(lance_dataset_create_vector_index(ds, "vec", "h", &p, true) != 0 &&
              lance_last_error_code() == LANCE_ERR_NOT_SUPPORTED,
          "IVF_HNSW_PQ");
    p.index_type = LANCE_INDEX_IVF_PQ;
    check(lance_dataset_create_vector_index(ds, "vec", "h", &p, true) != 0 &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "IVF_PQ without num_sub_vectors");
    lance_dataset_close(ds);

    // A context prepared on one snapshot is refused by a scanner of another.
    LanceDataset* a = lance_dataset_open(uri.c_str(), nullptr, 0);
    LanceDataset* b = lance_dataset_open(uri.c_str(), nullptr, 0);
    LanceFtsQueryContext* ctx = lance_dataset_prepare_fts_match_query(a, "text", "apple", 0, 0, LANCE_FTS_COVERAGE_STRICT);
    LanceScanner* other = lance_scanner_new(b, nullptr, nullptr);
    check(ctx != nullptr && lance_scanner_set_fts_query_context(other, ctx) != 0, "context from another handle");
    lance_scanner_close(other);
    lance_fts_query_context_close(ctx);
    lance_dataset_close(a);
    lance_dataset_close(b);
}

void test_coverage(const std::string& uri) {
    // Rows appended after the index: STRICT refuses, INDEX_ONLY searches the indexed rows alone.
    check(write(uri, 2000, 300, LANCE_WRITE_APPEND), "append: " + last_error());
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    check(lance_dataset_prepare_fts_match_query(ds, "text", "apple", 0, 0, LANCE_FTS_COVERAGE_STRICT) == nullptr &&
              last_error().find("STRICT") != std::string::npos,
          "STRICT with an unindexed fragment");
    LanceFtsQueryContext* ctx =
        lance_dataset_prepare_fts_match_query(ds, "text", "apple", 0, 0, LANCE_FTS_COVERAGE_INDEX_ONLY);
    check(ctx != nullptr, "INDEX_ONLY: " + last_error());
    LanceScanner* sc = lance_scanner_new(ds, nullptr, nullptr);
    lance_scanner_set_fts_query_context(sc, ctx);
    lance_fts_query_context_close(ctx);
    const auto got = collect(sc, "_score");
    lance_scanner_close(sc);
    bool only_old = !got.ids.empty();
    for (const auto id : got.ids) only_old = only_old && id < 2000;
    check(only_old, "INDEX_ONLY returns indexed rows alone");
    nano_lance::FtsSearchRequest core;
    core.query.kind = nano_lance::FtsQuery::Kind::Match;
    core.query.text = "apple";
    core.query.columns = {"text"};
    core.fast_search = true;
    nano_lance::FtsSearchResult want;
    std::string error;
    nano_lance::dataset_full_text_search(uri, core, want, error);
    check(got.values == want.scores, "INDEX_ONLY scores the indexed rows alone (fast_search)");
    lance_dataset_close(ds);
}

/// include_deleted_rows: every stored row, the deleted ones with a NULL _rowid (with_row_id required).
void test_include_deleted_rows(const std::string& uri) {
    check(write(uri, 0, 500, LANCE_WRITE_CREATE) && write(uri, 500, 500, LANCE_WRITE_APPEND), "write");
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    uint64_t deleted = 0;
    check(lance_dataset_delete(ds, "id % 7 = 3", &deleted) == 0 && deleted > 0, "delete: " + last_error());
    const char* cols[] = {"id", nullptr};
    LanceScanner* no_id = lance_scanner_new(ds, cols, nullptr);
    check(lance_scanner_set_include_deleted_rows(no_id, true) == 0, "setter");
    ArrowArrayStream stream{};
    check(lance_scanner_to_arrow_stream(no_id, &stream) != 0 && lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "include_deleted_rows needs with_row_id");
    lance_scanner_close(no_id);
    LanceScanner* sc = lance_scanner_new(ds, cols, nullptr);
    lance_scanner_with_row_id(sc, true);
    lance_scanner_set_include_deleted_rows(sc, true);
    check(lance_scanner_to_arrow_stream(sc, &stream) == 0, "scan with deleted rows: " + last_error());
    ArrowSchema schema;
    stream.get_schema(&stream, &schema);
    check((schema.children[1]->flags & ARROW_FLAG_NULLABLE) != 0, "_rowid is nullable");
    int64_t rows = 0, nulls = 0;
    bool right = true;
    while (true) {
        ArrowArray batch;
        if (stream.get_next(&stream, &batch) != 0 || batch.release == nullptr) break;
        ArrowArrayView view;
        ArrowArrayViewInitFromSchema(&view, &schema, nullptr);
        ArrowArrayViewSetArray(&view, &batch, nullptr);
        for (int64_t r = 0; r < batch.length; ++r) {
            const int64_t id = ArrowArrayViewGetIntUnsafe(view.children[0], r);
            const bool null = ArrowArrayViewIsNull(view.children[1], r);
            right = right && (null == (id % 7 == 3));
            nulls += null ? 1 : 0;
        }
        rows += batch.length;
        ArrowArrayViewReset(&view);
        batch.release(&batch);
    }
    schema.release(&schema);
    stream.release(&stream);
    lance_scanner_close(sc);
    check(rows == 1000 && nulls == static_cast<int64_t>(deleted) && right, "every stored row; the deleted ones without a _rowid");
    lance_dataset_close(ds);
}

/// merge_insert WHEN MATCHED UPDATE_IF: matched rows update only where the condition holds.
void test_update_if(const std::string& uri) {
    check(write(uri, 0, 100, LANCE_WRITE_CREATE), "write");
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    ArrowSchema schema;
    ArrowArray array;
    make_batch(90, 20, schema, array);  // ids 90..109: 10 match, 10 are new
    ArrowArrayStream source;
    ArrowBasicArrayStreamInit(&source, &schema, 1);
    ArrowBasicArrayStreamSetArray(&source, 0, &array);
    const char* on[] = {"id"};
    LanceMergeInsertParams params{};
    params.when_matched = LANCE_MERGE_WHEN_MATCHED_UPDATE_IF;
    params.when_matched_expr = "source.id % 2 = 0 AND target.id >= 90";
    params.when_not_matched = LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL;
    LanceMergeInsertResult result{};
    check(lance_dataset_merge_insert(ds, on, 1, &source, &params, &result) == 0, "merge_insert: " + last_error());
    check(result.num_updated_rows == 5 && result.num_inserted_rows == 10 && result.num_deleted_rows == 0,
          "5 updated, 10 inserted");
    check(lance_dataset_count_rows(ds) == 110, "110 rows");
    params.when_matched_expr = "";
    ArrowArrayStream empty;
    ArrowSchema s2;
    ArrowArray a2;
    make_batch(0, 1, s2, a2);
    ArrowBasicArrayStreamInit(&empty, &s2, 1);
    ArrowBasicArrayStreamSetArray(&empty, 0, &a2);
    check(lance_dataset_merge_insert(ds, on, 1, &empty, &params, nullptr) != 0 &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "UPDATE_IF without a condition");
    lance_dataset_close(ds);
}

}  // namespace

// ── index segments: built apart, committed together, searched apart ──────────────────────────────

/// One uncommitted segment of `params` (or a scalar `scalar_type`) over `fragment`.
std::vector<uint8_t> build_segment(LanceDataset* ds, const char* column, const char* name, uint32_t fragment,
                                   const LanceVectorIndexSegmentParams* params, int32_t scalar_type,
                                   LanceIndexSegmentBuildOptions options = {}) {
    options.fragment_ids = &fragment;
    options.fragment_count = 1;
    LanceIndexSegmentBuilder* b = params != nullptr
                                      ? lance_index_segment_builder_new_vector(ds, column, name, params, &options)
                                      : lance_index_segment_builder_new_scalar(ds, column, name, scalar_type, nullptr,
                                                                               &options);
    check(b != nullptr, std::string("segment builder: ") + last_error());
    uint8_t* bytes = nullptr;
    size_t len = 0;
    check(b != nullptr && lance_index_segment_builder_execute_uncommitted(b, &bytes, &len) == 0,
          "segment build: " + last_error());
    std::vector<uint8_t> out(bytes, bytes + len);
    lance_free_bytes(bytes);
    lance_index_segment_builder_free(b);
    return out;
}

int32_t commit(LanceDataset* ds, const char* name, const char* column, const std::vector<std::vector<uint8_t>>& segs) {
    std::vector<const uint8_t*> ptrs;
    std::vector<size_t> lens;
    for (const auto& s : segs) {
        ptrs.push_back(s.data());
        lens.push_back(s.size());
    }
    return lance_dataset_commit_index_segments(ds, name, column, ptrs.data(), lens.data(), segs.size());
}

std::array<uint8_t, 16> uuid_of(const std::vector<uint8_t>& seg) {
    LanceIndexSegmentMetadata* md = nullptr;
    std::array<uint8_t, 16> out{};
    if (lance_index_segment_metadata_parse(seg.data(), seg.size(), &md) == 0) {
        lance_index_segment_metadata_uuid(md, out.data());
    }
    lance_index_segment_metadata_free(md);
    return out;
}

Result nearest_in(const std::string& uri, const std::vector<float>& q, uint32_t k, const uint8_t* segs, size_t n) {
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    const char* cols[] = {"id", nullptr};
    LanceScanner* sc = lance_scanner_new(ds, cols, nullptr);
    lance_scanner_nearest(sc, "vec", q.data(), q.size(), LANCE_DTYPE_FLOAT32, k);
    lance_scanner_set_nprobes(sc, 4);
    if (n > 0U) {
        check(lance_scanner_set_index_segments(sc, segs, n) == 0, "set_index_segments");
    }
    auto r = collect(sc, "_distance");
    lance_scanner_close(sc);
    lance_dataset_close(ds);
    return r;
}

void test_segments(const std::filesystem::path& dir) {
    const std::string uri = (dir / "segments.lance").string();
    check(write(uri, 0, 1000, LANCE_WRITE_CREATE) && write(uri, 1000, 1000, LANCE_WRITE_APPEND), "write");
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    uint64_t frags[2] = {0, 0};
    lance_dataset_fragment_ids(ds, frags);

    // Vector: one IVF_PQ model trained once, a segment per fragment built with it (two "workers").
    ArrowArray centroids{};
    ArrowSchema centroids_schema{};
    ArrowArray codebook{};
    ArrowSchema codebook_schema{};
    check(lance_index_train_ivf_model(ds, "vec", 4, LANCE_METRIC_L2, nullptr, 0, &centroids, &centroids_schema) == 0,
          "train IVF: " + last_error());
    check(centroids.length == 4 && std::string(centroids_schema.format) == "+w:8", "4 centroids of 8");
    check(lance_index_train_pq_model(ds, "vec", 4, 8, LANCE_METRIC_L2, nullptr, 0, &centroids, &centroids_schema,
                                     &codebook, &codebook_schema) == 0,
          "train PQ: " + last_error());
    check(codebook.length == 4 * 256 && std::string(codebook_schema.format) == "+w:2", "a codebook of 4 x 256 x 2");
    // A codebook with centroids of another training is refused.
    ArrowArray other{};
    ArrowSchema other_schema{};
    lance_index_train_ivf_model(ds, "vec", 4, LANCE_METRIC_L2, nullptr, 0, &other, &other_schema);
    LanceVectorIndexSegmentParams pq = {LANCE_INDEX_IVF_PQ, LANCE_METRIC_L2, 4, 4, 8, 10, 0, 0, 0};
    {
        LanceIndexSegmentBuildOptions o{};
        o.ivf_centroids = &other;
        o.ivf_centroids_schema = &other_schema;
        o.pq_codebook = &codebook;
        o.pq_codebook_schema = &codebook_schema;
        check(lance_index_segment_builder_new_vector(ds, "vec", "pq", &pq, &o) == nullptr &&
                  last_error().find("not trained with the supplied ivf_centroids") != std::string::npos,
              "mismatched models");
    }
    std::vector<std::vector<uint8_t>> pq_segs;
    for (const auto f : frags) {
        LanceIndexSegmentBuildOptions o{};
        o.ivf_centroids = &centroids;
        o.ivf_centroids_schema = &centroids_schema;
        o.pq_codebook = &codebook;
        o.pq_codebook_schema = &codebook_schema;
        o.mode = LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED;
        pq_segs.push_back(build_segment(ds, "vec", "pq", static_cast<uint32_t>(f), &pq, 0, o));
    }
    for (auto* a : {&centroids, &codebook, &other}) a->release(a);
    for (auto* s : {&centroids_schema, &codebook_schema, &other_schema}) s->release(s);
    check(commit(ds, "pq", "vec", pq_segs) == 0, "commit IVF_PQ segments: " + last_error());
    check(lance_dataset_index_segment_count(ds, "pq") == 2, "two IVF_PQ segments");
    lance_dataset_close(ds);
    // Both segments share the model, so they are one index: searched together, the core's answer.
    const auto q = query_vector(5);
    nano_lance::NearestQuery core;
    core.column = "vec";
    core.key = q;
    core.k = 10;
    core.minimum_nprobes = core.maximum_nprobes.emplace(4);
    nano_lance::NearestResult want;
    std::string error;
    check(nano_lance::dataset_nearest(uri, core, want, error), "core nearest: " + error);
    const auto all = nearest_in(uri, q, 10, nullptr, 0);
    check(all.ok && all.values == want.distances, "IVF_PQ segments searched together");

    // A worker per segment: each searches its own, the union's best 10 are the whole search's.
    std::vector<std::pair<float, int64_t>> merged;
    for (const auto& seg : pq_segs) {
        const auto u = uuid_of(seg);
        const auto part = nearest_in(uri, q, 10, u.data(), 1);
        check(part.ok, "segment search: " + part.error);
        for (std::size_t i = 0; i < part.ids.size(); ++i) {
            check(part.ids[i] < 1000 == (&seg == &pq_segs[0]), "a segment returns only its own fragment's rows");
            merged.emplace_back(part.values[i], part.ids[i]);
        }
    }
    std::sort(merged.begin(), merged.end());
    merged.resize(std::min<std::size_t>(merged.size(), 10));
    std::vector<int64_t> merged_ids;
    for (const auto& m : merged) merged_ids.push_back(m.second);
    check(merged_ids == all.ids, "the segments' searches merged give the whole search");
    {
        const uint8_t unknown[16] = {1, 2, 3};
        const auto bad = nearest_in(uri, q, 10, unknown, 1);
        check(!bad.ok && bad.error.find("unknown index segments") != std::string::npos, "an unknown segment");
        LanceDataset* d = lance_dataset_open(uri.c_str(), nullptr, 0);
        LanceScanner* sc = lance_scanner_new(d, nullptr, nullptr);
        const auto u = uuid_of(pq_segs[0]);
        lance_scanner_set_index_segments(sc, u.data(), 1);
        ArrowArrayStream stream{};
        check(lance_scanner_to_arrow_stream(sc, &stream) != 0 && lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
              "index segments without nearest");
        lance_scanner_close(sc);
        lance_dataset_close(d);
    }

    // Replacement by coverage: a segment over fragment 0 alone is kept beside one over fragment 1,
    // a segment over both replaces both, one over fragment 0 alone then would orphan fragment 1.
    ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    LanceVectorIndexSegmentParams flat = {LANCE_INDEX_IVF_FLAT, LANCE_METRIC_L2, 2, 0, 0, 5, 0, 0, 0};
    check(commit(ds, "flat", "vec", {build_segment(ds, "vec", "flat", static_cast<uint32_t>(frags[0]), &flat, 0)}) == 0,
          "commit flat over fragment 0: " + last_error());
    // Building under a name that an index already holds is refused, as in Lance.
    {
        uint32_t f = static_cast<uint32_t>(frags[1]);
        LanceIndexSegmentBuildOptions o{};
        o.fragment_ids = &f;
        o.fragment_count = 1;
        LanceIndexSegmentBuilder* b = lance_index_segment_builder_new_vector(ds, "vec", "flat", &flat, &o);
        uint8_t* bytes = nullptr;
        size_t len = 0;
        check(b != nullptr && lance_index_segment_builder_execute_uncommitted(b, &bytes, &len) != 0 &&
                  lance_last_error_code() == LANCE_ERR_INDEX,
              "a segment under a taken name");
        lance_index_segment_builder_free(b);
    }
    const auto second = build_segment(ds, "vec", "flat_b", static_cast<uint32_t>(frags[1]), &flat, 0);
    const uint64_t before = lance_dataset_version(ds);
    check(commit(ds, "flat", "vec", {second}) == 0 && lance_dataset_index_segment_count(ds, "flat") == 2,
          "a disjoint segment is added: " + last_error());
    check(lance_dataset_version(ds) == before + 1, "one version per commit");
    LanceDataset* old = lance_dataset_open(uri.c_str(), nullptr, 0);
    lance_dataset_close(ds);
    ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    LanceIndexSegmentBuildOptions both{};
    both.mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
    LanceIndexSegmentBuilder* b = lance_index_segment_builder_new_vector(ds, "vec", "flat_all", &flat, &both);
    uint8_t* bytes = nullptr;
    size_t len = 0;
    check(b != nullptr && lance_index_segment_builder_execute_uncommitted(b, &bytes, &len) == 0, "segment over all");
    std::vector<uint8_t> whole(bytes, bytes + len);
    lance_free_bytes(bytes);
    lance_index_segment_builder_free(b);
    check(commit(ds, "flat", "vec", {whole}) == 0 && lance_dataset_index_segment_count(ds, "flat") == 1,
          "a covering segment replaces both: " + last_error());
    const auto part0 = build_segment(old, "vec", "flat_c", static_cast<uint32_t>(frags[0]), &flat, 0);
    check(commit(ds, "flat", "vec", {part0}) != 0 && last_error().find("orphan") != std::string::npos &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "partial overlap orphans fragments");
    check(commit(ds, "flat", "vec", {pq_segs[0]}) != 0, "an IVF_PQ segment over part of an IVF_FLAT index");
    check(commit(ds, "flat", "id", {whole}) != 0, "a segment of another column");
    check(commit(ds, "nope", "missing", {whole}) != 0 && lance_last_error_code() == LANCE_ERR_INDEX,
          "a missing column");
    lance_dataset_close(old);

    // Scalar: a BITMAP segment per fragment; a worker's scoped scan returns its fragment's rows.
    std::vector<std::vector<uint8_t>> bitmap;
    for (const auto f : frags) {
        bitmap.push_back(build_segment(ds, "id", "id_bitmap", static_cast<uint32_t>(f), nullptr, LANCE_SCALAR_BITMAP));
    }
    check(commit(ds, "id_bitmap", "id", bitmap) == 0, "commit bitmap segments: " + last_error());
    check(commit(ds, "id_bitmap", "id",
                 {build_segment(ds, "id", "id_btree", static_cast<uint32_t>(frags[0]), nullptr, LANCE_SCALAR_BTREE)}) != 0 &&
              last_error().find("cannot change index") != std::string::npos,
          "a type change must cover every fragment");
    {
        uint32_t f = static_cast<uint32_t>(frags[0]);
        const uint8_t uuid[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x4d, 0xef, 0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde};
        LanceIndexSegmentBuildOptions o{};
        o.fragment_ids = &f;
        o.fragment_count = 1;
        o.index_uuid = uuid;
        LanceIndexSegmentBuilder* bt = lance_index_segment_builder_new_scalar(ds, "id", "bt", LANCE_SCALAR_BTREE, nullptr, &o);
        uint8_t* out = nullptr;
        size_t n = 0;
        check(bt != nullptr && lance_index_segment_builder_execute_uncommitted(bt, &out, &n) != 0 &&
                  last_error().find("index_uuid is no longer accepted for BTree") != std::string::npos,
              "a BTree segment with an assigned UUID");
        lance_index_segment_builder_free(bt);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const auto u = uuid_of(bitmap[i]);
        const uint64_t f = frags[i];
        const char* cols[] = {"id", nullptr};
        LanceScanner* sc = lance_scanner_new(ds, cols, "id % 7 = 3");
        lance_scanner_set_fragment_ids(sc, &f, 1);
        check(lance_scanner_set_scalar_index_segment(sc, u.data()) == 0, "set_scalar_index_segment");
        const auto got = collect(sc, "");
        lance_scanner_close(sc);
        std::vector<int64_t> want_ids;
        for (int64_t id = static_cast<int64_t>(i) * 1000; id < static_cast<int64_t>(i + 1) * 1000; ++id) {
            if (id % 7 == 3) want_ids.push_back(id);
        }
        check(got.ok && got.ids == want_ids, "a scoped scan of fragment " + std::to_string(i) + ": " + got.error);
    }
    {
        const auto u = uuid_of(bitmap[0]);
        LanceScanner* sc = lance_scanner_new(ds, nullptr, "id = 3");
        lance_scanner_set_scalar_index_segment(sc, u.data());
        ArrowArrayStream stream{};
        check(lance_scanner_to_arrow_stream(sc, &stream) != 0 &&
                  last_error().find("explicit nonempty fragment_ids") != std::string::npos,
              "a scoped scan needs fragment_ids");
        lance_scanner_close(sc);
    }

    // Full text: an INVERTED segment per fragment; each worker's scores are the whole search's.
    std::vector<std::vector<uint8_t>> inverted;
    for (const auto f : frags) {
        inverted.push_back(build_segment(ds, "text", "text_fts", static_cast<uint32_t>(f), nullptr, LANCE_SCALAR_INVERTED));
    }
    check(commit(ds, "text_fts", "text", inverted) == 0, "commit INVERTED segments: " + last_error());
    LanceFtsQueryContext* ctx = lance_dataset_prepare_fts_match_query(ds, "text", "plum fig", 0, 0,
                                                                      LANCE_FTS_COVERAGE_STRICT);
    check(ctx != nullptr, "prepare: " + last_error());
    const auto fts = [&](const uint8_t* segs, size_t n) {
        const char* cols[] = {"id", nullptr};
        LanceScanner* sc = lance_scanner_new(ds, cols, nullptr);
        lance_scanner_set_fts_query_context(sc, ctx);
        if (n > 0U) check(lance_scanner_set_fts_index_segments(sc, segs, n) == 0, "set_fts_index_segments");
        auto r = collect(sc, "_score");
        lance_scanner_close(sc);
        return r;
    };
    const auto everything = fts(nullptr, 0);
    check(everything.ok && !everything.ids.empty(), "the full-text search: " + everything.error);
    std::map<int64_t, float> score_of;
    for (std::size_t i = 0; i < everything.ids.size(); ++i) score_of[everything.ids[i]] = everything.values[i];
    std::size_t seen = 0;
    for (std::size_t s = 0; s < 2; ++s) {
        const auto u = uuid_of(inverted[s]);
        const auto part = fts(u.data(), 1);
        check(part.ok, "segment FTS: " + part.error);
        for (std::size_t i = 0; i < part.ids.size(); ++i) {
            check((part.ids[i] < 1000) == (s == 0), "an FTS segment returns its own rows");
            check(score_of.count(part.ids[i]) == 1U && score_of[part.ids[i]] == part.values[i],
                  "a segment's score is the whole search's");
        }
        seen += part.ids.size();
    }
    check(seen == everything.ids.size(), "the segments' rows are the whole search's");
    {
        const uint8_t twice[32] = {};
        LanceScanner* sc = lance_scanner_new(ds, nullptr, nullptr);
        check(lance_scanner_set_fts_index_segments(sc, twice, 2) != 0, "duplicate FTS segment UUIDs");
        lance_scanner_close(sc);
    }
    lance_fts_query_context_close(ctx);
    lance_dataset_close(ds);
}

// ── phrase queries: an INVERTED index with positions, built and searched through lance-c ────────

void test_phrase(const std::filesystem::path& dir) {
    const std::string uri = (dir / "phrase.lance").string();
    check(write(uri, 0, 1000, LANCE_WRITE_CREATE) && write(uri, 1000, 1000, LANCE_WRITE_APPEND), "write");
    LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
    check(lance_dataset_create_scalar_index(ds, "text", "text_pos", LANCE_SCALAR_INVERTED,
                                            R"({"base_tokenizer":"simple","with_position":true})", false) == 0,
          "INVERTED with positions: " + last_error());
    for (const char* phrase : {"plum fig", "apple apple", "kiwi lime date"}) {
        for (const int32_t slop : {0, 1, 3}) {
            nano_lance::FtsSearchRequest r;
            r.query.kind = nano_lance::FtsQuery::Kind::Phrase;
            r.query.text = phrase;
            r.query.columns = {"text"};
            r.query.slop = static_cast<uint32_t>(slop);
            nano_lance::FtsSearchResult want;
            std::string error;
            check(nano_lance::dataset_full_text_search(uri, r, want, error), "core phrase: " + error);
            LanceFtsQueryContext* ctx =
                lance_dataset_prepare_fts_phrase_query(ds, "text", phrase, slop, LANCE_FTS_COVERAGE_STRICT);
            check(ctx != nullptr, "prepare phrase: " + last_error());
            LanceScanner* sc = lance_scanner_new(ds, nullptr, nullptr);
            lance_scanner_with_row_id(sc, true);
            lance_scanner_set_fts_query_context(sc, ctx);
            const auto got = collect(sc, "_score");
            lance_scanner_close(sc);
            lance_fts_query_context_close(ctx);
            const std::string what = std::string("phrase '") + phrase + "' slop " + std::to_string(slop);
            check(got.ok && got.row_ids == want.row_ids && got.values == want.scores, what + ": " + got.error);
            check(slop != 0 || std::string(phrase) != "plum fig" || !want.row_ids.empty(), what + " finds rows");
        }
    }
    // Exact phrases are a subset of slop 3's.
    const auto rows = [&](int32_t slop) {
        LanceFtsQueryContext* ctx =
            lance_dataset_prepare_fts_phrase_query(ds, "text", "plum fig", slop, LANCE_FTS_COVERAGE_STRICT);
        LanceScanner* sc = lance_scanner_new(ds, nullptr, nullptr);
        lance_scanner_with_row_id(sc, true);
        lance_scanner_set_fts_query_context(sc, ctx);
        auto r = collect(sc, "_score");
        lance_scanner_close(sc);
        lance_fts_query_context_close(ctx);
        std::sort(r.row_ids.begin(), r.row_ids.end());
        return r.row_ids;
    };
    const auto exact = rows(0);
    const auto loose = rows(3);
    check(exact.size() < loose.size() && std::includes(loose.begin(), loose.end(), exact.begin(), exact.end()),
          "slop widens the phrase");
    check(lance_dataset_prepare_fts_phrase_query(ds, "text", "plum fig", -1, LANCE_FTS_COVERAGE_STRICT) == nullptr &&
              lance_last_error_code() == LANCE_ERR_INVALID_ARGUMENT,
          "negative slop");
    lance_dataset_close(ds);
}

int main(int argc, char** argv) {
    const std::filesystem::path dir =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "nl_lance_c_search";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const auto type : {LANCE_INDEX_IVF_FLAT, LANCE_INDEX_IVF_PQ, LANCE_INDEX_IVF_HNSW_SQ}) {
        const char* kind = type == LANCE_INDEX_IVF_FLAT ? "IVF_FLAT" : (type == LANCE_INDEX_IVF_PQ ? "IVF_PQ" : "IVF_HNSW_SQ");
        const std::string uri = (dir / (std::string(kind) + ".lance")).string();
        check(write(uri, 0, 1000, LANCE_WRITE_CREATE) && write(uri, 1000, 1000, LANCE_WRITE_APPEND),
              "write: " + last_error());
        LanceDataset* ds = lance_dataset_open(uri.c_str(), nullptr, 0);
        LanceVectorIndexParams p{};
        p.index_type = type;
        p.metric = LANCE_METRIC_L2;
        p.num_partitions = 4;
        p.num_sub_vectors = 4;
        p.hnsw_m = 8;
        check(lance_dataset_create_vector_index(ds, "vec", nullptr, &p, false) == 0, "create vector index: " + last_error());
        check(lance_dataset_index_count(ds) == 1, "the index is listed");
        check(lance_dataset_create_scalar_index(ds, "text", "text_fts", LANCE_SCALAR_INVERTED,
                                                R"({"base_tokenizer":"simple","language":"English"})", false) == 0,
              "create INVERTED: " + last_error());
        lance_dataset_close(ds);
        test_nearest(uri, kind);
        if (type == LANCE_INDEX_IVF_FLAT) {
            test_fts(uri);
            test_errors(uri);
            test_coverage(uri);
        }
    }
    test_segments(dir);
    test_phrase(dir);
    test_include_deleted_rows((dir / "deleted.lance").string());
    test_update_if((dir / "upsert.lance").string());
    if (std::getenv("NANOLANCE_KEEP_TEST_DATA") == nullptr) {
        std::filesystem::remove_all(dir);
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("lance-c search: ok");
    return 0;
}
