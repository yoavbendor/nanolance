// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// lance-c's C API (compat/lance-c/include/lance/lance.h, from the lance-format/lance-c project,
// Apache-2.0, The Lance Authors) implemented over nanolance, so a program written against lance-c
// builds and links against nanolance unchanged.
//
// Every function the header declares is defined here. What nanolance implements does the work; what
// it does not (indexes, vector and full-text search, blob files, and until the shared filter core
// lands, filters and dataset changes other than writes) fails with LANCE_ERR_NOT_SUPPORTED and a
// message naming it -- never a silently different result. docs/LANCE_C_COMPAT.md lists which is
// which; lance-c's own C and C++ tests run against this in CI (tools/lance_c_suite.py).

#include <nanoarrow/nanoarrow.h>  // defines the Arrow C structs lance.h would otherwise declare

#include "lance/lance.h"

#include "nanolance/arrow_slice.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/dataset.hpp"
#include "nanolance/dataset_ops.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/fts_search.hpp"
#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/index_optimize.hpp"
#include "nanolance/index_segments.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/path_safety.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/vector_search.hpp"
#include "nanolance/work_stats.hpp"

#include "index_build.hpp"
#include "index_files.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

struct LanceSession {
    uint64_t index_cache_size_bytes = 0;
    uint64_t metadata_cache_size_bytes = 0;
};

struct LanceDataset {
    std::filesystem::path path;
    uint64_t version = 0;
    nano_lance::DatasetInfo info;
};

struct LanceVersions {
    std::vector<nano_lance::DatasetVersionInfo> versions;
};

struct LanceDataStatistics {
    std::vector<std::pair<uint32_t, uint64_t>> fields;  // (field id, bytes on disk), by id
};

struct LanceBatch;

/// A prepared full-text query: the query, on the snapshot it was prepared on.
struct FtsContextData {
    const LanceDataset* origin = nullptr;  // the snapshot's handle: a scanner must come from it
    std::filesystem::path path;
    uint64_t version = 0;
    nano_lance::FtsQuery query;
    bool index_only = false;  // LANCE_FTS_COVERAGE_INDEX_ONLY: the indexed rows alone
};

/// What a scanner searches for, besides the rows it reads: a k-NN query or a full-text one.
struct SearchSettings {
    bool nearest = false;
    std::string column;
    std::vector<float> key;
    uint32_t k = 0;
    std::optional<uint32_t> minimum_nprobes;
    std::optional<uint32_t> maximum_nprobes;
    std::optional<uint32_t> refine_factor;
    std::optional<uint32_t> ef;
    int32_t query_parallelism = 0;
    std::vector<std::array<uint8_t, 16>> index_segments;  // nearest: only these segments
    std::vector<std::array<uint8_t, 16>> fts_index_segments;  // a prepared FTS query: only these
    std::optional<std::array<uint8_t, 16>> scalar_segment;      // a plain scan scoped by one segment
    std::optional<nano_lance::VectorMetric> metric;
    bool use_index = true;
    bool prefilter = false;
    std::optional<nano_lance::FtsQuery> fts;
    std::shared_ptr<const FtsContextData> fts_context;
    bool use_scalar_index = true;
    bool include_deleted_rows = false;
};

struct LanceScanner {
    const LanceDataset* origin = nullptr;
    std::filesystem::path path;
    uint64_t version = 0;
    bool has_columns = false;
    std::vector<std::string> columns;
    std::string filter;  // SQL; empty: none
    int64_t limit = -1;
    int64_t offset = 0;
    int64_t batch_size = 0;
    bool with_row_id = false;
    bool with_row_address = false;
    bool has_fragments = false;
    std::vector<uint64_t> fragment_ids;
    int32_t blob_handling = LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS;
    SearchSettings search;
    std::atomic<bool> started{false};
    LanceScanStatisticsCallback stats_callback = nullptr;
    void* stats_ctx = nullptr;
    // lance_scanner_next / poll_next: the scanner's own stream.
    std::optional<ArrowArrayStream> own_stream;
    ArrowSchema own_schema{};

    ~LanceScanner() {
        if (own_stream && own_stream->release != nullptr) {
            own_stream->release(&*own_stream);
        }
        if (own_schema.release != nullptr) {
            own_schema.release(&own_schema);
        }
    }
};

/// One value of a Blob v2 column: where its bytes are, and the read cursor. Owns its location, so it
/// outlives the dataset handle.
struct LanceBlobFile {
    nano_lance::BlobV2Location location;
    uint64_t cursor = 0;
};
struct LanceIndexSegmentBuilder {
    std::filesystem::path path;
    uint64_t version = 0;               // the snapshot the segment is built on
    nano_lance::pb::Manifest manifest;  // that snapshot
    std::string column;
    std::string name;  // empty: Lance's default
    std::optional<std::vector<uint64_t>> fragments;
    std::optional<std::array<uint8_t, 16>> uuid;
    int32_t mode = LANCE_INDEX_SEGMENT_BUILD_AUTO;
    bool is_vector = false;
    int32_t scalar_type = 0;
    nano_lance::InvertedIndexOptions inverted;
    nano_lance::VectorIndexOptions vector;
    std::optional<nano_lance::index_build::VectorModel> model;
    LanceIndexBuildProgressCallback callback = nullptr;
    void* callback_ctx = nullptr;
    bool executed = false;
};
struct LanceIndexSegmentMetadata {
    nano_lance::pb::IndexMetadata index;
    std::vector<uint32_t> fragment_ids;
};
struct LanceFtsQueryContext {
    std::shared_ptr<const FtsContextData> data;
};

namespace {

// ── errors ──────────────────────────────────────────────────────────────────────────────────────

thread_local LanceErrorCode t_code = LANCE_OK;
thread_local std::string t_message;

void clear_error() {
    t_code = LANCE_OK;
    t_message.clear();
}

void set_error(LanceErrorCode code, std::string message) {
    t_code = code;
    t_message = std::move(message);
}

/// The lance-c code for a nanolance error message.
LanceErrorCode code_for(const std::string& error) {
    auto has = [&](const char* s) { return error.find(s) != std::string::npos; };
    if (has("commit conflict")) {
        return LANCE_ERR_COMMIT_CONFLICT;
    }
    if (has("already exists")) {
        return LANCE_ERR_DATASET_ALREADY_EXISTS;
    }
    // A bad expression, or a column it (or an operation) names that is not there.
    if (has("with_index_segments") || has("CreateIndex:") || has("is not present in the attached query context")) {
        return LANCE_ERR_INVALID_ARGUMENT;
    }
    if (has("invalid filter") || has("filter column") || has("column '") || has("cannot compare") ||
        has("cannot drop every column") || has("merge insert")) {
        return LANCE_ERR_INVALID_ARGUMENT;
    }
    if (has("not found") || has("No such file") || has("no manifest")) {
        return LANCE_ERR_NOT_FOUND;
    }
    if (has("not supported") || has("unsupported")) {
        return LANCE_ERR_NOT_SUPPORTED;
    }
    if (has("past the end") || has("out of range") || has("must name") || has("must not") ||
        has("not in the dataset")) {
        return LANCE_ERR_INVALID_ARGUMENT;
    }
    if (has("failed to open") || has("failed to read") || has("failed to write") || has("I/O")) {
        return LANCE_ERR_IO;
    }
    return LANCE_ERR_INTERNAL;
}

void fail(const std::string& error) {
    set_error(code_for(error), error);
}

void not_supported(const char* what) {
    set_error(LANCE_ERR_NOT_SUPPORTED, std::string("nanolance does not support ") + what);
}

bool invalid(const char* what) {
    set_error(LANCE_ERR_INVALID_ARGUMENT, what);
    return false;
}

bool invalid(const std::string& what) { return invalid(what.c_str()); }

/// Run `f`, turning an escaping C++ exception into LANCE_ERR_INTERNAL (the counterpart of
/// lance-c's panic guard: nothing unwinds into the caller).
template <typename R, typename F>
R guarded(R on_error, F&& f) {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        set_error(LANCE_ERR_INTERNAL, "out of memory");
    } catch (const std::exception& e) {
        set_error(LANCE_ERR_INTERNAL, e.what());
    } catch (...) {
        set_error(LANCE_ERR_INTERNAL, "unknown error");
    }
    return on_error;
}

// ── URIs ────────────────────────────────────────────────────────────────────────────────────────

std::filesystem::path memory_root() {
    static const std::filesystem::path root = [] {
        auto dir = std::filesystem::temp_directory_path() / ("nanolance-lance-c-memory-" + std::to_string(getpid()));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }();
    return root;
}

/// A dataset URI as a local path: file://, memory:// (a directory private to the process) and plain
/// paths. Object stores are not supported.
bool resolve_uri(const char* uri, std::filesystem::path& out) {
    if (uri == nullptr || uri[0] == '\0') {
        return invalid("uri must not be NULL or empty");
    }
    std::string text(uri);
    if (text.rfind("file://", 0) == 0) {
        out = text.substr(7);
        return true;
    }
    if (text.rfind("memory://", 0) == 0) {
        auto rest = text.substr(9);
        while (!rest.empty() && rest.front() == '/') {
            rest.erase(rest.begin());
        }
        out = memory_root() / (rest.empty() ? std::string("_root") : rest);
        return true;
    }
    if (text.find("://") != std::string::npos) {
        set_error(LANCE_ERR_NOT_SUPPORTED, "nanolance reads local datasets only, not " + text);
        return false;
    }
    out = text;
    return true;
}

bool load_info(const std::filesystem::path& path, uint64_t version, nano_lance::DatasetInfo& info) {
    std::string error;
    if (!nano_lance::dataset_info(path, version != 0U, version, info, error)) {
        set_error(error.find("not found") != std::string::npos ? LANCE_ERR_NOT_FOUND : code_for(error), error);
        return false;
    }
    return true;
}

LanceDataset* open_dataset(const char* uri, uint64_t version) {
    std::filesystem::path path;
    if (!resolve_uri(uri, path)) {
        return nullptr;
    }
    auto ds = std::make_unique<LanceDataset>();
    ds->path = path;
    if (!load_info(path, version, ds->info)) {
        return nullptr;
    }
    ds->version = ds->info.version.version;
    clear_error();
    return ds.release();
}

std::vector<std::string> column_list(const char* const* columns) {
    std::vector<std::string> out;
    for (auto* c = columns; c != nullptr && *c != nullptr; ++c) {
        out.emplace_back(*c);
    }
    return out;
}

using nano_lance::SharedBatch;

ArrowArray view_of(const std::shared_ptr<SharedBatch>& base, int64_t offset, int64_t length,
                   const std::vector<int64_t>& order) {
    return nano_lance::slice_batch(base, offset, length, order);
}

/// `schema` with its top-level children in `order` (a deep copy).
bool reordered_schema(const ArrowSchema& schema, const std::vector<int64_t>& order, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, static_cast<int64_t>(order.size())) != NANOARROW_OK ||
        ArrowSchemaSetMetadata(&out, schema.metadata) != NANOARROW_OK) {
        out.release(&out);
        return false;
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        out.children[i]->release(out.children[i]);
        if (ArrowSchemaDeepCopy(schema.children[order[i]], out.children[i]) != NANOARROW_OK) {
            out.release(&out);
            return false;
        }
    }
    return true;
}

/// Where each requested column is in the decoded schema. `names` empty means all, as decoded.
bool column_order(const ArrowSchema& schema, const std::vector<std::string>& names, std::vector<int64_t>& order,
                  std::string& error) {
    order.clear();
    if (names.empty()) {
        for (int64_t i = 0; i < schema.n_children; ++i) {
            order.push_back(i);
        }
        return true;
    }
    for (const auto& name : names) {
        int64_t found = -1;
        for (int64_t i = 0; i < schema.n_children; ++i) {
            if (schema.children[i]->name != nullptr && name == schema.children[i]->name) {
                found = i;
            }
        }
        if (found < 0) {
            error = "column '" + name + "' not found";
            return false;
        }
        order.push_back(found);
    }
    // The row id columns asked for with with_row_id / with_row_address come after the named ones.
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const char* n = schema.children[i]->name;
        if (n != nullptr && (std::strcmp(n, "_rowid") == 0 || std::strcmp(n, "_rowaddr") == 0) &&
            std::find(order.begin(), order.end(), i) == order.end()) {
            order.push_back(i);
        }
    }
    return true;
}

// ── the scan stream ─────────────────────────────────────────────────────────────────────────────

struct ScanStreamPrivate {
    nano_lance::LanceTableStream stream;
    ArrowSchema decoded_schema{};
    ArrowSchema schema{};
    std::vector<int64_t> order;
    int64_t batch_size = 0;
    // Slices of the current decoded batch not yet handed out.
    std::shared_ptr<SharedBatch> current;
    int64_t current_at = 0;
    // Statistics, reported once at EOF.
    LanceScanStatisticsCallback callback = nullptr;
    void* callback_ctx = nullptr;
    uint64_t bytes_at_open = 0;
    uint64_t reads_at_open = 0;
    bool reported = false;
    bool done = false;
    std::string last_error;
    // include_deleted_rows: the live row ids (ascending); `_rowid` is NULL for every other row.
    std::vector<uint64_t> live;
    int64_t rowid_column = -1;  // in the decoded schema
    bool mark_deleted = false;

    ~ScanStreamPrivate() {
        if (decoded_schema.release != nullptr) {
            decoded_schema.release(&decoded_schema);
        }
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }
};

int scan_get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
    auto* self = static_cast<ScanStreamPrivate*>(stream->private_data);
    return ArrowSchemaDeepCopy(&self->schema, out) == NANOARROW_OK ? 0 : ENOMEM;
}

void report_statistics(ScanStreamPrivate* self) {
    if (self->reported || self->callback == nullptr) {
        return;
    }
    self->reported = true;
    auto& c = nano_lance::work_stats::counters();
    const auto bytes = c.data_bytes_read.load(std::memory_order_relaxed) - self->bytes_at_open;
    const auto reads = c.data_reads.load(std::memory_order_relaxed) - self->reads_at_open;
    LanceScanStatistics stats{};
    stats.iops = reads;
    stats.requests = reads;
    stats.bytes_read = bytes;
    stats.metrics = nullptr;
    stats.metrics_len = 0;
    self->callback(self->callback_ctx, &stats);
}

/// The batch's `_rowid` column with NULL for the rows that are deleted (lance-c's
/// include_deleted_rows: deleted rows still in storage come back, without a row id).
bool null_deleted_row_ids(const ScanStreamPrivate& self, ArrowArray& batch) {
    ArrowArray* ids = batch.children[self.rowid_column];
    const auto* values = static_cast<const uint64_t*>(ids->buffers[1]) + ids->offset;
    const int64_t n = ids->length;
    ArrowArray rebuilt{};
    if (ArrowArrayInitFromType(&rebuilt, NANOARROW_TYPE_UINT64) != NANOARROW_OK ||
        ArrowArrayStartAppending(&rebuilt) != NANOARROW_OK) {
        return false;
    }
    for (int64_t i = 0; i < n; ++i) {  // the child's own rows (the parent's offset applies to them as before)
        const uint64_t id = values[i];
        const bool live = std::binary_search(self.live.begin(), self.live.end(), id);
        if ((live ? ArrowArrayAppendUInt(&rebuilt, id) : ArrowArrayAppendNull(&rebuilt, 1)) != NANOARROW_OK) {
            rebuilt.release(&rebuilt);
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&rebuilt, nullptr) != NANOARROW_OK) {
        rebuilt.release(&rebuilt);
        return false;
    }
    ids->release(ids);
    ArrowArrayMove(&rebuilt, ids);
    return true;
}

int scan_get_next(ArrowArrayStream* stream, ArrowArray* out) {
    auto* self = static_cast<ScanStreamPrivate*>(stream->private_data);
    std::memset(out, 0, sizeof(*out));
    for (;;) {
        if (self->current) {
            const int64_t length = self->current->array.length;
            if (self->current_at < length) {
                const int64_t take = self->batch_size > 0 ? std::min(self->batch_size, length - self->current_at)
                                                          : length - self->current_at;
                *out = view_of(self->current, self->current_at, take, self->order);
                self->current_at += take;
                return 0;
            }
            self->current.reset();
        }
        if (self->done) {
            report_statistics(self);
            return 0;  // end of stream: out->release stays null
        }
        auto batch = std::make_shared<SharedBatch>();
        std::string error;
        if (!self->stream.next(batch->array, error)) {
            self->last_error = error;
            return EIO;
        }
        if (batch->array.release == nullptr) {
            self->done = true;
            continue;
        }
        if (batch->array.length == 0) {
            continue;
        }
        if (self->mark_deleted && !null_deleted_row_ids(*self, batch->array)) {
            self->last_error = "out of memory";
            return ENOMEM;
        }
        self->current = std::move(batch);
        self->current_at = 0;
    }
}

const char* scan_get_last_error(ArrowArrayStream* stream) {
    auto* self = static_cast<ScanStreamPrivate*>(stream->private_data);
    return self->last_error.empty() ? nullptr : self->last_error.c_str();
}

void scan_release(ArrowArrayStream* stream) {
    delete static_cast<ScanStreamPrivate*>(stream->private_data);
    stream->release = nullptr;
}

/// The row id columns a projection names, as flags, and the other names.
void split_system_columns(std::vector<std::string>& names, bool& row_id, bool& row_address) {
    std::vector<std::string> rest;
    for (auto& n : names) {
        if (n == "_rowid") {
            row_id = true;
        } else if (n == "_rowaddr") {
            row_address = true;
        } else {
            rest.push_back(std::move(n));
        }
    }
    names = std::move(rest);
}

bool take_stream(ArrowSchema& decoded_schema, std::vector<ArrowArray>& batches, const std::vector<uint64_t>& sorted,
                 const std::vector<uint64_t>& wanted, const std::vector<std::string>& names, ArrowArrayStream& out);

float half_to_float(uint16_t h) {
    const uint32_t sign = (h & 0x8000U) << 16U;
    const uint32_t exp = (h >> 10U) & 0x1FU;
    const uint32_t mant = h & 0x3FFU;
    uint32_t bits = 0;
    if (exp == 0U) {
        if (mant == 0U) {
            bits = sign;
        } else {  // subnormal: normalize
            uint32_t e = 127U - 15U + 1U;
            uint32_t m = mant;
            while ((m & 0x400U) == 0U) {
                m <<= 1U;
                --e;
            }
            bits = sign | (e << 23U) | ((m & 0x3FFU) << 13U);
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13U);
    } else {
        bits = sign | ((exp + 127U - 15U) << 23U) | (mant << 13U);
    }
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// A full-text query context for `column` of `dataset`'s snapshot, as lance-c prepares one: the
/// column must have an INVERTED index; STRICT coverage requires it to cover every fragment, and
/// INDEX_ONLY searches (and scores) the rows it covers alone.
LanceFtsQueryContext* prepare_fts(const LanceDataset* dataset, const char* column, const char* query,
                                  int32_t coverage_mode, nano_lance::FtsQuery q, bool phrase) {
    if (dataset == nullptr || column == nullptr || column[0] == '\0' || query == nullptr) {
        invalid("dataset, column and query must not be NULL or empty");
        return nullptr;
    }
    if (coverage_mode != LANCE_FTS_COVERAGE_STRICT && coverage_mode != LANCE_FTS_COVERAGE_INDEX_ONLY) {
        invalid("invalid coverage_mode " + std::to_string(coverage_mode) + "; expected 0 (STRICT) or 1 (INDEX_ONLY)");
        return nullptr;
    }
    std::vector<nano_lance::IndexInfo> indices;
    std::string error;
    if (!nano_lance::dataset_list_indices(dataset->path, true, dataset->version, indices, error)) {
        fail(error);
        return nullptr;
    }
    std::set<uint64_t> covered;
    std::string uuid;
    std::string name;  // the column's first INVERTED index: its segments
    bool found = false;
    for (const auto& index : indices) {
        if (index.type == "Inverted" && index.fields.size() == 1U && index.fields[0] == column) {
            if (found && index.name != name) {
                continue;
            }
            found = true;
            name = index.name;
            uuid = index.uuid;
            covered.insert(index.fragment_ids.begin(), index.fragment_ids.end());
        }
    }
    if (!found) {
        invalid("no committed FTS index exists for column '" + std::string(column) + "' in dataset version " +
                std::to_string(dataset->version));
        return nullptr;
    }
    std::vector<uint64_t> unindexed;
    for (const auto& f : dataset->info.fragments) {
        if (covered.count(f.id) == 0U) {
            unindexed.push_back(f.id);
        }
    }
    if (coverage_mode == LANCE_FTS_COVERAGE_STRICT && !unindexed.empty()) {
        std::string ids;
        for (const auto id : unindexed) {
            ids += (ids.empty() ? "" : ", ") + std::to_string(id);
        }
        invalid("coverage_mode=STRICT requires every fragment in dataset version " + std::to_string(dataset->version) +
                " to be indexed; column '" + column + "' has " + std::to_string(unindexed.size()) +
                " unindexed fragments: [" + ids + "]");
        return nullptr;
    }
    if (phrase) {
        // Phrase queries need positions.
        nano_lance::fts::AnalyzerParams params;
        if (nano_lance::index_build::load_inverted_params(dataset->path / "_indices" / uuid, params, error) &&
            !params.with_position) {
            invalid("FTS index for column '" + std::string(column) +
                    "' does not store token positions required by Phrase queries; recreate the index with positions "
                    "enabled");
            return nullptr;
        }
    }
    auto data = std::make_shared<FtsContextData>();
    data->origin = dataset;
    data->path = dataset->path;
    data->version = dataset->version;
    q.text = query;
    q.columns = {column};
    data->query = std::move(q);
    data->index_only = coverage_mode == LANCE_FTS_COVERAGE_INDEX_ONLY;
    auto* context = new LanceFtsQueryContext;
    context->data = std::move(data);
    clear_error();
    return context;
}

// ── nearest / full-text search: the rows found, in order, with their distance or score ──────────

/// A struct batch with one more child, owning the batch it extends.
struct ExtendedBatch {
    ArrowArray base{};
    ArrowArray extra{};
    std::vector<ArrowArray*> children;
};

void release_extended(ArrowArray* array) {
    auto* self = static_cast<ExtendedBatch*>(array->private_data);
    if (self->extra.release != nullptr) {
        self->extra.release(&self->extra);
    }
    if (self->base.release != nullptr) {
        self->base.release(&self->base);
    }
    delete self;
    array->release = nullptr;
}

/// `batch` with a float32 child of `values` (one a row, past the batch's offset) added last.
bool extend_batch(ArrowArray& batch, const float* values, ArrowArray& out) {
    auto self = std::make_unique<ExtendedBatch>();
    if (ArrowArrayInitFromType(&self->extra, NANOARROW_TYPE_FLOAT) != NANOARROW_OK ||
        ArrowArrayStartAppending(&self->extra) != NANOARROW_OK) {
        return false;
    }
    for (int64_t i = 0; i < batch.offset; ++i) {
        ArrowArrayAppendDouble(&self->extra, 0.0);
    }
    for (int64_t i = 0; i < batch.length; ++i) {
        if (ArrowArrayAppendDouble(&self->extra, static_cast<double>(values[i])) != NANOARROW_OK) {
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&self->extra, nullptr) != NANOARROW_OK) {
        return false;
    }
    ArrowArrayMove(&batch, &self->base);
    self->children.assign(self->base.children, self->base.children + self->base.n_children);
    self->children.push_back(&self->extra);
    out = self->base;
    out.n_children = self->base.n_children + 1;
    out.children = self->children.data();
    out.release = &release_extended;
    out.private_data = self.release();
    return true;
}

/// `schema` with a nullable float32 child `name` added last (a deep copy).
bool extend_schema(const ArrowSchema& schema, const char* name, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, schema.n_children + 1) != NANOARROW_OK ||
        ArrowSchemaSetMetadata(&out, schema.metadata) != NANOARROW_OK) {
        out.release(&out);
        return false;
    }
    for (int64_t i = 0; i < schema.n_children; ++i) {
        out.children[i]->release(out.children[i]);
        if (ArrowSchemaDeepCopy(schema.children[i], out.children[i]) != NANOARROW_OK) {
            out.release(&out);
            return false;
        }
    }
    ArrowSchema* last = out.children[schema.n_children];
    if (ArrowSchemaSetType(last, NANOARROW_TYPE_FLOAT) != NANOARROW_OK || ArrowSchemaSetName(last, name) != NANOARROW_OK) {
        out.release(&out);
        return false;
    }
    last->flags |= ARROW_FLAG_NULLABLE;
    return true;
}

/// The k nearest rows, or the rows matching a full-text query, best first: the columns asked for,
/// then `_distance` / `_score`, then the row id columns -- as Lance returns them.
bool open_search(const LanceScanner& scanner, ArrowArrayStream& out) {
    const SearchSettings& search = scanner.search;
    if (scanner.has_fragments) {
        not_supported("fragment_ids with nearest or full-text search");
        return false;
    }
    std::vector<uint64_t> rows;
    std::vector<float> values;
    std::string error;
    const char* value_name = search.nearest ? "_distance" : "_score";
    if (search.nearest) {
        nano_lance::NearestQuery q;
        q.has_version = true;
        q.version = scanner.version;
        q.column = search.column;
        q.key = search.key;
        q.k = search.k;
        q.minimum_nprobes = search.minimum_nprobes.value_or(1U);
        q.maximum_nprobes = search.maximum_nprobes;
        q.refine_factor = search.refine_factor;
        q.ef = search.ef;
        q.query_parallelism = search.query_parallelism;
        q.segments = search.index_segments;
        q.metric = search.metric;
        q.use_index = search.use_index;
        if (!scanner.filter.empty()) {
            q.filter = scanner.filter;
        }
        q.prefilter = search.prefilter;
        nano_lance::NearestResult result;
        if (!nano_lance::dataset_nearest(scanner.path, q, result, error)) {
            fail(error);
            return false;
        }
        rows = std::move(result.row_ids);
        values = std::move(result.distances);
    } else {
        nano_lance::FtsSearchRequest r;
        r.has_version = true;
        r.version = scanner.version;
        if (search.fts_context != nullptr) {
            r.query = search.fts_context->query;
            r.fast_search = search.fts_context->index_only;
            r.segments = search.fts_index_segments;
        } else {
            r.query = *search.fts;
        }
        if (scanner.limit >= 0) {
            r.limit = static_cast<uint64_t>(scanner.limit + std::max<int64_t>(scanner.offset, 0));
        }
        if (!scanner.filter.empty()) {
            r.filter = scanner.filter;
        }
        r.prefilter = search.prefilter;
        nano_lance::FtsSearchResult result;
        if (!nano_lance::dataset_full_text_search(scanner.path, r, result, error)) {
            fail(error);
            return false;
        }
        rows = std::move(result.row_ids);
        values = std::move(result.scores);
    }
    // The scanner's offset and limit apply to the rows found.
    const auto first = std::min<std::size_t>(static_cast<std::size_t>(std::max<int64_t>(scanner.offset, 0)), rows.size());
    std::size_t last = rows.size();
    if (scanner.limit >= 0) {
        last = std::min(last, first + static_cast<std::size_t>(scanner.limit));
    }
    rows.assign(rows.begin() + static_cast<std::ptrdiff_t>(first), rows.begin() + static_cast<std::ptrdiff_t>(last));
    values.assign(values.begin() + static_cast<std::ptrdiff_t>(first), values.begin() + static_cast<std::ptrdiff_t>(last));

    std::vector<std::string> names = scanner.columns;
    bool row_id = scanner.with_row_id;
    bool row_address = scanner.with_row_address;
    split_system_columns(names, row_id, row_address);
    std::erase(names, std::string(value_name));
    nano_lance::LanceScanRequest request;
    request.has_version = true;
    request.version = scanner.version;
    request.columns = scanner.has_columns ? &names : nullptr;
    request.with_row_id = row_id;
    request.with_row_address = row_address;
    request.blob_handling = scanner.blob_handling == LANCE_BLOB_HANDLING_ALL_BINARY
                                ? nano_lance::BlobHandling::Binary
                                : nano_lance::BlobHandling::Descriptions;
    std::vector<uint64_t> sorted(rows);
    std::sort(sorted.begin(), sorted.end());
    std::vector<float> sorted_values(sorted.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto at = std::lower_bound(sorted.begin(), sorted.end(), rows[i]) - sorted.begin();
        sorted_values[static_cast<std::size_t>(at)] = values[i];
    }
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!nano_lance::lance_dataset_take_rows(scanner.path, request, sorted, schema, batches, error)) {
        fail(error);
        return false;
    }
    const auto release_batches = [&] {
        for (auto& b : batches) {
            if (b.release != nullptr) {
                b.release(&b);
            }
        }
    };
    // Each row's value, added as the batch's last column.
    ArrowSchema extended{};
    if (!extend_schema(schema, value_name, extended)) {
        schema.release(&schema);
        release_batches();
        fail("out of memory");
        return false;
    }
    std::vector<std::string> wanted;  // the data columns (in the order asked for), then the value
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const char* n = schema.children[i]->name;
        if (n != nullptr && std::strcmp(n, "_rowid") != 0 && std::strcmp(n, "_rowaddr") != 0) {
            wanted.emplace_back(n);
        }
    }
    schema.release(&schema);
    if (scanner.has_columns) {
        wanted = names;
    }
    wanted.emplace_back(value_name);
    std::size_t at = 0;
    for (auto& b : batches) {
        ArrowArray e{};
        if (!extend_batch(b, sorted_values.data() + at, e)) {
            extended.release(&extended);
            release_batches();
            fail("out of memory");
            return false;
        }
        at += static_cast<std::size_t>(e.length);
        b = e;
    }
    return take_stream(extended, batches, sorted, rows, wanted, out);
}

bool open_scan(const LanceScanner& scanner, ArrowArrayStream& out) {
    if (!scanner.search.index_segments.empty() && !scanner.search.nearest) {
        return invalid("index_segments requires nearest() to be configured");
    }
    if (!scanner.search.fts_index_segments.empty() && scanner.search.fts_context == nullptr) {
        return invalid("fts_index_segments requires an FTS query context");
    }
    bool use_scalar_index = scanner.search.use_scalar_index;
    if (scanner.search.scalar_segment) {
        // A worker's scoped scan (lance-c scalar_segment.rs): the rows of its fragments that pass the
        // whole filter. The segment only speeds that up in Lance, so it is validated and the rows
        // read as an ordinary filtered scan of those fragments, with no global scalar index.
        const auto& search = scanner.search;
        if (search.nearest || search.fts || search.fts_context != nullptr || !search.index_segments.empty() ||
            !search.fts_index_segments.empty() || search.include_deleted_rows) {
            return invalid("scalar_index_segment requires an ordinary scan of live rows; vector/FTS queries and "
                           "include_deleted_rows=true are unsupported");
        }
        if (!scanner.has_fragments || scanner.fragment_ids.empty()) {
            return invalid("scalar_index_segment requires explicit nonempty fragment_ids for its read and fallback "
                           "domain");
        }
        nano_lance::pb::Manifest manifest;
        std::string error;
        if (!nano_lance::load_manifest_version(scanner.path, scanner.version, manifest, error)) {
            fail(error);
            return false;
        }
        std::set<uint64_t> visible;
        for (const auto& f : manifest.fragments) {
            visible.insert(f.id);
        }
        if (std::any_of(scanner.fragment_ids.begin(), scanner.fragment_ids.end(),
                        [&](uint64_t id) { return visible.count(id) == 0U; })) {
            return invalid("scalar segment fragment_ids contains a fragment absent from the dataset snapshot");
        }
        const auto index = std::find_if(manifest.indices.begin(), manifest.indices.end(),
                                        [&](const auto& i) { return i.uuid == *search.scalar_segment; });
        if (index == manifest.indices.end()) {
            return invalid("scalar index segment " + nano_lance::pb::uuid_string(*search.scalar_segment) +
                           " is absent from the dataset snapshot");
        }
        if (index->fields.size() != 1U) {
            return invalid("scalar segment must index a single key field");
        }
        if (std::none_of(manifest.fields.begin(), manifest.fields.end(),
                         [&](const auto& f) { return f.id == index->fields.front(); })) {
            return invalid("scalar segment key field is absent from the dataset schema");
        }
        use_scalar_index = false;
    }
    if (scanner.search.nearest || scanner.search.fts || scanner.search.fts_context != nullptr) {
        return open_search(scanner, out);
    }
    auto self = std::make_unique<ScanStreamPrivate>();
    std::vector<std::string> names = scanner.columns;
    bool row_id = scanner.with_row_id;
    bool row_address = scanner.with_row_address;
    split_system_columns(names, row_id, row_address);
    nano_lance::LanceScanRequest request;
    request.has_version = true;
    request.version = scanner.version;
    request.columns = scanner.has_columns ? &names : nullptr;
    request.fragment_ids = scanner.has_fragments ? &scanner.fragment_ids : nullptr;
    request.range.offset = static_cast<uint64_t>(std::max<int64_t>(scanner.offset, 0));
    request.range.length = scanner.limit < 0 ? nano_lance::LanceRowRange::kAllRows
                                             : static_cast<uint64_t>(scanner.limit);
    request.with_row_id = row_id;
    request.with_row_address = row_address;
    request.filter = scanner.filter.empty() ? nullptr : &scanner.filter;
    request.use_scalar_index = use_scalar_index;
    request.include_deleted_rows = scanner.search.include_deleted_rows;
    // lance-c returns a blob column as its description unless asked for the bytes.
    request.blob_handling = scanner.blob_handling == LANCE_BLOB_HANDLING_ALL_BINARY
                                ? nano_lance::BlobHandling::Binary
                                : nano_lance::BlobHandling::Descriptions;
    auto& c = nano_lance::work_stats::counters();
    self->bytes_at_open = c.data_bytes_read.load(std::memory_order_relaxed);
    self->reads_at_open = c.data_reads.load(std::memory_order_relaxed);
    std::string error;
    // An offset past the end reads nothing, as in Lance (nanolance's reader calls it an error). With
    // a filter the range counts the rows that pass, and the reader handles it.
    nano_lance::DatasetInfo info;
    if (!load_info(scanner.path, scanner.version, info)) {
        return false;
    }
    uint64_t rows = 0;
    for (const auto& f : info.fragments) {
        if (!scanner.has_fragments ||
            std::find(scanner.fragment_ids.begin(), scanner.fragment_ids.end(), f.id) != scanner.fragment_ids.end()) {
            rows += f.rows();
        }
    }
    if (scanner.filter.empty()) {
        request.range.offset = std::min<uint64_t>(request.range.offset, rows);
    }
    if (scanner.search.include_deleted_rows) {
        if (!row_id) {
            invalid("include_deleted_rows requires with_row_id=true");
            return false;
        }
        // The live row ids, from a read of no column: the others are deleted.
        nano_lance::LanceScanRequest live = request;
        const std::vector<std::string> none;
        live.columns = &none;
        live.filter = nullptr;
        live.include_deleted_rows = false;
        live.with_row_address = false;
        live.range = nano_lance::LanceRowRange{};
        ArrowSchema live_schema{};
        std::vector<ArrowArray> live_batches;
        if (!nano_lance::lance_dataset_scan(scanner.path, live, live_schema, live_batches, error)) {
            fail(error);
            return false;
        }
        for (auto& b : live_batches) {
            const ArrowArray* ids = b.children[b.n_children - 1];
            const auto* v = static_cast<const uint64_t*>(ids->buffers[1]) + ids->offset;
            self->live.insert(self->live.end(), v, v + ids->length);
            b.release(&b);
        }
        live_schema.release(&live_schema);
        std::sort(self->live.begin(), self->live.end());
        self->mark_deleted = true;
    }
    if (!nano_lance::LanceTableStream::open_request(scanner.path, request, self->decoded_schema, self->stream, error)) {
        fail(error);
        return false;
    }
    if (self->mark_deleted) {
        for (int64_t i = 0; i < self->decoded_schema.n_children; ++i) {
            const char* n = self->decoded_schema.children[i]->name;
            if (n != nullptr && std::strcmp(n, "_rowid") == 0) {
                self->rowid_column = i;
                self->decoded_schema.children[i]->flags |= ARROW_FLAG_NULLABLE;
            }
        }
        if (self->rowid_column < 0) {
            fail("include_deleted_rows: the scan has no _rowid column");
            return false;
        }
    }
    std::vector<std::string> wanted = names;
    if (scanner.has_columns) {
        // The order the caller named them in, system columns included.
        wanted = scanner.columns;
    }
    if (!column_order(self->decoded_schema, scanner.has_columns ? wanted : std::vector<std::string>{}, self->order,
                      error) ||
        !reordered_schema(self->decoded_schema, self->order, self->schema)) {
        fail(error.empty() ? "failed to build the scan schema" : error);
        return false;
    }
    self->batch_size = scanner.batch_size;
    self->callback = scanner.stats_callback;
    self->callback_ctx = scanner.stats_ctx;
    out.get_schema = &scan_get_schema;
    out.get_next = &scan_get_next;
    out.get_last_error = &scan_get_last_error;
    out.release = &scan_release;
    out.private_data = self.release();
    return true;
}

// ── take: a stream over slices of the rows read ─────────────────────────────────────────────────

struct TakeStreamPrivate {
    ArrowSchema schema{};
    std::vector<ArrowArray> views;
    std::size_t next = 0;

    ~TakeStreamPrivate() {
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
        for (std::size_t i = next; i < views.size(); ++i) {
            if (views[i].release != nullptr) {
                views[i].release(&views[i]);
            }
        }
    }
};

int take_get_schema(ArrowArrayStream* stream, ArrowSchema* out) {
    auto* self = static_cast<TakeStreamPrivate*>(stream->private_data);
    return ArrowSchemaDeepCopy(&self->schema, out) == NANOARROW_OK ? 0 : ENOMEM;
}

int take_get_next(ArrowArrayStream* stream, ArrowArray* out) {
    auto* self = static_cast<TakeStreamPrivate*>(stream->private_data);
    std::memset(out, 0, sizeof(*out));
    if (self->next < self->views.size()) {
        *out = self->views[self->next++];
    }
    return 0;
}

const char* take_get_last_error(ArrowArrayStream*) { return nullptr; }

void take_release(ArrowArrayStream* stream) {
    delete static_cast<TakeStreamPrivate*>(stream->private_data);
    stream->release = nullptr;
}

/// The rows `wanted` (in order, repeats kept) of a take that decoded `sorted` (ascending, distinct)
/// into `batches`, as a stream of views: one per run of rows adjacent in both.
bool take_stream(ArrowSchema& decoded_schema, std::vector<ArrowArray>& batches, const std::vector<uint64_t>& sorted,
                 const std::vector<uint64_t>& wanted, const std::vector<std::string>& names, ArrowArrayStream& out) {
    auto self = std::make_unique<TakeStreamPrivate>();
    std::vector<int64_t> order;
    std::string error;
    std::vector<std::shared_ptr<SharedBatch>> shared;
    for (auto& b : batches) {
        shared.push_back(std::make_shared<SharedBatch>(std::move(b)));
    }
    const bool ok = column_order(decoded_schema, names, order, error) &&
                    reordered_schema(decoded_schema, order, self->schema);
    decoded_schema.release(&decoded_schema);
    if (!ok) {
        fail(error.empty() ? "failed to build the take schema" : error);
        return false;
    }
    // Row k of `sorted` is row `local` of batch `which`.
    std::vector<std::pair<std::size_t, int64_t>> where;
    where.reserve(sorted.size());
    for (std::size_t b = 0; b < shared.size(); ++b) {
        for (int64_t r = 0; r < shared[b]->array.length; ++r) {
            where.emplace_back(b, r);
        }
    }
    if (where.size() != sorted.size()) {
        fail("take returned " + std::to_string(where.size()) + " rows for " + std::to_string(sorted.size()));
        return false;
    }
    std::size_t i = 0;
    while (i < wanted.size()) {
        const auto k = static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), wanted[i]) - sorted.begin());
        const auto [batch, row] = where[k];
        int64_t run = 1;
        while (i + static_cast<std::size_t>(run) < wanted.size()) {
            const auto next = static_cast<std::size_t>(
                std::lower_bound(sorted.begin(), sorted.end(), wanted[i + static_cast<std::size_t>(run)]) - sorted.begin());
            if (next != k + static_cast<std::size_t>(run) || where[next].first != batch) {
                break;
            }
            ++run;
        }
        self->views.push_back(view_of(shared[batch], row, run, order));
        i += static_cast<std::size_t>(run);
    }
    out.get_schema = &take_get_schema;
    out.get_next = &take_get_next;
    out.get_last_error = &take_get_last_error;
    out.release = &take_release;
    out.private_data = self.release();
    return true;
}

template <typename TakeFn>
int32_t take_impl(const LanceDataset* dataset, const uint64_t* rows, size_t count, const char* const* columns,
                  struct ArrowArrayStream* out, TakeFn&& read) {
    if (dataset == nullptr || out == nullptr || (rows == nullptr && count != 0)) {
        invalid("dataset, out and (when num > 0) the rows must not be NULL");
        return -1;
    }
    std::vector<std::string> names = column_list(columns);
    std::vector<std::string> wanted_names = names;
    bool row_id = false;
    bool row_address = false;
    split_system_columns(names, row_id, row_address);
    std::vector<uint64_t> wanted(rows, rows + count);
    std::vector<uint64_t> sorted(wanted);
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    nano_lance::LanceScanRequest request;
    request.has_version = true;
    request.version = dataset->version;
    request.columns = columns != nullptr ? &names : nullptr;
    request.with_row_id = row_id;
    request.with_row_address = row_address;
    request.blob_handling = nano_lance::BlobHandling::Descriptions;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    if (!read(request, sorted, wanted, schema, batches, error)) {
        fail(error);
        return -1;
    }
    if (!take_stream(schema, batches, sorted, wanted, columns != nullptr ? wanted_names : std::vector<std::string>{},
                     *out)) {
        for (auto& b : batches) {
            if (b.release != nullptr) {
                b.release(&b);
            }
        }
        return -1;
    }
    clear_error();
    return 0;
}

// ── writes ──────────────────────────────────────────────────────────────────────────────────────

bool same_type(const ArrowSchema& a, const ArrowSchema& b) {
    if (std::strcmp(a.format != nullptr ? a.format : "", b.format != nullptr ? b.format : "") != 0 ||
        a.n_children != b.n_children) {
        return false;
    }
    for (int64_t i = 0; i < a.n_children; ++i) {
        const char* an = a.children[i]->name != nullptr ? a.children[i]->name : "";
        const char* bn = b.children[i]->name != nullptr ? b.children[i]->name : "";
        if (std::strcmp(an, bn) != 0 || !same_type(*a.children[i], *b.children[i])) {
            return false;
        }
    }
    return true;
}

struct WriteRequest {
    const char* uri = nullptr;
    const ArrowSchema* schema = nullptr;
    ArrowArrayStream* stream = nullptr;
    int32_t mode = LANCE_WRITE_CREATE;
    const LanceWriteParams* params = nullptr;
    bool commit = true;  // false: lance_write_fragments (data files only)
    LanceDataset** out_dataset = nullptr;
};

int32_t write_impl(const WriteRequest& req) {
    struct ReleaseStream {
        ArrowArrayStream* s;
        ~ReleaseStream() {
            if (s != nullptr && s->release != nullptr) {
                s->release(s);
            }
        }
    } consume{req.stream};
    if (req.schema == nullptr || req.stream == nullptr) {
        invalid("schema and stream must not be NULL");
        return -1;
    }
    if (req.mode < LANCE_WRITE_CREATE || req.mode > LANCE_WRITE_OVERWRITE) {
        invalid("mode must be LANCE_WRITE_CREATE, LANCE_WRITE_APPEND or LANCE_WRITE_OVERWRITE");
        return -1;
    }
    std::filesystem::path path;
    if (!resolve_uri(req.uri, path)) {
        return -1;
    }
    uint64_t max_rows = 1024ULL * 1024ULL;
    uint64_t max_bytes = 0;
    if (req.params != nullptr) {
        if (req.params->max_rows_per_file != 0U) {
            max_rows = req.params->max_rows_per_file;
        }
        max_bytes = req.params->max_bytes_per_file;
        if (const char* v = req.params->data_storage_version; v != nullptr) {
            const std::string version(v);
            if (version == "2.0" || version == "2.1" || version == "2.3" || version == "legacy" || version == "0.1") {
                set_error(LANCE_ERR_NOT_SUPPORTED, "nanolance writes data storage version 2.2, not " + version);
                return -1;
            }
            if (version != "2.2" && version != "stable" && version != "next") {
                invalid("unknown data_storage_version");
                return -1;
            }
        }
        if (req.params->enable_stable_row_ids) {
            not_supported("stable row ids");
            return -1;
        }
    }
    ArrowSchema stream_schema{};
    if (req.stream->get_schema(req.stream, &stream_schema) != 0) {
        fail("failed to read the stream's schema");
        return -1;
    }
    struct ReleaseSchema {
        ArrowSchema* s;
        ~ReleaseSchema() {
            if (s->release != nullptr) {
                s->release(s);
            }
        }
    } release_schema{&stream_schema};
    if (!same_type(*req.schema, stream_schema)) {
        invalid("the stream's schema does not match the schema given");
        return -1;
    }
    std::string error;
    const bool exists = nano_lance::highest_manifest_version(path, error) != 0U;
    if (req.commit && req.mode == LANCE_WRITE_CREATE && exists) {
        set_error(LANCE_ERR_DATASET_ALREADY_EXISTS, "Dataset already exists: " + path.string());
        return -1;
    }
    const bool append = req.commit && req.mode == LANCE_WRITE_APPEND && exists;

    NanoLanceWriteOptions options{};
    options.append = append;
    options.stage_fragments = true;
    options.max_pending_bytes = max_bytes;
    NanoLanceWriter writer{};
    if (nano_lance_writer_open(&writer, path.string().c_str(), &options) != NANO_LANCE_OK) {
        fail(writer.last_error);
        nano_lance_writer_close(&writer);
        return -1;
    }
    struct CloseWriter {
        NanoLanceWriter* w;
        ~CloseWriter() { nano_lance_writer_close(w); }
    } close_writer{&writer};
    nano_lance_writer_set_ignore_nullability(&writer, true);
    if (req.commit) {
        // What lance-c records on create: every 20 versions, versions older than 14 days are
        // reclaimed (run_auto_cleanup after each commit, as Lance's auto cleanup hook).
        nano_lance_writer_set_initial_config(&writer, "lance.auto_cleanup.interval", "20");
        nano_lance_writer_set_initial_config(&writer, "lance.auto_cleanup.older_than", "14days");
    }
    uint64_t pending = 0;
    bool wrote = false;
    for (;;) {
        ArrowArray batch{};
        if (req.stream->get_next(req.stream, &batch) != 0) {
            const char* why = req.stream->get_last_error != nullptr ? req.stream->get_last_error(req.stream) : nullptr;
            fail(std::string("reading the input stream failed") + (why != nullptr ? std::string(": ") + why : ""));
            return -1;
        }
        if (batch.release == nullptr) {
            break;
        }
        const auto rows = static_cast<uint64_t>(batch.length);
        const int rc = nano_lance_write_batch(&writer, &batch, &stream_schema);
        if (batch.release != nullptr) {
            batch.release(&batch);
        }
        if (rc != NANO_LANCE_OK) {
            set_error(rc == NANO_LANCE_UNSUPPORTED ? LANCE_ERR_NOT_SUPPORTED : LANCE_ERR_INVALID_ARGUMENT,
                      writer.last_error);
            return -1;
        }
        wrote = true;
        pending += rows;
        if (pending >= max_rows) {
            if (nano_lance_writer_commit(&writer, false) != NANO_LANCE_OK) {
                fail(writer.last_error);
                return -1;
            }
            pending = 0;
        }
    }
    if (!wrote) {
        // No rows: a dataset of the schema alone (an empty batch sets the schema).
        ArrowArray empty{};
        if (ArrowArrayInitFromSchema(&empty, &stream_schema, nullptr) != NANOARROW_OK ||
            ArrowArrayFinishBuildingDefault(&empty, nullptr) != NANOARROW_OK ||
            nano_lance_write_batch(&writer, &empty, &stream_schema) != NANO_LANCE_OK) {
            if (empty.release != nullptr) {
                empty.release(&empty);
            }
            fail(writer.last_error[0] != '\0' ? writer.last_error : "failed to write an empty dataset");
            return -1;
        }
        empty.release(&empty);
    }
    if (!req.commit) {
        if (pending != 0U && nano_lance_writer_commit(&writer, false) != NANO_LANCE_OK) {
            fail(writer.last_error);
            return -1;
        }
        // Data files only: no manifest, and no empty _versions left behind.
        std::error_code ec;
        std::filesystem::remove(path / "_versions", ec);
        clear_error();
        return 0;
    }
    const int mode = append ? NANO_LANCE_COMMIT_APPEND
                            : (req.mode == LANCE_WRITE_CREATE ? NANO_LANCE_COMMIT_CREATE : NANO_LANCE_COMMIT_OVERWRITE);
    uint64_t version = 0;
    if (nano_lance_writer_finish(&writer, mode, &version) != NANO_LANCE_OK) {
        fail(writer.last_error);
        return -1;
    }
    if (req.out_dataset != nullptr) {
        auto* ds = open_dataset(req.uri, version);
        if (ds == nullptr) {
            return -1;
        }
        *req.out_dataset = ds;
    }
    clear_error();
    return 0;
}

// ── data statistics ─────────────────────────────────────────────────────────────────────────────

bool data_statistics(const LanceDataset& ds, LanceDataStatistics& out) {
    nano_lance::pb::Manifest manifest;
    std::string error;
    if (!nano_lance::load_manifest_version(ds.path, ds.version, manifest, error)) {
        fail(error);
        return false;
    }
    std::map<uint32_t, uint64_t> bytes;
    for (const auto& f : manifest.fields) {
        bytes[static_cast<uint32_t>(f.id)] = 0;
    }
    for (const auto& fragment : manifest.fragments) {
        for (const auto& file : fragment.files) {
            const auto jailed = nano_lance::safe_join_under(ds.path / "data", file.path);
            if (!jailed) {
                fail("data file path escapes the dataset directory");
                return false;
            }
            nano_lance::pb::FileDescriptor descriptor;
            nano_lance::LanceDataFileFooterLayout layout{};
            std::vector<nano_lance::pb::ColumnMetadata> columns;
            if (!nano_lance::read_lance_data_file_footer_and_descriptor(*jailed, descriptor, layout, error) ||
                !nano_lance::read_lance_data_file_column_metadatas(*jailed, layout, columns, error)) {
                fail(error);
                return false;
            }
            for (std::size_t i = 0; i < file.fields.size() && i < file.column_indices.size(); ++i) {
                const auto c = file.column_indices[i];
                if (c < 0 || static_cast<std::size_t>(c) >= columns.size()) {
                    continue;
                }
                uint64_t sum = 0;
                for (const auto& page : columns[static_cast<std::size_t>(c)].pages) {
                    for (const auto size : page.buffer_sizes) {
                        sum += size;
                    }
                }
                bytes[static_cast<uint32_t>(file.fields[i])] += sum;
            }
        }
    }
    out.fields.assign(bytes.begin(), bytes.end());
    return true;
}

// ── scanner helpers ─────────────────────────────────────────────────────────────────────────────

bool check_scanner(const LanceScanner* scanner) {
    if (scanner == nullptr) {
        return invalid("scanner must not be NULL");
    }
    return true;
}

/// Setters that must come before the scan starts.
template <typename F>
int32_t before_scan(LanceScanner* scanner, F&& set) {
    if (!check_scanner(scanner)) {
        return -1;
    }
    if (scanner->started.load()) {
        invalid("the scan has already started");
        return -1;
    }
    if (!set()) {
        return -1;
    }
    clear_error();
    return 0;
}

struct LanceBatchImpl {
    std::shared_ptr<SharedBatch> array;
    ArrowSchema schema{};
    ~LanceBatchImpl() {
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }
};

int32_t scanner_next(LanceScanner* scanner, LanceBatch** out) {
    if (!scanner->own_stream) {
        scanner->started = true;
        ArrowArrayStream stream{};
        if (!open_scan(*scanner, stream)) {
            return -1;
        }
        scanner->own_stream = stream;
        if (stream.get_schema(&*scanner->own_stream, &scanner->own_schema) != 0) {
            fail("failed to read the scan schema");
            return -1;
        }
    }
    auto batch = std::make_shared<SharedBatch>();
    if (scanner->own_stream->get_next(&*scanner->own_stream, &batch->array) != 0) {
        const char* why = scanner->own_stream->get_last_error(&*scanner->own_stream);
        fail(why != nullptr ? why : "scan failed");
        return -1;
    }
    if (batch->array.release == nullptr) {
        clear_error();
        return 1;
    }
    auto impl = std::make_unique<LanceBatchImpl>();
    impl->array = std::move(batch);
    if (ArrowSchemaDeepCopy(&scanner->own_schema, &impl->schema) != NANOARROW_OK) {
        fail("failed to copy the batch schema");
        return -1;
    }
    *out = reinterpret_cast<LanceBatch*>(impl.release());
    clear_error();
    return 0;
}

}  // namespace

extern "C" {

// ── errors ──────────────────────────────────────────────────────────────────────────────────────

LanceErrorCode lance_last_error_code(void) { return t_code; }

const char* lance_last_error_message(void) {
    if (t_code == LANCE_OK && t_message.empty()) {
        return nullptr;
    }
    char* copy = static_cast<char*>(std::malloc(t_message.size() + 1U));
    if (copy != nullptr) {
        std::memcpy(copy, t_message.c_str(), t_message.size() + 1U);
    }
    return copy;
}

void lance_free_string(const char* s) { std::free(const_cast<char*>(s)); }

void lance_free_bytes(uint8_t* bytes) { std::free(bytes); }

// ── session ─────────────────────────────────────────────────────────────────────────────────────

LanceSession* lance_session_new(uint64_t index_cache_size_bytes, uint64_t metadata_cache_size_bytes) {
    clear_error();
    return new LanceSession{index_cache_size_bytes, metadata_cache_size_bytes};
}

void lance_session_close(LanceSession* session) { delete session; }

int32_t lance_session_get_cache_stats(const LanceSession* session, LanceSessionCacheStats* out_stats) {
    if (session == nullptr || out_stats == nullptr) {
        invalid("session and out_stats must not be NULL");
        return -1;
    }
    // nanolance keeps no index or metadata cache a session could share.
    std::memset(out_stats, 0, sizeof(*out_stats));
    clear_error();
    return 0;
}

// ── dataset ─────────────────────────────────────────────────────────────────────────────────────

LanceDataset* lance_dataset_open(const char* uri, const char* const* /*storage_opts*/, uint64_t version) {
    return guarded<LanceDataset*>(nullptr, [&] { return open_dataset(uri, version); });
}

LanceDataset* lance_dataset_open_with_session(const char* uri, const char* const* storage_opts, uint64_t version,
                                              const LanceSession* session) {
    if (session == nullptr) {
        invalid("session must not be NULL");
        return nullptr;
    }
    return lance_dataset_open(uri, storage_opts, version);
}

void lance_dataset_close(LanceDataset* dataset) { delete dataset; }

uint64_t lance_dataset_version(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return 0;
    }
    clear_error();
    return dataset->version;
}

uint64_t lance_dataset_count_rows(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return 0;
    }
    clear_error();
    return dataset->info.num_rows();
}

uint64_t lance_dataset_latest_version(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return 0;
    }
    uint64_t latest = 0;
    std::string error;
    if (!nano_lance::dataset_latest_version(dataset->path, latest, error)) {
        fail(error);
        return 0;
    }
    clear_error();
    return latest;
}

LanceVersions* lance_dataset_versions(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return nullptr;
    }
    return guarded<LanceVersions*>(nullptr, [&]() -> LanceVersions* {
        auto out = std::make_unique<LanceVersions>();
        std::string error;
        if (!nano_lance::dataset_versions(dataset->path, out->versions, error)) {
            fail(error);
            return nullptr;
        }
        clear_error();
        return out.release();
    });
}

uint64_t lance_versions_count(const LanceVersions* versions) {
    if (versions == nullptr) {
        invalid("versions must not be NULL");
        return 0;
    }
    clear_error();
    return versions->versions.size();
}

uint64_t lance_versions_id_at(const LanceVersions* versions, size_t index) {
    if (versions == nullptr || index >= versions->versions.size()) {
        invalid("NULL versions handle or index out of range");
        return 0;
    }
    clear_error();
    return versions->versions[index].version;
}

int64_t lance_versions_timestamp_ms_at(const LanceVersions* versions, size_t index) {
    if (versions == nullptr || index >= versions->versions.size()) {
        invalid("NULL versions handle or index out of range");
        return 0;
    }
    clear_error();
    return versions->versions[index].timestamp_ns / 1000000;
}

void lance_versions_close(LanceVersions* versions) { delete versions; }

LanceDataStatistics* lance_dataset_calculate_data_stats(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return nullptr;
    }
    return guarded<LanceDataStatistics*>(nullptr, [&]() -> LanceDataStatistics* {
        auto out = std::make_unique<LanceDataStatistics>();
        if (!data_statistics(*dataset, *out)) {
            return nullptr;
        }
        clear_error();
        return out.release();
    });
}

uint64_t lance_data_statistics_count(const LanceDataStatistics* stats) {
    if (stats == nullptr) {
        invalid("statistics handle must not be NULL");
        return 0;
    }
    clear_error();
    return stats->fields.size();
}

uint32_t lance_data_statistics_field_id_at(const LanceDataStatistics* stats, size_t index) {
    if (stats == nullptr || index >= stats->fields.size()) {
        invalid("NULL statistics handle or index out of range");
        return 0;
    }
    clear_error();
    return stats->fields[index].first;
}

uint64_t lance_data_statistics_bytes_on_disk_at(const LanceDataStatistics* stats, size_t index) {
    if (stats == nullptr || index >= stats->fields.size()) {
        invalid("NULL statistics handle or index out of range");
        return 0;
    }
    clear_error();
    return stats->fields[index].second;
}

void lance_data_statistics_close(LanceDataStatistics* stats) { delete stats; }

LanceDataset* lance_dataset_restore(const LanceDataset* dataset, uint64_t version) {
    if (dataset == nullptr || version == 0U) {
        invalid("dataset must not be NULL and version must be >= 1");
        return nullptr;
    }
    return guarded<LanceDataset*>(nullptr, [&]() -> LanceDataset* {
        uint64_t new_version = 0;
        std::string error;
        if (!nano_lance::dataset_restore(dataset->path, version, new_version, error)) {
            fail(error);
            return nullptr;
        }
        auto out = std::make_unique<LanceDataset>();
        out->path = dataset->path;
        if (!load_info(out->path, new_version, out->info)) {
            return nullptr;
        }
        out->version = new_version;
        clear_error();
        return out.release();
    });
}

int32_t lance_dataset_schema(const LanceDataset* dataset, struct ArrowSchema* out) {
    if (dataset == nullptr || out == nullptr) {
        invalid("dataset and out must not be NULL");
        return -1;
    }
    return guarded<int32_t>(-1, [&]() -> int32_t {
        nano_lance::LanceScanRequest request;
        request.has_version = true;
        request.version = dataset->version;
        std::string error;
        if (!nano_lance::lance_dataset_schema(dataset->path, request, *out, error)) {
            std::memset(out, 0, sizeof(*out));
            fail(error);
            return -1;
        }
        clear_error();
        return 0;
    });
}

uint64_t lance_dataset_fragment_count(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return 0;
    }
    clear_error();
    return dataset->info.fragments.size();
}

int32_t lance_dataset_fragment_ids(const LanceDataset* dataset, uint64_t* out_ids) {
    if (dataset == nullptr || out_ids == nullptr) {
        invalid("dataset and out_ids must not be NULL");
        return -1;
    }
    for (std::size_t i = 0; i < dataset->info.fragments.size(); ++i) {
        out_ids[i] = dataset->info.fragments[i].id;
    }
    clear_error();
    return 0;
}

int32_t lance_dataset_take(const LanceDataset* dataset, const uint64_t* indices, size_t num_indices,
                           const char* const* columns, struct ArrowArrayStream* out) {
    return guarded<int32_t>(-1, [&] {
        return take_impl(dataset, indices, num_indices, columns, out,
                         [&](const nano_lance::LanceScanRequest& request, const std::vector<uint64_t>& sorted,
                             const std::vector<uint64_t>&, ArrowSchema& schema, std::vector<ArrowArray>& batches,
                             std::string& error) {
                             return nano_lance::lance_dataset_take(dataset->path, request, sorted, schema, batches,
                                                                   error);
                         });
    });
}

int32_t lance_dataset_take_rows(const LanceDataset* dataset, const uint64_t* row_ids, size_t num_row_ids,
                                const char* const* columns, struct ArrowArrayStream* out) {
    if (dataset != nullptr && dataset->info.stable_row_ids()) {
        not_supported("row ids of a dataset with stable row ids");
        return -1;
    }
    // Row ids that name no row are left out, as lance-c allows.
    std::vector<uint64_t> found;
    if (dataset != nullptr && row_ids != nullptr) {
        for (size_t i = 0; i < num_row_ids; ++i) {
            const auto fragment = row_ids[i] >> 32U;
            const auto offset = row_ids[i] & 0xFFFFFFFFULL;
            for (const auto& f : dataset->info.fragments) {
                if (f.id == fragment && offset < f.physical_rows) {
                    found.push_back(row_ids[i]);
                    break;
                }
            }
        }
    }
    return guarded<int32_t>(-1, [&] {
        return take_impl(dataset, found.empty() ? row_ids : found.data(), found.size(), columns, out,
                         [&](const nano_lance::LanceScanRequest& request, const std::vector<uint64_t>& sorted,
                             const std::vector<uint64_t>&, ArrowSchema& schema, std::vector<ArrowArray>& batches,
                             std::string& error) {
                             return nano_lance::lance_dataset_take_rows(dataset->path, request, sorted, schema,
                                                                        batches, error);
                         });
    });
}

// ── scanner ─────────────────────────────────────────────────────────────────────────────────────

LanceScanner* lance_scanner_new(const LanceDataset* dataset, const char* const* columns, const char* filter) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return nullptr;
    }
    auto scanner = std::make_unique<LanceScanner>();
    if (filter != nullptr && filter[0] != '\0') {
        nano_lance::expr::Expression parsed;
        std::string error;
        if (!nano_lance::expr::Expression::parse(filter, parsed, error)) {
            set_error(LANCE_ERR_INVALID_ARGUMENT, error);
            return nullptr;
        }
        scanner->filter = filter;
    }
    scanner->origin = dataset;
    scanner->path = dataset->path;
    scanner->version = dataset->version;
    scanner->has_columns = columns != nullptr;
    scanner->columns = column_list(columns);
    clear_error();
    return scanner.release();
}

int32_t lance_scanner_set_limit(LanceScanner* scanner, int64_t limit) {
    return before_scan(scanner, [&] {
        scanner->limit = limit;
        return true;
    });
}

int32_t lance_scanner_set_offset(LanceScanner* scanner, int64_t offset) {
    return before_scan(scanner, [&] {
        if (offset < 0) {
            return invalid("offset must not be negative");
        }
        scanner->offset = offset;
        return true;
    });
}

int32_t lance_scanner_set_batch_size(LanceScanner* scanner, int64_t batch_size) {
    return before_scan(scanner, [&] {
        if (batch_size < 0) {
            return invalid("batch_size must not be negative");
        }
        scanner->batch_size = batch_size;
        return true;
    });
}

int32_t lance_scanner_set_batch_size_bytes(LanceScanner* scanner, uint64_t batch_size_bytes) {
    return before_scan(scanner, [&] { return batch_size_bytes > 0U || invalid("batch_size_bytes must be > 0"); });
}

// Tuning knobs that change how a scan runs, not what it returns: accepted.
int32_t lance_scanner_set_io_buffer_size(LanceScanner* scanner, uint64_t) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_batch_readahead(LanceScanner* scanner, size_t) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_fragment_readahead(LanceScanner* scanner, size_t) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_target_parallelism(LanceScanner* scanner, size_t) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_scan_in_order(LanceScanner* scanner, bool) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_use_scalar_index(LanceScanner* scanner, bool enable) {
    return before_scan(scanner, [&] {
        scanner->search.use_scalar_index = enable;
        return true;
    });
}
int32_t lance_scanner_set_strict_batch_size(LanceScanner* scanner, bool) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_use_stats(LanceScanner* scanner, bool) {
    return before_scan(scanner, [] { return true; });
}

int32_t lance_scanner_with_row_id(LanceScanner* scanner, bool enable) {
    return before_scan(scanner, [&] {
        scanner->with_row_id = enable;
        return true;
    });
}

int32_t lance_scanner_with_row_address(LanceScanner* scanner, bool enable) {
    return before_scan(scanner, [&] {
        scanner->with_row_address = enable;
        return true;
    });
}

int32_t lance_scanner_set_include_deleted_rows(LanceScanner* scanner, bool include_deleted_rows) {
    return before_scan(scanner, [&] {
        scanner->search.include_deleted_rows = include_deleted_rows;
        return true;
    });
}

int32_t lance_scanner_set_blob_handling(LanceScanner* scanner, LanceBlobHandling handling) {
    return before_scan(scanner, [&] {
        const auto value = static_cast<int32_t>(handling);
        if (value < LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS || value > LANCE_BLOB_HANDLING_ALL_DESCRIPTIONS) {
            return invalid("unknown blob handling");
        }
        scanner->blob_handling = value;
        return true;
    });
}

int32_t lance_scanner_set_fragment_ids(LanceScanner* scanner, const uint64_t* ids, size_t len) {
    return before_scan(scanner, [&] {
        if (ids == nullptr && len != 0U) {
            return invalid("ids must not be NULL");
        }
        scanner->has_fragments = true;
        scanner->fragment_ids.assign(ids, ids + len);
        return true;
    });
}

int32_t lance_scanner_set_substrait_filter(LanceScanner* scanner, const uint8_t* bytes, size_t) {
    if (!check_scanner(scanner) || bytes == nullptr) {
        invalid("scanner and bytes must not be NULL");
        return -1;
    }
    not_supported("Substrait filters");
    return -1;
}

int32_t lance_scanner_additional_sql_filter(LanceScanner* scanner, const char* filter) {
    if (!check_scanner(scanner)) {
        return -1;
    }
    if (filter == nullptr || filter[0] == '\0') {
        invalid("filter must not be NULL or empty");
        return -1;
    }
    return before_scan(scanner, [&] {
        nano_lance::expr::Expression parsed;
        std::string error;
        if (!nano_lance::expr::Expression::parse(filter, parsed, error)) {
            set_error(LANCE_ERR_INVALID_ARGUMENT, error);
            return false;
        }
        scanner->filter = scanner->filter.empty() ? std::string(filter)
                                                  : "(" + scanner->filter + ") AND (" + filter + ")";
        return true;
    });
}

int32_t lance_scanner_set_statistics_callback(LanceScanner* scanner, LanceScanStatisticsCallback callback,
                                              void* callback_ctx) {
    return before_scan(scanner, [&] {
        if (callback == nullptr) {
            return invalid("callback must not be NULL");
        }
        scanner->stats_callback = callback;
        scanner->stats_ctx = callback_ctx;
        return true;
    });
}

void lance_scanner_close(LanceScanner* scanner) { delete scanner; }

int32_t lance_scanner_to_arrow_stream(LanceScanner* scanner, struct ArrowArrayStream* out) {
    if (!check_scanner(scanner) || out == nullptr) {
        invalid("scanner and out must not be NULL");
        return -1;
    }
    return guarded<int32_t>(-1, [&]() -> int32_t {
        scanner->started = true;
        std::memset(out, 0, sizeof(*out));
        if (!open_scan(*scanner, *out)) {
            return -1;
        }
        clear_error();
        return 0;
    });
}

int32_t lance_scanner_next(LanceScanner* scanner, LanceBatch** out) {
    if (!check_scanner(scanner) || out == nullptr) {
        invalid("scanner and out must not be NULL");
        return -1;
    }
    *out = nullptr;
    return guarded<int32_t>(-1, [&] { return scanner_next(scanner, out); });
}

void lance_scanner_scan_async(const LanceScanner* scanner, LanceCallback callback, void* callback_ctx) {
    if (callback == nullptr) {
        invalid("callback must not be NULL");
        return;
    }
    if (scanner == nullptr) {
        invalid("scanner must not be NULL");
        callback(callback_ctx, -1, nullptr);
        return;
    }
    // A copy of the scanner's settings: the scan outlives the call, and may outlive the scanner.
    auto copy = std::make_shared<LanceScanner>();
    copy->path = scanner->path;
    copy->version = scanner->version;
    copy->has_columns = scanner->has_columns;
    copy->columns = scanner->columns;
    copy->filter = scanner->filter;
    copy->limit = scanner->limit;
    copy->offset = scanner->offset;
    copy->batch_size = scanner->batch_size;
    copy->with_row_id = scanner->with_row_id;
    copy->with_row_address = scanner->with_row_address;
    copy->has_fragments = scanner->has_fragments;
    copy->fragment_ids = scanner->fragment_ids;
    copy->stats_callback = scanner->stats_callback;
    copy->stats_ctx = scanner->stats_ctx;
    copy->blob_handling = scanner->blob_handling;
    copy->search = scanner->search;
    std::thread([copy, callback, callback_ctx] {
        auto* stream = new ArrowArrayStream{};
        const bool ok = guarded<bool>(false, [&] { return open_scan(*copy, *stream); });
        if (!ok) {
            delete stream;
            callback(callback_ctx, -1, nullptr);
            return;
        }
        clear_error();
        callback(callback_ctx, 0, stream);
    }).detach();
}

void lance_scanner_async_stream_free(struct ArrowArrayStream* stream) {
    if (stream == nullptr) {
        return;
    }
    if (stream->release != nullptr) {
        stream->release(stream);
    }
    delete stream;
}

LancePollStatus lance_scanner_poll_next(LanceScanner* scanner, LanceWaker waker, void* /*waker_ctx*/,
                                        LanceBatch** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!check_scanner(scanner) || waker == nullptr || out == nullptr) {
        invalid("scanner, waker and out must not be NULL");
        return LANCE_POLL_ERROR;
    }
    // Decoding is synchronous here: a poll is always ready, never pending, so the waker never fires.
    const int32_t rc = guarded<int32_t>(-1, [&] { return scanner_next(scanner, out); });
    return rc == 0 ? LANCE_POLL_READY : (rc == 1 ? LANCE_POLL_FINISHED : LANCE_POLL_ERROR);
}

int32_t lance_batch_to_arrow(const LanceBatch* batch, struct ArrowArray* out_array, struct ArrowSchema* out_schema) {
    if (batch == nullptr || out_array == nullptr || out_schema == nullptr) {
        invalid("batch, out_array and out_schema must not be NULL");
        return -1;
    }
    const auto* impl = reinterpret_cast<const LanceBatchImpl*>(batch);
    if (ArrowSchemaDeepCopy(&impl->schema, out_schema) != NANOARROW_OK) {
        fail("failed to copy the batch schema");
        return -1;
    }
    std::vector<int64_t> all;
    for (int64_t i = 0; i < impl->array->array.n_children; ++i) {
        all.push_back(i);
    }
    *out_array = view_of(impl->array, 0, impl->array->array.length, all);
    clear_error();
    return 0;
}

void lance_batch_free(LanceBatch* batch) { delete reinterpret_cast<LanceBatchImpl*>(batch); }

// ── writes ──────────────────────────────────────────────────────────────────────────────────────

int32_t lance_dataset_write(const char* uri, const struct ArrowSchema* schema, struct ArrowArrayStream* stream,
                            int32_t mode, const char* const* /*storage_opts*/, LanceDataset** out_dataset) {
    return guarded<int32_t>(-1, [&] {
        WriteRequest req;
        req.uri = uri;
        req.schema = schema;
        req.stream = stream;
        req.mode = mode;
        req.out_dataset = out_dataset;
        return write_impl(req);
    });
}

int32_t lance_dataset_write_with_params(const char* uri, const struct ArrowSchema* schema,
                                        struct ArrowArrayStream* stream, int32_t mode, const LanceWriteParams* params,
                                        const char* const* /*storage_opts*/, LanceDataset** out_dataset) {
    return guarded<int32_t>(-1, [&] {
        WriteRequest req;
        req.uri = uri;
        req.schema = schema;
        req.stream = stream;
        req.mode = mode;
        req.params = params;
        req.out_dataset = out_dataset;
        return write_impl(req);
    });
}

int32_t lance_write_fragments(const char* uri, const struct ArrowSchema* schema, struct ArrowArrayStream* stream,
                              const char* const* /*storage_opts*/) {
    return guarded<int32_t>(-1, [&] {
        WriteRequest req;
        req.uri = uri;
        req.schema = schema;
        req.stream = stream;
        req.mode = LANCE_WRITE_OVERWRITE;
        req.commit = false;
        return write_impl(req);
    });
}

// ── not supported: dataset changes other than writes (planned), indexes, search, blob files ─────

#define NL_UNSUPPORTED_INT(what) \
    not_supported(what);         \
    return -1

}  // extern "C"

namespace {

/// After a change: the handle sees the new version, as lance-c's mutate-in-place contract says.
bool refresh(LanceDataset* ds, uint64_t version) {
    nano_lance::DatasetInfo info;
    if (!load_info(ds->path, version, info)) {
        return false;
    }
    ds->info = std::move(info);
    ds->version = version;
    return true;
}

template <typename F>
int32_t mutate(LanceDataset* dataset, F&& op) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        uint64_t version = 0;
        std::string error;
        if (!op(version, error)) {
            fail(error);
            return -1;
        }
        if (!refresh(dataset, version)) {
            return -1;
        }
        clear_error();
        return 0;
    });
}

// ── index segments ──────────────────────────────────────────────────────────────────────────────

namespace segments {

// The provenance a trained model carries in its schema metadata (lance-c's index_model.rs).
constexpr const char* kKind = "lance:index_model:kind";
constexpr const char* kMetric = "lance:index_model:metric";
constexpr const char* kDimension = "lance:index_model:dimension";
constexpr const char* kIvfId = "lance:index_model:ivf_id";
constexpr const char* kSubVectors = "lance:index_model:num_sub_vectors";
constexpr const char* kBits = "lance:index_model:num_bits";

struct Provenance {
    std::string kind;
    int32_t metric = 0;
    std::size_t dimension = 0;
    std::string ivf_id;
    std::optional<uint32_t> num_sub_vectors;
    std::optional<uint32_t> num_bits;
};

/// A FixedSizeList<Float32> model handed in through the Arrow C data interface, copied.
struct Model {
    std::vector<float> values;
    std::size_t rows = 0;
    std::size_t list_size = 0;
    Provenance provenance;
};

std::string random_uuid_text() { return nano_lance::pb::uuid_string(nano_lance::index_files::new_uuid()); }

bool parse_uuid(const std::string& text, std::array<uint8_t, 16>& out) {
    std::string hex;
    for (const char c : text) {
        if (c != '-') {
            hex += c;
        }
    }
    if (hex.size() != 32U || text.size() != 36U) {
        return false;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

bool parse_number(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 19U || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    out = std::strtoull(s.c_str(), nullptr, 10);
    return true;
}

/// `name`_schema and its provenance metadata, `name` (the array) and its values.
bool borrow_model(ArrowArray* array, const ArrowSchema* schema, const char* name, Model& out) {
    const std::string n = name;
    if (array == nullptr) {
        return invalid(n + " must not be NULL");
    }
    if (schema == nullptr || schema->release == nullptr || schema->format == nullptr) {
        return invalid(n + "_schema is uninitialized or already released");
    }
    const std::string format = schema->format;
    if (format.rfind("+w:", 0) != 0 || schema->n_children != 1 || schema->children == nullptr ||
        schema->children[0] == nullptr || schema->children[0]->format == nullptr ||
        std::string(schema->children[0]->format) != "f") {
        return invalid(n + "_schema must describe FixedSizeList<Float32> with positive list_size, got '" + format + "'");
    }
    uint64_t list_size = 0;
    if (!parse_number(format.substr(3), list_size) || list_size == 0U) {
        return invalid(n + "_schema must describe FixedSizeList<Float32> with positive list_size, got '" + format + "'");
    }
    // Provenance.
    const auto value = [&](const char* key, std::string& v) {
        ArrowStringView found{nullptr, 0};
        if (schema->metadata == nullptr ||
            ArrowMetadataGetValue(schema->metadata, ArrowCharView(key), &found) != NANOARROW_OK ||
            found.data == nullptr) {
            return false;
        }
        v.assign(found.data, static_cast<std::size_t>(found.size_bytes));
        return true;
    };
    auto& p = out.provenance;
    std::string text;
    for (const char* key : {kKind, kMetric, kDimension, kIvfId}) {
        if (!value(key, text)) {
            return invalid(n + "_schema metadata is missing required key '" + key + "'");
        }
        if (key == kKind) {
            p.kind = text;
            continue;
        }
        uint64_t number = 0;
        std::array<uint8_t, 16> uuid{};
        if (key == kIvfId ? !parse_uuid(text, uuid) : !parse_number(text, number)) {
            return invalid(n + "_schema metadata '" + key + "' is invalid: " + text);
        }
        if (key == kMetric) {
            p.metric = static_cast<int32_t>(number);
        } else if (key == kDimension) {
            p.dimension = static_cast<std::size_t>(number);
        } else {
            p.ivf_id = text;
        }
    }
    for (const char* key : {kSubVectors, kBits}) {
        if (value(key, text)) {
            uint64_t number = 0;
            if (!parse_number(text, number) || number > UINT32_MAX) {
                return invalid(n + "_schema metadata '" + key + "' value '" + text + "' is invalid");
            }
            (key == kSubVectors ? p.num_sub_vectors : p.num_bits) = static_cast<uint32_t>(number);
        }
    }
    // The array: one list per row, no nulls.
    if (array->release == nullptr) {
        return invalid(n + " is uninitialized or already released");
    }
    if (array->length < 0 || array->offset < 0 || array->n_children != 1 || array->children == nullptr ||
        array->children[0] == nullptr || array->children[0]->release == nullptr) {
        return invalid(n + " must be a FixedSizeList array with one child");
    }
    if (array->null_count > 0) {
        return invalid(n + " must not contain NULL lists; null_count is " + std::to_string(array->null_count));
    }
    const ArrowArray* child = array->children[0];
    if (child->n_buffers != 2 || child->buffers == nullptr || child->offset < 0 || child->length < 0) {
        return invalid(n + ".children[0] Float32 must have two buffers");
    }
    if (child->null_count > 0) {
        return invalid(n + " values must not contain NULLs; null_count is " + std::to_string(child->null_count));
    }
    const auto rows = static_cast<std::size_t>(array->length);
    const auto first = static_cast<std::size_t>(array->offset) * list_size + static_cast<std::size_t>(child->offset);
    const auto count = rows * list_size;
    if (static_cast<std::size_t>(child->offset) + static_cast<std::size_t>(child->length) <
        static_cast<std::size_t>(array->offset + array->length) * list_size) {
        return invalid(n + ".children[0] does not cover the parent range");
    }
    const auto* data = static_cast<const float*>(child->buffers[1]);
    if (count > 0U && data == nullptr) {
        return invalid(n + ".children[0] has a NULL values buffer");
    }
    out.values.assign(data == nullptr ? nullptr : data + first, data == nullptr ? nullptr : data + first + count);
    out.rows = rows;
    out.list_size = static_cast<std::size_t>(list_size);
    return true;
}

/// Export `values` as a FixedSizeList<Float32>[list_size] named "model" with `metadata`.
bool export_model(const std::vector<float>& values, std::size_t list_size, const std::map<std::string, std::string>& metadata,
                  ArrowArray* out_array, ArrowSchema* out_schema) {
    ArrowSchema schema;
    ArrowSchemaInit(&schema);
    ArrowBuffer meta;
    bool ok = ArrowSchemaSetTypeFixedSize(&schema, NANOARROW_TYPE_FIXED_SIZE_LIST, static_cast<int32_t>(list_size)) ==
                  NANOARROW_OK &&
              ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_FLOAT) == NANOARROW_OK &&
              ArrowSchemaSetName(schema.children[0], "item") == NANOARROW_OK &&
              ArrowSchemaSetName(&schema, "model") == NANOARROW_OK;
    schema.flags = 0;  // the model itself is not nullable; its items are
    ok = ok && ArrowMetadataBuilderInit(&meta, nullptr) == NANOARROW_OK;
    for (const auto& [k, v] : metadata) {
        ok = ok && ArrowMetadataBuilderAppend(&meta, ArrowCharView(k.c_str()), ArrowCharView(v.c_str())) == NANOARROW_OK;
    }
    ok = ok && ArrowSchemaSetMetadata(&schema, reinterpret_cast<const char*>(meta.data)) == NANOARROW_OK;
    ArrowBufferReset(&meta);
    ArrowArray array;
    array.release = nullptr;
    ok = ok && ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK &&
         ArrowBufferAppend(ArrowArrayBuffer(array.children[0], 1), values.data(),
                           static_cast<int64_t>(values.size() * sizeof(float))) == NANOARROW_OK;
    if (ok) {
        array.children[0]->length = static_cast<int64_t>(values.size());
        array.children[0]->null_count = 0;
        array.length = static_cast<int64_t>(values.size() / list_size);
        array.null_count = 0;
        ok = ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK;
    }
    if (!ok) {
        if (array.release != nullptr) {
            array.release(&array);
        }
        schema.release(&schema);
        set_error(LANCE_ERR_INTERNAL, "out of memory");
        return false;
    }
    ArrowArrayMove(&array, out_array);
    ArrowSchemaMove(&schema, out_schema);
    return true;
}

/// fragment_ids / fragment_count: none (every fragment), or distinct fragments of the version.
bool parse_fragments(const nano_lance::pb::Manifest& manifest, uint64_t version, const uint32_t* ids, size_t count,
                     std::optional<std::vector<uint64_t>>& out) {
    if (ids == nullptr && count == 0U) {
        out.reset();
        return true;
    }
    if (ids == nullptr) {
        return invalid("fragment_ids is NULL but fragment_count is " + std::to_string(count));
    }
    if (count == 0U) {
        return invalid("fragment_ids is non-NULL but fragment_count is 0");
    }
    std::set<uint64_t> existing;
    for (const auto& f : manifest.fragments) {
        existing.insert(f.id);
    }
    std::set<uint32_t> seen;
    std::vector<uint64_t> list;
    for (size_t i = 0; i < count; ++i) {
        if (!seen.insert(ids[i]).second) {
            return invalid("fragment_ids[" + std::to_string(i) + "] is duplicate fragment id " + std::to_string(ids[i]));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (existing.count(ids[i]) == 0U) {
            return invalid("fragment_ids[" + std::to_string(i) + "]=" + std::to_string(ids[i]) +
                           " does not exist in dataset version " + std::to_string(version));
        }
        list.push_back(ids[i]);
    }
    out = std::move(list);
    return true;
}

bool load_snapshot(const LanceDataset& dataset, nano_lance::pb::Manifest& manifest) {
    std::string error;
    if (!nano_lance::load_manifest_version(dataset.path, dataset.version, manifest, error)) {
        fail(error);
        return false;
    }
    return true;
}

/// The dimension of the float32 vector column `column`.
bool vector_dim(const nano_lance::pb::Manifest& manifest, const std::string& column, std::size_t& dim) {
    std::vector<std::string> parts;
    const auto* field = nano_lance::index_files::find_field(manifest, column, parts);
    if (field == nullptr) {
        return invalid("column '" + column + "' does not exist");
    }
    const std::string& t = field->logical_type;
    const std::string prefix = "fixed_size_list:";
    const auto last = t.rfind(':');
    uint64_t n = 0;
    if (t.rfind(prefix, 0) != 0 || last == std::string::npos || last < prefix.size() ||
        !parse_number(t.substr(last + 1U), n) || n == 0U) {
        return invalid("column '" + column + "' is not a vector column (got " + t + ")");
    }
    if (t.substr(prefix.size(), last - prefix.size()) != "float") {
        return invalid("column '" + column + "' must have Float32 vector elements, got " +
                       t.substr(prefix.size(), last - prefix.size()));
    }
    dim = static_cast<std::size_t>(n);
    return true;
}

bool start_builder(LanceIndexSegmentBuilder& b, const LanceDataset& dataset, const char* column, const char* name,
                   const LanceIndexSegmentBuildOptions* options, bool scalar) {
    b.path = dataset.path;
    b.version = dataset.version;
    b.column = column;
    if (name != nullptr) {
        b.name = name;
    }
    if (!load_snapshot(dataset, b.manifest)) {
        return false;
    }
    if (options == nullptr) {
        return true;
    }
    if (!parse_fragments(b.manifest, b.version, options->fragment_ids, options->fragment_count, b.fragments)) {
        return false;
    }
    if ((options->ivf_centroids == nullptr) != (options->ivf_centroids_schema == nullptr)) {
        return invalid("ivf_centroids and ivf_centroids_schema must both be NULL or both be non-NULL");
    }
    if ((options->pq_codebook == nullptr) != (options->pq_codebook_schema == nullptr)) {
        return invalid("pq_codebook and pq_codebook_schema must both be NULL or both be non-NULL");
    }
    if (scalar && (options->ivf_centroids != nullptr || options->pq_codebook != nullptr)) {
        return invalid("ivf_centroids and pq_codebook are not valid for a scalar index segment");
    }
    if (options->mode < LANCE_INDEX_SEGMENT_BUILD_AUTO || options->mode > LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED) {
        return invalid("mode must be 0 (AUTO), 1 (LOCAL_TRAIN), or 2 (PRECOMPUTED); got " +
                       std::to_string(options->mode));
    }
    if (scalar && options->mode == LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED) {
        return invalid("mode PRECOMPUTED is not valid for a scalar index segment");
    }
    b.mode = options->mode;
    if (options->index_uuid != nullptr) {
        std::array<uint8_t, 16> uuid{};
        std::memcpy(uuid.data(), options->index_uuid, 16);
        b.uuid = uuid;
    }
    return true;
}

/// The segment's vector index options from lance-c's parameters (index.rs build_vector_params).
bool vector_options(const LanceVectorIndexSegmentParams& p, nano_lance::VectorIndexOptions& o) {
    switch (p.index_type) {
    case LANCE_INDEX_IVF_FLAT: o.type = "IVF_FLAT"; break;
    case LANCE_INDEX_IVF_PQ: o.type = "IVF_PQ"; break;
    case LANCE_INDEX_IVF_HNSW_SQ: o.type = "IVF_HNSW_SQ"; break;
    case LANCE_INDEX_IVF_SQ: not_supported("IVF_SQ indexes"); return false;
    case LANCE_INDEX_IVF_HNSW_PQ: not_supported("IVF_HNSW_PQ indexes"); return false;
    case LANCE_INDEX_IVF_HNSW_FLAT: not_supported("IVF_HNSW_FLAT indexes"); return false;
    default: return invalid("invalid LanceVectorIndexType " + std::to_string(p.index_type));
    }
    switch (p.metric) {
    case LANCE_METRIC_L2: o.metric = nano_lance::VectorMetric::L2; break;
    case LANCE_METRIC_COSINE: o.metric = nano_lance::VectorMetric::Cosine; break;
    case LANCE_METRIC_DOT: o.metric = nano_lance::VectorMetric::Dot; break;
    case LANCE_METRIC_HAMMING: not_supported("the hamming metric"); return false;
    default: return invalid("invalid LanceMetricType " + std::to_string(p.metric));
    }
    if (p.num_partitions == 0U) {
        return invalid("num_partitions is required for this index type and must be > 0");
    }
    o.num_partitions = p.num_partitions;
    if (o.type == "IVF_PQ") {
        if (p.num_sub_vectors == 0U) {
            return invalid("num_sub_vectors is required for this index type and must be > 0");
        }
        o.num_sub_vectors = p.num_sub_vectors;
        o.num_bits = p.num_bits == 0U ? 8U : p.num_bits;
        if (o.num_bits != 4U && o.num_bits != 8U) {
            return invalid("num_bits must be 4 or 8 for Lance PQ indexes, got " + std::to_string(o.num_bits));
        }
    }
    if (o.type == "IVF_HNSW_SQ") {
        if (p.hnsw_m == 0U) {
            return invalid("hnsw_m is required for this index type and must be > 0");
        }
        if (p.num_bits != 0U && p.num_bits != 8U) {
            return invalid("num_bits must be 0 or 8 for Lance SQ indexes, got " + std::to_string(p.num_bits));
        }
        o.hnsw_m = p.hnsw_m;
        if (p.hnsw_ef_construction != 0U) {
            o.hnsw_ef_construction = p.hnsw_ef_construction;
        }
    }
    if (p.max_iterations != 0U) {
        o.max_iters = p.max_iterations;
    }
    if (p.sample_rate != 0U) {
        o.sample_rate = p.sample_rate;
    }
    return true;
}

/// A trainer's inputs (index_model.rs parse_common).
struct Trainer {
    std::filesystem::path path;
    uint64_t version = 0;
    nano_lance::pb::Manifest manifest;
    std::string column;
    std::size_t dim = 0;
    nano_lance::VectorMetric metric = nano_lance::VectorMetric::L2;
    std::optional<std::vector<uint64_t>> fragments;
};

bool start_trainer(Trainer& t, const LanceDataset* dataset, const char* column, int32_t metric,
                   const uint32_t* fragment_ids, size_t fragment_count, ArrowArray* out_array, ArrowSchema* out_schema) {
    if (dataset == nullptr || column == nullptr || out_array == nullptr || out_schema == nullptr) {
        return invalid("dataset, column, out_array, and out_schema must not be NULL");
    }
    if (out_array->release != nullptr || out_schema->release != nullptr) {
        return invalid("out_array and out_schema must be empty (release callbacks must be NULL)");
    }
    switch (metric) {
    case LANCE_METRIC_L2: t.metric = nano_lance::VectorMetric::L2; break;
    case LANCE_METRIC_COSINE: t.metric = nano_lance::VectorMetric::Cosine; break;
    case LANCE_METRIC_DOT: t.metric = nano_lance::VectorMetric::Dot; break;
    case LANCE_METRIC_HAMMING: return invalid("the hamming metric is not valid for Float32 vectors");
    default: return invalid("invalid LanceMetricType " + std::to_string(metric));
    }
    if (column[0] == '\0') {
        return invalid("column must not be NULL or empty");
    }
    t.path = dataset->path;
    t.version = dataset->version;
    t.column = column;
    return load_snapshot(*dataset, t.manifest) && vector_dim(t.manifest, t.column, t.dim) &&
           parse_fragments(t.manifest, t.version, fragment_ids, fragment_count, t.fragments);
}

bool train(const Trainer& t, const nano_lance::VectorIndexOptions& o, const nano_lance::index_build::VectorModel* model,
           nano_lance::index_build::VectorModel& trained) {
    nano_lance::index_build::SegmentTarget target;
    target.manifest = &t.manifest;
    target.version = t.version;
    target.fragments = t.fragments ? &*t.fragments : nullptr;
    target.model = model;
    target.trained = &trained;
    std::string error;
    if (!nano_lance::index_build::build_vector_segment(t.path, t.column, o, target, error)) {
        fail(error);
        return false;
    }
    return true;
}

/// The last part of an index details type URL: "BTreeIndexDetails", "VectorIndexDetails", ...
std::string details_kind(const std::string& url) {
    const auto dot = url.rfind('.');
    return dot == std::string::npos ? url : url.substr(dot + 1U);
}

/// The index name a build gets (create.rs): the given one, else <column>_idx, then _2, _3, ... past
/// indexes of that name on another column or of another kind; refused when an index of that kind
/// on this column already holds it.
bool resolve_name(LanceIndexSegmentBuilder& b, const std::string& kind) {
    std::vector<std::string> parts;
    const auto* field = nano_lance::index_files::find_field(b.manifest, b.column, parts);
    if (field == nullptr) {
        return invalid("column '" + b.column + "' does not exist");
    }
    const auto clashes = [&](const std::string& name) {
        return std::any_of(b.manifest.indices.begin(), b.manifest.indices.end(), [&](const auto& i) {
            return i.name == name && (i.fields.empty() || i.fields.front() != field->id ||
                                      details_kind(i.details_type_url) != kind);
        });
    };
    if (b.name.empty()) {
        const std::string base = b.column + "_idx";
        b.name = base;
        for (int n = 2; clashes(b.name); ++n) {
            b.name = base + "_" + std::to_string(n);
        }
    }
    bool named = false;
    for (const auto& i : b.manifest.indices) {
        if (i.name != b.name) {
            continue;
        }
        if (i.fields.empty() || i.fields.front() != field->id) {
            set_error(LANCE_ERR_INDEX, "Index name '" + b.name +
                                           "' already exists with different fields, please specify a different name");
            return false;
        }
        named = true;
    }
    if (named) {
        set_error(LANCE_ERR_INDEX,
                  "Index name '" + b.name + "' already exists, please specify a different name or use replace=True");
        return false;
    }
    return true;
}

bool execute(LanceIndexSegmentBuilder& b, std::vector<uint8_t>& bytes) {
    std::string kind = "VectorIndexDetails";
    if (!b.is_vector) {
        kind = b.scalar_type == LANCE_SCALAR_BTREE    ? "BTreeIndexDetails"
               : b.scalar_type == LANCE_SCALAR_BITMAP ? "BitmapIndexDetails"
               : b.scalar_type == LANCE_SCALAR_LABEL_LIST ? "LabelListIndexDetails"
                                                          : "InvertedIndexDetails";
    }
    if (!resolve_name(b, kind)) {
        return false;
    }
    if (b.uuid && b.fragments && !b.fragments->empty() &&
        (b.scalar_type == LANCE_SCALAR_BTREE || b.scalar_type == LANCE_SCALAR_LABEL_LIST)) {
        return invalid(std::string("index_uuid is no longer accepted for ") +
                       (b.scalar_type == LANCE_SCALAR_BTREE ? "BTree" : "LabelList") +
                       " distributed index builds; segment UUIDs are generated by Lance and returned in the index "
                       "metadata.");
    }
    nano_lance::pb::IndexMetadata entry;
    nano_lance::index_build::Progress progress;
    if (b.callback != nullptr) {
        progress = [&b](int event, const char* stage, uint64_t total, const char* unit, uint64_t completed) {
            b.callback(b.callback_ctx, event, stage, total, unit, completed);
        };
    }
    nano_lance::index_build::SegmentTarget target;
    target.manifest = &b.manifest;
    target.version = b.version;
    target.fragments = b.fragments ? &*b.fragments : nullptr;
    target.out = &entry;
    target.uuid = b.uuid ? &*b.uuid : nullptr;
    target.progress = b.callback != nullptr ? &progress : nullptr;
    std::string error;
    bool ok = false;
    if (b.is_vector) {
        b.vector.name = b.name;
        target.model = b.model ? &*b.model : nullptr;
        ok = nano_lance::index_build::build_vector_segment(b.path, b.column, b.vector, target, error);
    } else if (b.scalar_type == LANCE_SCALAR_INVERTED) {
        b.inverted.name = b.name;
        ok = nano_lance::index_build::build_inverted_segment(b.path, b.column, b.inverted, target, error);
    } else {
        nano_lance::ScalarIndexOptions o;
        o.name = b.name;
        const auto type = b.scalar_type == LANCE_SCALAR_BTREE    ? nano_lance::ScalarIndexType::BTree
                          : b.scalar_type == LANCE_SCALAR_BITMAP ? nano_lance::ScalarIndexType::Bitmap
                                                                 : nano_lance::ScalarIndexType::LabelList;
        ok = nano_lance::index_build::build_scalar_segment(b.path, b.column, type, o, target, error);
    }
    if (!ok) {
        fail(error);
        return false;
    }
    bytes = nano_lance::pb::encode_index_message(entry);
    return true;
}

/// One IndexMetadata message, with Lance's range checks (index_segment.rs decode_segment_metadata).
bool decode_metadata(const uint8_t* bytes, size_t len, nano_lance::pb::IndexMetadata& out, std::string& error) {
    if (!nano_lance::pb::decode_index_message(bytes, len, out, error)) {
        error = "invalid IndexMetadata protobuf: " + error;
        return false;
    }
    if (out.index_version > static_cast<uint32_t>(INT32_MAX)) {
        error = "IndexMetadata index_version must be >= 0";
        return false;
    }
    for (std::size_t i = 0; i < out.fields.size(); ++i) {
        if (out.fields[i] < 0) {
            error = "IndexMetadata fields[" + std::to_string(i) + "] must be >= 0, got " + std::to_string(out.fields[i]);
            return false;
        }
    }
    if (out.created_at > static_cast<uint64_t>(INT64_MAX)) {
        error = "IndexMetadata created_at exceeds i64::MAX milliseconds";
        return false;
    }
    return true;
}

/// Whether protobuf message `b` has field `number`.
bool has_field(const std::vector<uint8_t>& b, uint64_t number) {
    std::size_t i = 0;
    const auto varint = [&](uint64_t& v) {
        v = 0;
        for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
            const auto c = b[i++];
            v |= static_cast<uint64_t>(c & 0x7FU) << shift;
            if ((c & 0x80U) == 0U) {
                return true;
            }
        }
        return false;
    };
    while (i < b.size()) {
        uint64_t key = 0;
        uint64_t v = 0;
        if (!varint(key)) {
            return false;
        }
        if ((key >> 3U) == number) {
            return true;
        }
        switch (key & 7U) {
        case 0: if (!varint(v)) return false; break;
        case 1: i += 8; break;
        case 2: if (!varint(v) || v > b.size() - i) return false; i += static_cast<std::size_t>(v); break;
        case 5: i += 4; break;
        default: return false;
        }
    }
    return false;
}

/// LanceScalarIndexType / LanceVectorIndexType of a segment; -1 an unknown kind, -2 RQ.
int32_t index_type_code(const nano_lance::pb::IndexMetadata& index) {
    const auto kind = details_kind(index.details_type_url);
    if (kind == "BTreeIndexDetails") return LANCE_SCALAR_BTREE;
    if (kind == "BitmapIndexDetails") return LANCE_SCALAR_BITMAP;
    if (kind == "LabelListIndexDetails") return LANCE_SCALAR_LABEL_LIST;
    if (kind == "InvertedIndexDetails") return LANCE_SCALAR_INVERTED;
    if (kind != "VectorIndexDetails") return -1;
    const auto type = nano_lance::vector_index_type(index.details_value);
    if (type.empty()) {
        // No compression is Lance's flat; with an HNSW layer (field 3), IVF_HNSW_FLAT.
        return has_field(index.details_value, 3) ? LANCE_INDEX_IVF_HNSW_FLAT : LANCE_INDEX_IVF_FLAT;
    }
    if (type == "IVF_FLAT") return LANCE_INDEX_IVF_FLAT;
    if (type == "IVF_SQ") return LANCE_INDEX_IVF_SQ;
    if (type == "IVF_PQ") return LANCE_INDEX_IVF_PQ;
    if (type == "IVF_HNSW_SQ") return LANCE_INDEX_IVF_HNSW_SQ;
    if (type == "IVF_HNSW_PQ") return LANCE_INDEX_IVF_HNSW_PQ;
    if (type == "IVF_HNSW_FLAT") return LANCE_INDEX_IVF_HNSW_FLAT;
    return -2;
}

LanceErrorCode code_of(nano_lance::SegmentErrorKind kind, const std::string& error) {
    switch (kind) {
    case nano_lance::SegmentErrorKind::InvalidArgument: return LANCE_ERR_INVALID_ARGUMENT;
    case nano_lance::SegmentErrorKind::Index: return LANCE_ERR_INDEX;
    case nano_lance::SegmentErrorKind::NotFound: return LANCE_ERR_NOT_FOUND;
    default: return code_for(error);
    }
}

bool list(const LanceDataset* dataset, const char* index_name, std::vector<std::array<uint8_t, 16>>& out) {
    if (dataset == nullptr || index_name == nullptr) {
        return invalid("dataset and index_name must not be NULL");
    }
    if (index_name[0] == '\0') {
        return invalid("index_name must not be empty");
    }
    std::string error;
    nano_lance::SegmentErrorKind kind{};
    if (!nano_lance::dataset_index_segments(dataset->path, dataset->version, index_name, out, error, kind)) {
        set_error(code_of(kind, error), error);
        return false;
    }
    return true;
}

}  // namespace segments

}  // namespace

extern "C" {

int32_t lance_dataset_delete(LanceDataset* dataset, const char* predicate, uint64_t* out_num_deleted) {
    if (dataset == nullptr || predicate == nullptr || predicate[0] == '\0') {
        invalid("dataset and predicate must not be NULL or empty");
        return -1;
    }
    uint64_t deleted = 0;
    const int32_t rc = mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_delete(dataset->path, predicate, deleted, v, e);
    });
    if (rc == 0 && out_num_deleted != nullptr) {
        *out_num_deleted = deleted;
    }
    return rc;
}

int32_t lance_dataset_update(LanceDataset* dataset, const char* predicate, const char* const* columns,
                             const char* const* values, size_t num_updates, uint64_t* out_num_updated) {
    if (dataset == nullptr || columns == nullptr || values == nullptr || num_updates == 0U) {
        invalid("dataset, columns and values must not be NULL, and num_updates must be > 0");
        return -1;
    }
    std::vector<std::pair<std::string, std::string>> assignments;
    for (size_t i = 0; i < num_updates; ++i) {
        if (columns[i] == nullptr || values[i] == nullptr || columns[i][0] == '\0') {
            invalid("an update column or value is NULL or empty");
            return -1;
        }
        assignments.emplace_back(columns[i], values[i]);
    }
    const std::string where = predicate != nullptr ? predicate : "";
    uint64_t updated = 0;
    const int32_t rc = mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_update(dataset->path, where.empty() ? nullptr : &where, assignments, updated, v, e);
    });
    if (rc == 0 && out_num_updated != nullptr) {
        *out_num_updated = updated;
    }
    return rc;
}

int32_t lance_dataset_merge_insert(LanceDataset* dataset, const char* const* on_columns, size_t num_on_columns,
                                   struct ArrowArrayStream* source, const LanceMergeInsertParams* params,
                                   LanceMergeInsertResult* out_result) {
    if (dataset == nullptr || on_columns == nullptr || num_on_columns == 0U || source == nullptr) {
        if (source != nullptr && source->release != nullptr) {
            source->release(source);
        }
        invalid("dataset, on_columns and source must not be NULL, and num_on_columns must be > 0");
        return -1;
    }
    nano_lance::MergeInsertSpec spec;
    for (size_t i = 0; i < num_on_columns; ++i) {
        spec.on.emplace_back(on_columns[i] != nullptr ? on_columns[i] : "");
    }
    if (params != nullptr) {
        using WM = nano_lance::MergeInsertSpec::WhenMatched;
        switch (params->when_matched) {
            case LANCE_MERGE_WHEN_MATCHED_DO_NOTHING: spec.when_matched = WM::DoNothing; break;
            case LANCE_MERGE_WHEN_MATCHED_UPDATE_ALL: spec.when_matched = WM::UpdateAll; break;
            case LANCE_MERGE_WHEN_MATCHED_FAIL: spec.when_matched = WM::Fail; break;
            case LANCE_MERGE_WHEN_MATCHED_DELETE: spec.when_matched = WM::Delete; break;
            case LANCE_MERGE_WHEN_MATCHED_UPDATE_IF:
                if (params->when_matched_expr == nullptr || params->when_matched_expr[0] == '\0') {
                    if (source->release != nullptr) {
                        source->release(source);
                    }
                    invalid("when_matched UPDATE_IF requires a non-empty when_matched_expr");
                    return -1;
                }
                spec.when_matched = WM::UpdateIf;
                spec.when_matched_condition = params->when_matched_expr;
                break;
            default:
                if (source->release != nullptr) {
                    source->release(source);
                }
                invalid("unknown when_matched");
                return -1;
        }
        if (params->when_not_matched != LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL &&
            params->when_not_matched != LANCE_MERGE_WHEN_NOT_MATCHED_DO_NOTHING) {
            if (source->release != nullptr) {
                source->release(source);
            }
            invalid("unknown when_not_matched");
            return -1;
        }
        spec.when_not_matched_insert_all = params->when_not_matched == LANCE_MERGE_WHEN_NOT_MATCHED_INSERT_ALL;
        switch (params->when_not_matched_by_source) {
            case LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_KEEP: break;
            case LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_DELETE: spec.when_not_matched_by_source_delete = true; break;
            case LANCE_MERGE_WHEN_NOT_MATCHED_BY_SOURCE_DELETE_IF:
                if (params->when_not_matched_by_source_expr == nullptr ||
                    params->when_not_matched_by_source_expr[0] == '\0') {
                    if (source->release != nullptr) {
                        source->release(source);
                    }
                    invalid("DELETE_IF needs when_not_matched_by_source_expr");
                    return -1;
                }
                spec.when_not_matched_by_source_delete = true;
                spec.when_not_matched_by_source_condition = params->when_not_matched_by_source_expr;
                break;
            default:
                if (source->release != nullptr) {
                    source->release(source);
                }
                invalid("unknown when_not_matched_by_source");
                return -1;
        }
    }
    nano_lance::MergeInsertStats stats;
    const int32_t rc = mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_merge_insert(dataset->path, spec, *source, stats, v, e);
    });
    if (rc == 0 && out_result != nullptr) {
        out_result->num_inserted_rows = stats.inserted;
        out_result->num_updated_rows = stats.updated;
        out_result->num_deleted_rows = stats.deleted;
    }
    return rc;
}

int32_t lance_dataset_compact_files(LanceDataset* dataset, const LanceCompactionOptions* options,
                                    LanceCompactionMetrics* out_metrics) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return -1;
    }
    nano_lance::CompactionOptions opts;
    if (options != nullptr && options->target_rows_per_fragment != 0U) {
        opts.target_rows_per_fragment = options->target_rows_per_fragment;
    }
    nano_lance::CompactionMetrics metrics;
    const int32_t rc = mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_compact_files(dataset->path, opts, metrics, v, e);
    });
    if (rc == 0 && out_metrics != nullptr) {
        out_metrics->fragments_removed = metrics.fragments_removed;
        out_metrics->fragments_added = metrics.fragments_added;
        out_metrics->files_removed = metrics.files_removed;
        out_metrics->files_added = metrics.files_added;
    }
    return rc;
}

int32_t lance_dataset_drop_columns(LanceDataset* dataset, const char* const* columns, size_t num_columns) {
    if (dataset == nullptr || columns == nullptr || num_columns == 0U) {
        invalid("dataset and columns must not be NULL, and num_columns must be > 0");
        return -1;
    }
    std::vector<std::string> names;
    for (size_t i = 0; i < num_columns; ++i) {
        if (columns[i] == nullptr || columns[i][0] == '\0') {
            invalid("a column name is NULL or empty");
            return -1;
        }
        names.emplace_back(columns[i]);
    }
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_drop_columns(dataset->path, names, v, e);
    });
}

int32_t lance_dataset_alter_columns(LanceDataset* dataset, const LanceColumnAlteration* alterations,
                                    size_t num_alterations) {
    if (dataset == nullptr || alterations == nullptr || num_alterations == 0U) {
        invalid("dataset and alterations must not be NULL, and num_alterations must be > 0");
        return -1;
    }
    std::vector<nano_lance::ColumnAlteration> alts;
    for (size_t i = 0; i < num_alterations; ++i) {
        const auto& a = alterations[i];
        if (a.path == nullptr || a.path[0] == '\0') {
            invalid("an alteration's path is NULL or empty");
            return -1;
        }
        if (a.nullable_mode < LANCE_COLUMN_NULLABLE_UNCHANGED || a.nullable_mode > LANCE_COLUMN_NULLABLE_FALSE) {
            invalid("unknown nullable_mode");
            return -1;
        }
        if (a.rename == nullptr && a.nullable_mode == LANCE_COLUMN_NULLABLE_UNCHANGED && a.data_type == nullptr) {
            invalid("an alteration must change something");
            return -1;
        }
        nano_lance::ColumnAlteration alt;
        alt.path = a.path;
        if (a.rename != nullptr) {
            alt.rename = std::string(a.rename);
        }
        if (a.nullable_mode != LANCE_COLUMN_NULLABLE_UNCHANGED) {
            alt.nullable = a.nullable_mode == LANCE_COLUMN_NULLABLE_TRUE;
        }
        alt.data_type = a.data_type;
        alts.push_back(std::move(alt));
    }
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_alter_columns(dataset->path, alts, v, e);
    });
}

int32_t lance_dataset_add_columns_sql(LanceDataset* dataset, const LanceSqlColumn* columns, size_t num_columns,
                                      uint64_t /*batch_size*/) {
    if (dataset == nullptr || columns == nullptr || num_columns == 0U) {
        invalid("dataset and columns must not be NULL, and num_columns must be > 0");
        return -1;
    }
    std::vector<std::pair<std::string, std::string>> cols;
    for (size_t i = 0; i < num_columns; ++i) {
        if (columns[i].name == nullptr || columns[i].name[0] == '\0' || columns[i].expression == nullptr ||
            columns[i].expression[0] == '\0') {
            invalid("a column's name or expression is NULL or empty");
            return -1;
        }
        cols.emplace_back(columns[i].name, columns[i].expression);
    }
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_add_columns_sql(dataset->path, cols, v, e);
    });
}

int32_t lance_dataset_add_columns_nulls(LanceDataset* dataset, const struct ArrowSchema* schema) {
    if (dataset == nullptr || schema == nullptr || schema->n_children <= 0) {
        invalid("dataset and a schema of at least one field are required");
        return -1;
    }
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_add_columns_nulls(dataset->path, *schema, v, e);
    });
}

int32_t lance_dataset_add_columns_stream(LanceDataset* dataset, struct ArrowArrayStream* stream,
                                         uint64_t /*batch_size*/) {
    if (dataset == nullptr || stream == nullptr) {
        if (stream != nullptr && stream->release != nullptr) {
            stream->release(stream);
        }
        invalid("dataset and stream must not be NULL");
        return -1;
    }
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_add_columns_stream(dataset->path, *stream, v, e);
    });
}

}  // extern "C"

namespace {

/// Blob handles for `rows` (row indices, or row ids when `by_id`), in their order. All or nothing:
/// on failure `out` is untouched.
int32_t take_blobs(const LanceDataset* dataset, const uint64_t* rows, size_t count, const char* column,
                   LanceBlobFile** out, bool by_id) {
    if (dataset == nullptr || column == nullptr || out == nullptr || (rows == nullptr && count != 0U)) {
        invalid("dataset, column, out and (when num > 0) the rows must not be NULL");
        return -1;
    }
    if (count == 0U) {
        clear_error();
        return 0;
    }
    return guarded<int32_t>(-1, [&]() -> int32_t {
        const std::vector<uint64_t> wanted(rows, rows + count);
        std::vector<uint64_t> sorted(wanted);
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        const std::vector<std::string> names = {column};
        nano_lance::LanceScanRequest request;
        request.has_version = true;
        request.version = dataset->version;
        request.columns = &names;
        request.blob_handling = nano_lance::BlobHandling::Locations;
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        const bool ok = by_id ? nano_lance::lance_dataset_take_rows(dataset->path, request, sorted, schema, batches, error)
                              : nano_lance::lance_dataset_take(dataset->path, request, sorted, schema, batches, error);
        if (!ok) {
            fail(error);
            return -1;
        }
        struct Cleanup {
            ArrowSchema& schema;
            std::vector<ArrowArray>& batches;
            ~Cleanup() {
                for (auto& b : batches) {
                    if (b.release != nullptr) {
                        b.release(&b);
                    }
                }
                if (schema.release != nullptr) {
                    schema.release(&schema);
                }
            }
        } cleanup{schema, batches};
        const ArrowSchema* field = schema.n_children == 1 ? schema.children[0] : nullptr;
        if (field == nullptr || field->n_children != 6 || std::strcmp(field->children[5]->name, "file") != 0) {
            invalid("column is not a blob column");
            return -1;
        }
        // One handle (or none, for a null value) per row of `sorted`, in order.
        std::vector<std::unique_ptr<LanceBlobFile>> taken;
        taken.reserve(sorted.size());
        for (auto& batch : batches) {
            ArrowArrayView view{};
            ArrowError aerr{};
            if (ArrowArrayViewInitFromSchema(&view, &schema, &aerr) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &aerr) != NANOARROW_OK) {
                ArrowArrayViewReset(&view);
                fail(std::string("cannot read the blob descriptions: ") + aerr.message);
                return -1;
            }
            const ArrowArrayView* blob = view.children[0];
            for (int64_t r = 0; r < batch.length; ++r) {
                if (ArrowArrayViewIsNull(blob, r)) {
                    taken.emplace_back(nullptr);
                    continue;
                }
                auto handle = std::make_unique<LanceBlobFile>();
                const auto kind = static_cast<uint8_t>(ArrowArrayViewGetUIntUnsafe(blob->children[0], r));
                handle->location.position = ArrowArrayViewGetUIntUnsafe(blob->children[1], r);
                handle->location.size = ArrowArrayViewGetUIntUnsafe(blob->children[2], r);
                const auto file = ArrowArrayViewGetStringUnsafe(blob->children[5], r);
                handle->location.file.assign(file.data, static_cast<std::size_t>(file.size_bytes));
                handle->location.external = kind == nano_lance::kBlobKindExternal;
                if (kind == nano_lance::kBlobKindDedicated) {
                    handle->location.position = 0;
                }
                taken.push_back(std::move(handle));
            }
            ArrowArrayViewReset(&view);
        }
        if (taken.size() != sorted.size()) {
            fail("take returned " + std::to_string(taken.size()) + " blobs for " + std::to_string(sorted.size()));
            return -1;
        }
        // Repeats of a row get handles of their own: each has its own cursor.
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            const auto k = static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), wanted[i]) -
                                                    sorted.begin());
            out[i] = taken[k] == nullptr ? nullptr : new LanceBlobFile(*taken[k]);
        }
        clear_error();
        return 0;
    });
}

/// `len` bytes of `blob` from blob-relative `offset` into `dst`.
bool read_blob(const LanceBlobFile& blob, uint64_t offset, uint8_t* dst, std::size_t len) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!nano_lance::blob_v2_read(blob.location, offset, len, bytes, error)) {
        set_error(LANCE_ERR_IO, error);
        return false;
    }
    if (len != 0U) {
        std::memcpy(dst, bytes.data(), len);
    }
    return true;
}

}  // namespace

extern "C" {

int32_t lance_dataset_take_blobs(const LanceDataset* dataset, const uint64_t* row_ids, size_t num_row_ids,
                                 const char* column, LanceBlobFile** out) {
    return take_blobs(dataset, row_ids, num_row_ids, column, out, true);
}

int32_t lance_dataset_take_blobs_by_indices(const LanceDataset* dataset, const uint64_t* indices, size_t num_indices,
                                            const char* column, LanceBlobFile** out) {
    return take_blobs(dataset, indices, num_indices, column, out, false);
}

uint64_t lance_blob_file_size(const LanceBlobFile* blob) {
    if (blob == nullptr) {
        invalid("blob must not be NULL");
        return 0;
    }
    clear_error();
    return blob->location.size;
}

int32_t lance_blob_file_read(LanceBlobFile* blob, uint8_t* dst, size_t dst_len) {
    if (blob == nullptr) {
        invalid("blob must not be NULL");
        return -1;
    }
    const uint64_t remaining = blob->cursor >= blob->location.size ? 0U : blob->location.size - blob->cursor;
    if (remaining > dst_len || (dst == nullptr && remaining != 0U)) {
        invalid("dst is smaller than the bytes remaining in the blob");
        return -1;
    }
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (remaining != 0U && !read_blob(*blob, blob->cursor, dst, static_cast<std::size_t>(remaining))) {
            return -1;
        }
        blob->cursor += remaining;
        clear_error();
        return 0;
    });
}

int32_t lance_blob_file_read_up_to(LanceBlobFile* blob, uint8_t* dst, size_t len, size_t* bytes_read) {
    if (blob == nullptr || bytes_read == nullptr || (dst == nullptr && len != 0U)) {
        invalid("blob, bytes_read and (when len > 0) dst must not be NULL");
        return -1;
    }
    const uint64_t remaining = blob->cursor >= blob->location.size ? 0U : blob->location.size - blob->cursor;
    const auto n = static_cast<std::size_t>(std::min<uint64_t>(len, remaining));
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (n != 0U && !read_blob(*blob, blob->cursor, dst, n)) {
            return -1;
        }
        blob->cursor += n;
        *bytes_read = n;
        clear_error();
        return 0;
    });
}

int32_t lance_blob_file_read_range(const LanceBlobFile* blob, uint64_t offset, uint8_t* dst, size_t len) {
    if (blob == nullptr || (dst == nullptr && len != 0U)) {
        invalid("blob and (when len > 0) dst must not be NULL");
        return -1;
    }
    if (len == 0U) {
        clear_error();
        return 0;
    }
    if (offset > std::numeric_limits<uint64_t>::max() - len || offset + len > blob->location.size) {
        invalid("the range ends past the end of the blob");
        return -1;
    }
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (!read_blob(*blob, offset, dst, len)) {
            return -1;
        }
        clear_error();
        return 0;
    });
}

int32_t lance_blob_file_seek(LanceBlobFile* blob, uint64_t pos) {
    if (blob == nullptr) {
        invalid("blob must not be NULL");
        return -1;
    }
    blob->cursor = pos;
    clear_error();
    return 0;
}

int32_t lance_blob_file_tell(const LanceBlobFile* blob, uint64_t* pos) {
    if (blob == nullptr || pos == nullptr) {
        invalid("blob and pos must not be NULL");
        return -1;
    }
    *pos = blob->cursor;
    clear_error();
    return 0;
}

void lance_blob_file_close(LanceBlobFile* blob) { delete blob; }

int32_t lance_dataset_create_vector_index(LanceDataset* dataset, const char* column, const char* index_name,
                                          const LanceVectorIndexParams* params, bool replace) {
    if (dataset == nullptr || column == nullptr || params == nullptr) {
        invalid("dataset, column, and params must not be NULL");
        return -1;
    }
    if (column[0] == '\0') {
        invalid("column must not be empty");
        return -1;
    }
    nano_lance::VectorIndexOptions options;
    switch (params->index_type) {
    case LANCE_INDEX_IVF_FLAT: options.type = "IVF_FLAT"; break;
    case LANCE_INDEX_IVF_PQ: options.type = "IVF_PQ"; break;
    case LANCE_INDEX_IVF_SQ: NL_UNSUPPORTED_INT("IVF_SQ indexes");
    case LANCE_INDEX_IVF_HNSW_SQ: options.type = "IVF_HNSW_SQ"; break;
    case LANCE_INDEX_IVF_HNSW_PQ: NL_UNSUPPORTED_INT("IVF_HNSW_PQ indexes");
    case LANCE_INDEX_IVF_HNSW_FLAT: NL_UNSUPPORTED_INT("IVF_HNSW_FLAT indexes");
    default:
        invalid("unknown vector index type " + std::to_string(static_cast<int>(params->index_type)));
        return -1;
    }
    switch (params->metric) {
    case LANCE_METRIC_L2: options.metric = nano_lance::VectorMetric::L2; break;
    case LANCE_METRIC_COSINE: options.metric = nano_lance::VectorMetric::Cosine; break;
    case LANCE_METRIC_DOT: options.metric = nano_lance::VectorMetric::Dot; break;
    case LANCE_METRIC_HAMMING: NL_UNSUPPORTED_INT("the hamming metric");
    default:
        invalid("unknown metric " + std::to_string(static_cast<int>(params->metric)));
        return -1;
    }
    if (params->num_partitions == 0U) {
        invalid("num_partitions is required and must be greater than 0");
        return -1;
    }
    options.num_partitions = params->num_partitions;
    if (options.type == "IVF_PQ") {
        if (params->num_sub_vectors == 0U) {
            invalid("num_sub_vectors is required and must be greater than 0");
            return -1;
        }
        options.num_sub_vectors = params->num_sub_vectors;
        options.num_bits = params->num_bits == 0U ? 8U : params->num_bits;
        if (options.num_bits != 4U && options.num_bits != 8U) {
            invalid("num_bits must be 4 or 8 for Lance PQ indexes, got " + std::to_string(options.num_bits));
            return -1;
        }
    }
    if (options.type == "IVF_HNSW_SQ") {
        if (params->hnsw_m == 0U) {
            invalid("hnsw_m is required for this index type and must be > 0");
            return -1;
        }
        if (params->num_bits != 0U && params->num_bits != 8U) {
            invalid("num_bits must be 0 or 8 for Lance SQ indexes, got " + std::to_string(params->num_bits));
            return -1;
        }
        options.hnsw_m = params->hnsw_m;
        if (params->hnsw_ef_construction != 0U) {
            options.hnsw_ef_construction = params->hnsw_ef_construction;
        }
    }
    if (params->max_iterations != 0U) {
        options.max_iters = params->max_iterations;
    }
    if (params->sample_rate != 0U) {
        options.sample_rate = params->sample_rate;
    }
    options.name = index_name != nullptr ? index_name : "";
    options.replace = replace;
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_create_vector_index(dataset->path, column, options, v, e);
    });
}
int32_t lance_dataset_create_scalar_index(LanceDataset* dataset, const char* column, const char* index_name,
                                          LanceScalarIndexType index_type, const char* params_json, bool replace) {
    if (dataset == nullptr || column == nullptr || column[0] == '\0') {
        invalid("dataset and column must not be NULL or empty");
        return -1;
    }
    if (index_type == LANCE_SCALAR_INVERTED) {
        // The analyzer as Lance's InvertedIndexParams spell it ({"base_tokenizer": "simple", ...});
        // LanceDB's defaults for what is left out.
        nano_lance::InvertedIndexOptions options;
        options.name = index_name != nullptr ? index_name : "";
        options.replace = replace;
        std::string error;
        if (params_json != nullptr && params_json[0] != '\0' &&
            !nano_lance::fts::parse_params(params_json, options.params, error)) {
            set_error(error.find("not supported") != std::string::npos ? LANCE_ERR_NOT_SUPPORTED
                                                                       : LANCE_ERR_INVALID_ARGUMENT,
                      error);
            return -1;
        }
        return mutate(dataset, [&](uint64_t& v, std::string& e) {
            return nano_lance::dataset_create_inverted_index(dataset->path, column, options, v, e);
        });
    }
    nano_lance::ScalarIndexType type{};
    switch (index_type) {
        case LANCE_SCALAR_BTREE: type = nano_lance::ScalarIndexType::BTree; break;
        case LANCE_SCALAR_BITMAP: type = nano_lance::ScalarIndexType::Bitmap; break;
        case LANCE_SCALAR_LABEL_LIST: type = nano_lance::ScalarIndexType::LabelList; break;
        default:
            invalid("unknown scalar index type " + std::to_string(static_cast<int>(index_type)));
            return -1;
    }
    if (params_json != nullptr && params_json[0] != '\0' && std::string(params_json) != "{}") {
        NL_UNSUPPORTED_INT("scalar index parameters");
    }
    nano_lance::ScalarIndexOptions options;
    options.name = index_name != nullptr ? index_name : "";
    options.replace = replace;
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_create_scalar_index(dataset->path, column, type, options, v, e);
    });
}
LanceIndexSegmentBuilder* lance_index_segment_builder_new_scalar(const LanceDataset* dataset, const char* column,
                                                                 const char* index_name, int32_t index_type,
                                                                 const char* params_json,
                                                                 const LanceIndexSegmentBuildOptions* options) {
    return guarded<LanceIndexSegmentBuilder*>(nullptr, [&]() -> LanceIndexSegmentBuilder* {
        if (dataset == nullptr || column == nullptr) {
            invalid("dataset and column must not be NULL");
            return nullptr;
        }
        if (column[0] == '\0') {
            invalid("column must not be NULL or empty");
            return nullptr;
        }
        if (index_type < LANCE_SCALAR_BTREE || index_type > LANCE_SCALAR_INVERTED) {
            invalid("invalid LanceScalarIndexType " + std::to_string(index_type) +
                    "; expected 1 (BTREE), 2 (BITMAP), 3 (LABEL_LIST), or 4 (INVERTED)");
            return nullptr;
        }
        auto b = std::make_unique<LanceIndexSegmentBuilder>();
        b->scalar_type = index_type;
        if (index_type == LANCE_SCALAR_INVERTED) {
            std::string error;
            if (params_json != nullptr && params_json[0] != '\0' &&
                !nano_lance::fts::parse_params(params_json, b->inverted.params, error)) {
                set_error(error.find("not supported") != std::string::npos ? LANCE_ERR_NOT_SUPPORTED
                                                                           : LANCE_ERR_INVALID_ARGUMENT,
                          error);
                return nullptr;
            }
        } else if (params_json != nullptr && params_json[0] != '\0' && std::string(params_json) != "{}") {
            not_supported("scalar index parameters");
            return nullptr;
        }
        if (!segments::start_builder(*b, *dataset, column, index_name, options, true)) {
            return nullptr;
        }
        clear_error();
        return b.release();
    });
}

LanceIndexSegmentBuilder* lance_index_segment_builder_new_vector(const LanceDataset* dataset, const char* column,
                                                                 const char* index_name,
                                                                 const LanceVectorIndexSegmentParams* params,
                                                                 const LanceIndexSegmentBuildOptions* options) {
    return guarded<LanceIndexSegmentBuilder*>(nullptr, [&]() -> LanceIndexSegmentBuilder* {
        if (dataset == nullptr || column == nullptr || params == nullptr) {
            invalid("dataset, column, and params must not be NULL");
            return nullptr;
        }
        if (column[0] == '\0') {
            invalid("column must not be NULL or empty");
            return nullptr;
        }
        auto b = std::make_unique<LanceIndexSegmentBuilder>();
        b->is_vector = true;
        if (!segments::vector_options(*params, b->vector) ||
            !segments::start_builder(*b, *dataset, column, index_name, options, false)) {
            return nullptr;
        }
        std::size_t dim = 0;
        if (!segments::vector_dim(b->manifest, column, dim)) {
            return nullptr;
        }
        const bool pq = params->index_type == LANCE_INDEX_IVF_PQ;
        std::optional<segments::Model> centroids;
        std::optional<segments::Model> codebook;
        if (options != nullptr && options->ivf_centroids != nullptr) {
            centroids.emplace();
            if (!segments::borrow_model(options->ivf_centroids, options->ivf_centroids_schema, "ivf_centroids",
                                        *centroids)) {
                return nullptr;
            }
            const auto& p = centroids->provenance;
            if (p.kind != "ivf" || p.metric != params->metric || p.dimension != dim) {
                invalid("ivf_centroids provenance must be kind=ivf, metric=" + std::to_string(params->metric) +
                        ", dimension=" + std::to_string(dim) + "; got kind=" + p.kind +
                        ", metric=" + std::to_string(p.metric) + ", dimension=" + std::to_string(p.dimension));
                return nullptr;
            }
        }
        if (options != nullptr && options->pq_codebook != nullptr) {
            codebook.emplace();
            if (!segments::borrow_model(options->pq_codebook, options->pq_codebook_schema, "pq_codebook", *codebook)) {
                return nullptr;
            }
            const auto bits = params->num_bits == 0U ? 8U : params->num_bits;
            const auto& p = codebook->provenance;
            if (p.kind != "pq" || p.metric != params->metric || p.dimension != dim ||
                p.num_sub_vectors != std::optional<uint32_t>(params->num_sub_vectors) ||
                p.num_bits != std::optional<uint32_t>(bits)) {
                invalid("pq_codebook provenance does not match metric " + std::to_string(params->metric) +
                        ", dimension " + std::to_string(dim) + ", num_sub_vectors " +
                        std::to_string(params->num_sub_vectors) + ", num_bits " + std::to_string(bits));
                return nullptr;
            }
            if (!centroids || centroids->provenance.ivf_id != p.ivf_id) {
                invalid("pq_codebook was not trained with the supplied ivf_centroids");
                return nullptr;
            }
        }
        if (centroids) {
            if (centroids->list_size != dim) {
                invalid("ivf_centroids must have type FixedSizeList<Float32>[" + std::to_string(dim) + "], got list size " +
                        std::to_string(centroids->list_size));
                return nullptr;
            }
            if (centroids->rows != params->num_partitions) {
                invalid("ivf_centroids length " + std::to_string(centroids->rows) + " does not match num_partitions " +
                        std::to_string(params->num_partitions));
                return nullptr;
            }
        }
        if (!pq && codebook) {
            invalid("pq_codebook is not valid for vector index type " + b->vector.type);
            return nullptr;
        }
        if (pq && centroids.has_value() != codebook.has_value()) {
            invalid("precomputed PQ segment builds require both ivf_centroids and pq_codebook");
            return nullptr;
        }
        const bool has_models = centroids || codebook;
        if (b->mode == LANCE_INDEX_SEGMENT_BUILD_LOCAL_TRAIN && has_models) {
            invalid("mode LOCAL_TRAIN does not accept precomputed model arrays");
            return nullptr;
        }
        if (b->mode == LANCE_INDEX_SEGMENT_BUILD_PRECOMPUTED && !has_models) {
            invalid("mode PRECOMPUTED requires precomputed model arrays");
            return nullptr;
        }
        if (codebook) {
            const std::size_t m = params->num_sub_vectors;
            if (m == 0U || dim % m != 0U) {
                invalid("dimension " + std::to_string(dim) + " must be divisible by num_sub_vectors " + std::to_string(m));
                return nullptr;
            }
            const std::size_t rows = m << b->vector.num_bits;
            if (codebook->rows != rows || codebook->list_size != dim / m) {
                invalid("pq_codebook must be FixedSizeList<Float32> with length " + std::to_string(rows) +
                        " and list_size " + std::to_string(dim / m) + ", got length " +
                        std::to_string(codebook->rows) + ", list size " + std::to_string(codebook->list_size));
                return nullptr;
            }
            if (params->metric == LANCE_METRIC_DOT && b->fragments &&
                b->fragments->size() != b->manifest.fragments.size()) {
                invalid("pq_codebook is supplied for metric=DOT and an effective strict fragment subset (" +
                        std::to_string(b->fragments->size()) + " of " +
                        std::to_string(b->manifest.fragments.size()) +
                        " fragments): cover the full dataset in one segment (pass NULL fragment_ids or list every "
                        "fragment)");
                return nullptr;
            }
        }
        if (centroids) {
            auto& model = b->model.emplace();
            model.type = b->vector.type;
            model.metric = b->vector.metric;
            model.dim = dim;
            model.partitions = centroids->rows;
            model.centroids = std::move(centroids->values);
            model.nbits = b->vector.num_bits;
            model.m = pq ? params->num_sub_vectors : 0U;
            model.has_codebook = codebook.has_value();
            if (codebook) {
                model.codebook = std::move(codebook->values);
            }
            model.has_sq = false;
            model.hnsw_m = b->vector.hnsw_m;
            model.hnsw_ef_construction = b->vector.hnsw_ef_construction;
            model.hnsw_max_level = b->vector.hnsw_max_level;
        }
        clear_error();
        return b.release();
    });
}

int32_t lance_index_train_ivf_model(const LanceDataset* dataset, const char* column, uint32_t num_partitions,
                                    int32_t metric, const uint32_t* fragment_ids, size_t fragment_count,
                                    struct ArrowArray* out_array, struct ArrowSchema* out_schema) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (num_partitions == 0U) {
            invalid("num_partitions must be > 0, got 0");
            return -1;
        }
        segments::Trainer t;
        if (!segments::start_trainer(t, dataset, column, metric, fragment_ids, fragment_count, out_array,
                                     out_schema)) {
            return -1;
        }
        nano_lance::VectorIndexOptions o;
        o.type = "IVF_FLAT";
        o.metric = t.metric;
        o.num_partitions = num_partitions;
        nano_lance::index_build::VectorModel trained;
        if (!segments::train(t, o, nullptr, trained)) {
            return -1;
        }
        std::map<std::string, std::string> meta = {{segments::kKind, "ivf"},
                                                   {segments::kMetric, std::to_string(metric)},
                                                   {segments::kDimension, std::to_string(t.dim)},
                                                   {segments::kIvfId, segments::random_uuid_text()}};
        if (!segments::export_model(trained.centroids, t.dim, meta, out_array, out_schema)) {
            return -1;
        }
        clear_error();
        return 0;
    });
}

int32_t lance_index_train_pq_model(const LanceDataset* dataset, const char* column, uint32_t num_sub_vectors,
                                   uint32_t num_bits, int32_t metric, const uint32_t* fragment_ids,
                                   size_t fragment_count, struct ArrowArray* ivf_centroids,
                                   const struct ArrowSchema* ivf_centroids_schema, struct ArrowArray* out_array,
                                   struct ArrowSchema* out_schema) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (num_sub_vectors == 0U) {
            invalid("num_sub_vectors must be > 0, got 0");
            return -1;
        }
        if (num_bits != 4U && num_bits != 8U) {
            invalid("num_bits must be 4 or 8 for Lance PQ indexes, got " + std::to_string(num_bits));
            return -1;
        }
        segments::Trainer t;
        if (!segments::start_trainer(t, dataset, column, metric, fragment_ids, fragment_count, out_array,
                                     out_schema)) {
            return -1;
        }
        if (t.dim % num_sub_vectors != 0U) {
            invalid("dimension " + std::to_string(t.dim) + " must be divisible by num_sub_vectors " +
                    std::to_string(num_sub_vectors));
            return -1;
        }
        segments::Model centroids;
        if (!segments::borrow_model(ivf_centroids, ivf_centroids_schema, "ivf_centroids", centroids)) {
            return -1;
        }
        const auto& p = centroids.provenance;
        if (p.kind != "ivf" || p.metric != metric || p.dimension != t.dim) {
            invalid("ivf_centroids provenance must be kind=ivf, metric=" + std::to_string(metric) +
                    ", dimension=" + std::to_string(t.dim) + "; got kind=" + p.kind + ", metric=" +
                    std::to_string(p.metric) + ", dimension=" + std::to_string(p.dimension));
            return -1;
        }
        if (centroids.rows == 0U || centroids.list_size != t.dim) {
            invalid("ivf_centroids must be a non-empty FixedSizeList<Float32> with list_size " +
                    std::to_string(t.dim));
            return -1;
        }
        nano_lance::VectorIndexOptions o;
        o.type = "IVF_PQ";
        o.metric = t.metric;
        o.num_partitions = static_cast<uint32_t>(centroids.rows);
        o.num_sub_vectors = num_sub_vectors;
        o.num_bits = num_bits;
        nano_lance::index_build::VectorModel model;
        model.type = "IVF_PQ";
        model.metric = t.metric;
        model.dim = t.dim;
        model.partitions = centroids.rows;
        model.centroids = std::move(centroids.values);
        model.nbits = num_bits;
        model.m = num_sub_vectors;
        model.has_codebook = false;
        model.has_sq = false;
        nano_lance::index_build::VectorModel trained;
        if (!segments::train(t, o, &model, trained)) {
            return -1;
        }
        std::map<std::string, std::string> meta = {{segments::kKind, "pq"},
                                                   {segments::kMetric, std::to_string(metric)},
                                                   {segments::kDimension, std::to_string(t.dim)},
                                                   {segments::kIvfId, p.ivf_id},
                                                   {segments::kSubVectors, std::to_string(num_sub_vectors)},
                                                   {segments::kBits, std::to_string(num_bits)}};
        if (!segments::export_model(trained.codebook, t.dim / num_sub_vectors, meta, out_array, out_schema)) {
            return -1;
        }
        clear_error();
        return 0;
    });
}

int32_t lance_index_segment_builder_execute_uncommitted(LanceIndexSegmentBuilder* builder, uint8_t** out_bytes,
                                                        size_t* out_len) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (builder == nullptr) {
            invalid("builder must not be NULL");
            return -1;
        }
        if (builder->executed) {
            invalid("index segment builder is single-use and has already been executed");
            return -1;
        }
        builder->executed = true;
        if (out_bytes == nullptr || out_len == nullptr) {
            invalid("out_bytes and out_len must not be NULL");
            return -1;
        }
        std::vector<uint8_t> bytes;
        if (!segments::execute(*builder, bytes)) {
            return -1;
        }
        auto* buffer = static_cast<uint8_t*>(std::malloc(bytes.size()));
        if (buffer == nullptr) {
            set_error(LANCE_ERR_INTERNAL, "failed to allocate " + std::to_string(bytes.size()) + " metadata bytes");
            return -1;
        }
        std::memcpy(buffer, bytes.data(), bytes.size());
        *out_bytes = buffer;
        *out_len = bytes.size();
        clear_error();
        return 0;
    });
}

int32_t lance_index_segment_builder_set_progress_callback(LanceIndexSegmentBuilder* builder,
                                                          LanceIndexBuildProgressCallback callback,
                                                          void* callback_ctx) {
    if (builder == nullptr) {
        invalid("builder must not be NULL");
        return -1;
    }
    if (callback == nullptr) {
        invalid("progress callback must not be NULL");
        return -1;
    }
    if (builder->executed) {
        invalid("progress callback must be set before the builder is executed");
        return -1;
    }
    builder->callback = callback;
    builder->callback_ctx = callback_ctx;
    clear_error();
    return 0;
}

void lance_index_segment_builder_free(LanceIndexSegmentBuilder* builder) { delete builder; }

int32_t lance_index_segment_metadata_parse(const uint8_t* bytes, size_t len, LanceIndexSegmentMetadata** out_metadata) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (bytes == nullptr || len == 0U || out_metadata == nullptr) {
            invalid("bytes must be non-NULL, len must be > 0, and out_metadata must be non-NULL");
            return -1;
        }
        auto md = std::make_unique<LanceIndexSegmentMetadata>();
        std::string error;
        if (!segments::decode_metadata(bytes, len, md->index, error)) {
            invalid(error);
            return -1;
        }
        if (md->index.name.find('\0') != std::string::npos) {
            invalid("index metadata name contains an embedded NUL byte");
            return -1;
        }
        md->fragment_ids = md->index.fragment_ids;
        *out_metadata = md.release();
        clear_error();
        return 0;
    });
}

int32_t lance_index_segment_metadata_uuid(const LanceIndexSegmentMetadata* metadata, uint8_t* out_uuid) {
    if (metadata == nullptr || out_uuid == nullptr) {
        invalid("metadata and out_uuid must not be NULL");
        return -1;
    }
    std::memcpy(out_uuid, metadata->index.uuid.data(), 16);
    clear_error();
    return 0;
}

const char* lance_index_segment_metadata_name(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata is NULL");
        return nullptr;
    }
    clear_error();
    return metadata->index.name.c_str();
}

uint64_t lance_index_segment_metadata_dataset_version(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata is NULL");
        return 0;
    }
    clear_error();
    return metadata->index.dataset_version;
}

int32_t lance_index_segment_metadata_index_version(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata is NULL");
        return -1;
    }
    clear_error();
    return static_cast<int32_t>(metadata->index.index_version);
}

int32_t lance_index_segment_metadata_index_type(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata must not be NULL");
        return -1;
    }
    const auto& url = metadata->index.details_type_url;
    if (url.empty()) {
        invalid("index metadata does not contain index_details");
        return -1;
    }
    const int32_t type = segments::index_type_code(metadata->index);
    if (type < 0) {
        set_error(LANCE_ERR_NOT_SUPPORTED,
                  type == -2 ? "Rabit-quantized vector metadata has no LanceVectorIndexType value"
                             : "unsupported index_details type_url '" + url + "'");
        return -1;
    }
    clear_error();
    return type;
}

const char* lance_index_segment_metadata_index_details_type_url(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata is NULL");
        return nullptr;
    }
    if (metadata->index.details_type_url.empty()) {
        set_error(LANCE_ERR_NOT_FOUND, "index metadata does not contain index_details");
        return nullptr;
    }
    clear_error();
    return metadata->index.details_type_url.c_str();
}

size_t lance_index_segment_metadata_field_count(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata is NULL");
        return 0;
    }
    clear_error();
    return metadata->index.fields.size();
}

int32_t lance_index_segment_metadata_field_ids(const LanceIndexSegmentMetadata* metadata, int32_t* out_field_ids,
                                               size_t capacity, size_t* out_count) {
    if (metadata == nullptr || out_count == nullptr) {
        invalid("metadata and out_count must not be NULL");
        return -1;
    }
    const auto& ids = metadata->index.fields;
    if (capacity < ids.size()) {
        invalid("capacity " + std::to_string(capacity) + " is smaller than field_count " + std::to_string(ids.size()));
        return -1;
    }
    if (!ids.empty() && out_field_ids == nullptr) {
        invalid("out_field_ids is NULL but field_count is " + std::to_string(ids.size()));
        return -1;
    }
    std::copy(ids.begin(), ids.end(), out_field_ids);
    *out_count = ids.size();
    clear_error();
    return 0;
}

size_t lance_index_segment_metadata_fragment_count(const LanceIndexSegmentMetadata* metadata) {
    if (metadata == nullptr) {
        invalid("metadata is NULL");
        return 0;
    }
    clear_error();
    return metadata->fragment_ids.size();
}

int32_t lance_index_segment_metadata_fragment_ids(const LanceIndexSegmentMetadata* metadata,
                                                  uint32_t* out_fragment_ids, size_t capacity, size_t* out_count) {
    if (metadata == nullptr || out_count == nullptr) {
        invalid("metadata and out_count must not be NULL");
        return -1;
    }
    const auto& ids = metadata->fragment_ids;
    if (capacity < ids.size()) {
        invalid("capacity " + std::to_string(capacity) + " is smaller than fragment_count " +
                std::to_string(ids.size()));
        return -1;
    }
    if (!ids.empty() && out_fragment_ids == nullptr) {
        invalid("out_fragment_ids is NULL but fragment_count is " + std::to_string(ids.size()));
        return -1;
    }
    std::copy(ids.begin(), ids.end(), out_fragment_ids);
    *out_count = ids.size();
    clear_error();
    return 0;
}

void lance_index_segment_metadata_free(LanceIndexSegmentMetadata* metadata) { delete metadata; }

int32_t lance_dataset_commit_index_segments(LanceDataset* dataset, const char* index_name, const char* column,
                                            const uint8_t* const* segment_metadata_bytes,
                                            const size_t* segment_metadata_lens, size_t segment_count) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (dataset == nullptr || index_name == nullptr || column == nullptr) {
            invalid("dataset, index_name, and column must not be NULL");
            return -1;
        }
        if (index_name[0] == '\0') {
            invalid("index_name must not be NULL or empty");
            return -1;
        }
        if (column[0] == '\0') {
            invalid("column must not be NULL or empty");
            return -1;
        }
        if (segment_count == 0U) {
            invalid("segment_count must be > 0; at least one index segment is required to commit an index");
            return -1;
        }
        if (segment_metadata_bytes == nullptr || segment_metadata_lens == nullptr) {
            invalid("segment_metadata_bytes and segment_metadata_lens must not be NULL when segment_count is " +
                    std::to_string(segment_count));
            return -1;
        }
        std::vector<std::vector<uint8_t>> segment_set;
        for (size_t i = 0; i < segment_count; ++i) {
            const uint8_t* bytes = segment_metadata_bytes[i];
            const size_t len = segment_metadata_lens[i];
            if (bytes == nullptr || len == 0U) {
                invalid("segment_metadata_bytes[" + std::to_string(i) + "] must be non-NULL and segment_metadata_lens[" +
                        std::to_string(i) + "] must be > 0");
                return -1;
            }
            nano_lance::pb::IndexMetadata check;
            std::string error;
            if (!segments::decode_metadata(bytes, len, check, error)) {
                invalid("segment_metadata_bytes[" + std::to_string(i) + "]: " + error);
                return -1;
            }
            segment_set.emplace_back(bytes, bytes + len);
        }
        uint64_t version = 0;
        std::string error;
        nano_lance::SegmentErrorKind kind{};
        if (!nano_lance::dataset_commit_index_segments(dataset->path, index_name, column, segment_set, version, error,
                                                       kind)) {
            set_error(segments::code_of(kind, error), error);
            return -1;
        }
        if (!refresh(dataset, version)) {
            return -1;
        }
        clear_error();
        return 0;
    });
}
int32_t lance_dataset_drop_index(LanceDataset* dataset, const char* name) {
    if (dataset == nullptr || name == nullptr) {
        invalid("dataset and name must not be NULL");
        return -1;
    }
    return mutate(dataset, [&](uint64_t& v, std::string& e) {
        return nano_lance::dataset_drop_index(dataset->path, name, v, e);
    });
}
uint64_t lance_dataset_index_count(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return 0;
    }
    std::vector<nano_lance::IndexInfo> indices;
    std::string error;
    if (!nano_lance::dataset_list_indices(dataset->path, true, dataset->version, indices, error)) {
        fail(error);
        return 0;
    }
    std::set<std::string> names;
    for (const auto& i : indices) {
        names.insert(i.name);
    }
    clear_error();
    return names.size();
}
const char* lance_dataset_index_list_json(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return nullptr;
    }
    std::vector<nano_lance::IndexInfo> indices;
    std::string error;
    if (!nano_lance::dataset_list_indices(dataset->path, true, dataset->version, indices, error)) {
        fail(error);
        return nullptr;
    }
    const auto quote = [](const std::string& s) {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20U) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
        return out + "\"";
    };
    std::string json = "[";
    for (const auto& i : indices) {
        json += json.size() > 1 ? "," : "";
        json += "{\"name\":" + quote(i.name) + ",\"uuid\":" + quote(i.uuid) + ",\"type\":" + quote(i.type) +
                ",\"fields\":[";
        for (std::size_t k = 0; k < i.fields.size(); ++k) {
            json += (k > 0 ? "," : "") + quote(i.fields[k]);
        }
        json += "],\"fragment_ids\":[";
        for (std::size_t k = 0; k < i.fragment_ids.size(); ++k) {
            json += (k > 0 ? "," : "") + std::to_string(i.fragment_ids[k]);
        }
        json += "],\"dataset_version\":" + std::to_string(i.dataset_version) + "}";
    }
    json += "]";
    char* out = static_cast<char*>(std::malloc(json.size() + 1U));
    if (out == nullptr) {
        fail("out of memory");
        return nullptr;
    }
    std::memcpy(out, json.c_str(), json.size() + 1U);
    clear_error();
    return out;
}
uint64_t lance_dataset_index_segment_count(const LanceDataset* dataset, const char* index_name) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
        std::vector<std::array<uint8_t, 16>> uuids;
        if (!segments::list(dataset, index_name, uuids)) {
            return 0;
        }
        clear_error();
        return uuids.size();
    });
}
int32_t lance_dataset_index_segments(const LanceDataset* dataset, const char* index_name, uint8_t* out_uuids,
                                     size_t capacity, uint64_t* out_count) {
    return guarded<int32_t>(-1, [&]() -> int32_t {
        if (dataset == nullptr || index_name == nullptr || out_uuids == nullptr) {
            invalid("dataset, index_name, and out_uuids must not be NULL");
            return -1;
        }
        std::vector<std::array<uint8_t, 16>> uuids;
        if (!segments::list(dataset, index_name, uuids)) {
            return -1;
        }
        if (uuids.size() > capacity) {
            invalid("out_uuids capacity (" + std::to_string(capacity) + ") too small for " +
                    std::to_string(uuids.size()) + " segments");
            return -1;
        }
        for (size_t i = 0; i < uuids.size(); ++i) {
            std::memcpy(out_uuids + i * 16U, uuids[i].data(), 16);
        }
        if (out_count != nullptr) {
            *out_count = uuids.size();
        }
        clear_error();
        return 0;
    });
}

int32_t lance_scanner_nearest(LanceScanner* scanner, const char* column, const void* query_data, size_t query_len,
                              LanceDataType element_type, uint32_t k) {
    return before_scan(scanner, [&] {
        if (column == nullptr || column[0] == '\0' || query_data == nullptr || query_len == 0U) {
            return invalid("column and query_data must not be NULL or empty");
        }
        if (k == 0U) {
            return invalid("k must be greater than 0");
        }
        if (scanner->search.fts || scanner->search.fts_context != nullptr) {
            return invalid("cannot call nearest after full_text_search; they are mutually exclusive");
        }
        std::vector<float> key(query_len);
        switch (element_type) {
        case LANCE_DTYPE_FLOAT32:
            std::memcpy(key.data(), query_data, query_len * sizeof(float));
            break;
        case LANCE_DTYPE_FLOAT64:
            for (size_t i = 0; i < query_len; ++i) {
                key[i] = static_cast<float>(static_cast<const double*>(query_data)[i]);
            }
            break;
        case LANCE_DTYPE_FLOAT16:
            for (size_t i = 0; i < query_len; ++i) {
                key[i] = half_to_float(static_cast<const uint16_t*>(query_data)[i]);
            }
            break;
        case LANCE_DTYPE_UINT8:
            for (size_t i = 0; i < query_len; ++i) {
                key[i] = static_cast<float>(static_cast<const uint8_t*>(query_data)[i]);
            }
            break;
        case LANCE_DTYPE_INT8:
            for (size_t i = 0; i < query_len; ++i) {
                key[i] = static_cast<float>(static_cast<const int8_t*>(query_data)[i]);
            }
            break;
        default:
            return invalid("unknown element_type " + std::to_string(static_cast<int>(element_type)));
        }
        scanner->search.nearest = true;
        scanner->search.column = column;
        scanner->search.key = std::move(key);
        scanner->search.k = k;
        return true;
    });
}
int32_t lance_scanner_nearest_multivector(LanceScanner* scanner, const char*, const void*, size_t, size_t,
                                          LanceDataType, uint32_t) {
    return before_scan(scanner, [] {
        not_supported("multi-vector search");
        return false;
    });
}
int32_t lance_scanner_set_nprobes(LanceScanner* scanner, uint32_t nprobes) {
    return before_scan(scanner, [&] {
        if (nprobes == 0U) {
            return invalid("nprobes must be greater than 0, got 0");
        }
        scanner->search.minimum_nprobes = nprobes;
        scanner->search.maximum_nprobes = nprobes;
        return true;
    });
}
int32_t lance_scanner_set_minimum_nprobes(LanceScanner* scanner, uint32_t minimum_nprobes) {
    return before_scan(scanner, [&] {
        if (minimum_nprobes == 0U) {
            return invalid("minimum_nprobes must be greater than 0, got 0");
        }
        if (scanner->search.maximum_nprobes && minimum_nprobes > *scanner->search.maximum_nprobes) {
            return invalid("minimum_nprobes (" + std::to_string(minimum_nprobes) + ") must not exceed maximum_nprobes (" +
                           std::to_string(*scanner->search.maximum_nprobes) + ")");
        }
        scanner->search.minimum_nprobes = minimum_nprobes;
        return true;
    });
}
int32_t lance_scanner_set_maximum_nprobes(LanceScanner* scanner, uint32_t maximum_nprobes) {
    return before_scan(scanner, [&] {
        if (maximum_nprobes == 0U) {
            return invalid("maximum_nprobes must be greater than 0, got 0");
        }
        if (scanner->search.minimum_nprobes && maximum_nprobes < *scanner->search.minimum_nprobes) {
            return invalid("maximum_nprobes (" + std::to_string(maximum_nprobes) +
                           ") must not be less than minimum_nprobes (" +
                           std::to_string(*scanner->search.minimum_nprobes) + ")");
        }
        scanner->search.maximum_nprobes = maximum_nprobes;
        return true;
    });
}
// The approximation mode changes only binary-quantized searches in Lance; for the indexes nanolance
// searches it changes nothing, as in Lance.
int32_t lance_scanner_set_approx_mode(LanceScanner* scanner, LanceApproxMode approx_mode) {
    return before_scan(scanner, [&] {
        const auto mode = static_cast<int32_t>(approx_mode);
        return (mode >= 0 && mode <= 2) ||
               invalid("approx_mode must be 0 (FAST), 1 (NORMAL), or 2 (ACCURATE), got " + std::to_string(mode));
    });
}
int32_t lance_scanner_set_query_parallelism(LanceScanner* scanner, int32_t query_parallelism) {
    return before_scan(scanner, [&] {
        if (query_parallelism < -1) {
            return invalid("query_parallelism must be -1, 0, or greater than 0, got " +
                           std::to_string(query_parallelism));
        }
        scanner->search.query_parallelism = query_parallelism;
        return true;
    });
}
int32_t lance_scanner_set_refine_factor(LanceScanner* scanner, uint32_t f) {
    return before_scan(scanner, [&] {
        scanner->search.refine_factor = f;
        return true;
    });
}
int32_t lance_scanner_set_ef(LanceScanner* scanner, uint32_t e) {
    return before_scan(scanner, [&] {
        scanner->search.ef = e;
        return true;
    });
}
int32_t lance_scanner_set_metric(LanceScanner* scanner, LanceMetricType metric) {
    return before_scan(scanner, [&] {
        switch (metric) {
        case LANCE_METRIC_L2: scanner->search.metric = nano_lance::VectorMetric::L2; return true;
        case LANCE_METRIC_COSINE: scanner->search.metric = nano_lance::VectorMetric::Cosine; return true;
        case LANCE_METRIC_DOT: scanner->search.metric = nano_lance::VectorMetric::Dot; return true;
        case LANCE_METRIC_HAMMING: not_supported("the hamming metric"); return false;
        }
        return invalid("unknown metric " + std::to_string(static_cast<int>(metric)));
    });
}
int32_t lance_scanner_set_use_index(LanceScanner* scanner, bool enable) {
    return before_scan(scanner, [&] {
        scanner->search.use_index = enable;
        return true;
    });
}
int32_t lance_scanner_set_prefilter(LanceScanner* scanner, bool enable) {
    return before_scan(scanner, [&] {
        scanner->search.prefilter = enable;
        return true;
    });
}
int32_t lance_scanner_set_index_segments(LanceScanner* scanner, const uint8_t* segment_uuids, size_t len) {
    return before_scan(scanner, [&] {
        if (segment_uuids == nullptr && len > 0U) {
            return invalid("segment_uuids is NULL but len > 0");
        }
        scanner->search.index_segments.assign(len, {});
        for (size_t i = 0; i < len; ++i) {
            std::memcpy(scanner->search.index_segments[i].data(), segment_uuids + i * 16U, 16);
        }
        return true;
    });
}
int32_t lance_scanner_set_scalar_index_segment(LanceScanner* scanner, const uint8_t* segment_uuid) {
    return before_scan(scanner, [&] {
        if (segment_uuid == nullptr) {
            scanner->search.scalar_segment.reset();
            return true;
        }
        std::array<uint8_t, 16> uuid{};
        std::memcpy(uuid.data(), segment_uuid, 16);
        scanner->search.scalar_segment = uuid;
        return true;
    });
}

LanceFtsQueryContext* lance_dataset_prepare_fts_query(const LanceDataset* dataset, const char* column,
                                                      const char* query, uint32_t max_fuzzy_distance,
                                                      int32_t coverage_mode) {
    return lance_dataset_prepare_fts_match_query(dataset, column, query, LANCE_FTS_MATCH_OPERATOR_OR,
                                                 max_fuzzy_distance, coverage_mode);
}
LanceFtsQueryContext* lance_dataset_prepare_fts_match_query(const LanceDataset* dataset, const char* column,
                                                            const char* query, int32_t match_operator,
                                                            uint32_t max_fuzzy_distance, int32_t coverage_mode) {
    return guarded<LanceFtsQueryContext*>(nullptr, [&]() -> LanceFtsQueryContext* {
        if (match_operator != LANCE_FTS_MATCH_OPERATOR_OR && match_operator != LANCE_FTS_MATCH_OPERATOR_AND) {
            invalid("invalid match_operator " + std::to_string(match_operator) + "; expected 0 (OR) or 1 (AND)");
            return nullptr;
        }
        if (max_fuzzy_distance != 0U) {
            invalid("max_fuzzy_distance must be 0: prepared fuzzy matching is not available");
            return nullptr;
        }
        nano_lance::FtsQuery q;
        q.kind = nano_lance::FtsQuery::Kind::Match;
        q.and_operator = match_operator == LANCE_FTS_MATCH_OPERATOR_AND;
        return prepare_fts(dataset, column, query, coverage_mode, std::move(q), false);
    });
}
LanceFtsQueryContext* lance_dataset_prepare_fts_phrase_query(const LanceDataset* dataset, const char* column,
                                                             const char* query, int32_t slop,
                                                             int32_t coverage_mode) {
    return guarded<LanceFtsQueryContext*>(nullptr, [&]() -> LanceFtsQueryContext* {
        if (slop < 0) {
            invalid("slop must not be negative, got " + std::to_string(slop));
            return nullptr;
        }
        nano_lance::FtsQuery q;
        q.kind = nano_lance::FtsQuery::Kind::Phrase;
        q.slop = static_cast<uint32_t>(slop);
        return prepare_fts(dataset, column, query, coverage_mode, std::move(q), true);
    });
}
void lance_fts_query_context_close(LanceFtsQueryContext* context) { delete context; }
int32_t lance_scanner_full_text_search(LanceScanner* scanner, const char* query, const char* const* columns,
                                       uint32_t max_fuzzy_distance) {
    return before_scan(scanner, [&] {
        if (query == nullptr) {
            return invalid("scanner and query must not be NULL");
        }
        if (scanner->search.nearest) {
            return invalid("cannot call full_text_search after nearest; they are mutually exclusive");
        }
        if (scanner->search.fts_context != nullptr) {
            return invalid(
                "cannot call full_text_search after attaching an FTS query context; the context already owns the query");
        }
        if (max_fuzzy_distance > 0U) {
            not_supported("fuzzy full-text matching (max_fuzzy_distance > 0)");
            return false;
        }
        // A query string over the columns given, or every column with an INVERTED index: Lance's
        // FullTextSearchQuery::new(query).with_columns(columns).
        nano_lance::FtsQuery q;
        q.kind = nano_lance::FtsQuery::Kind::MultiMatch;
        q.text = query;
        q.columns = column_list(columns);
        scanner->search.fts = std::move(q);
        return true;
    });
}
int32_t lance_scanner_set_fts_query_context(LanceScanner* scanner, const LanceFtsQueryContext* context) {
    return before_scan(scanner, [&] {
        if (context == nullptr || context->data == nullptr) {
            return invalid("context must not be NULL");
        }
        if (scanner->search.nearest) {
            return invalid("cannot attach an FTS query context after nearest; they are mutually exclusive");
        }
        if (scanner->search.fts) {
            return invalid("cannot attach an FTS query context after full_text_search; the context owns the query");
        }
        if (context->data->origin != scanner->origin || context->data->version != scanner->version) {
            return invalid(
                "the FTS query context was prepared on a different dataset snapshot than the scanner's");
        }
        if (scanner->has_fragments) {
            return invalid("fragment_ids cannot be combined with an FTS query context; split the query by FTS index "
                           "segment UUID instead");
        }
        scanner->search.fts_context = context->data;
        return true;
    });
}
int32_t lance_scanner_set_fts_index_segments(LanceScanner* scanner, const uint8_t* segment_uuids, size_t len) {
    return before_scan(scanner, [&] {
        if (segment_uuids == nullptr && len > 0U) {
            return invalid("segment_uuids is NULL but len is greater than 0");
        }
        std::vector<std::array<uint8_t, 16>> uuids(len);
        for (size_t i = 0; i < len; ++i) {
            std::memcpy(uuids[i].data(), segment_uuids + i * 16U, 16);
        }
        const std::set<std::array<uint8_t, 16>> unique(uuids.begin(), uuids.end());
        if (unique.size() != uuids.size()) {
            return invalid("segment_uuids contains duplicate UUIDs; len=" + std::to_string(uuids.size()) +
                           ", unique=" + std::to_string(unique.size()));
        }
        scanner->search.fts_index_segments = std::move(uuids);  // empty: every segment of the context
        return true;
    });
}

}  // extern "C"
