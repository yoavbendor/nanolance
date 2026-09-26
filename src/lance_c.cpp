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

#include "nanolance/dataset.hpp"
#include "nanolance/data_file_reader.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/path_safety.hpp"
#include "nanolance/work_stats.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

struct LanceScanner {
    std::filesystem::path path;
    uint64_t version = 0;
    bool has_columns = false;
    std::vector<std::string> columns;
    int64_t limit = -1;
    int64_t offset = 0;
    int64_t batch_size = 0;
    bool with_row_id = false;
    bool with_row_address = false;
    bool has_fragments = false;
    std::vector<uint64_t> fragment_ids;
    int32_t blob_handling = LANCE_BLOB_HANDLING_BLOBS_DESCRIPTIONS;
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

struct LanceBlobFile {};
struct LanceIndexSegmentBuilder {};
struct LanceIndexSegmentMetadata {};
struct LanceFtsQueryContext {};

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
    if (has("already exists")) {
        return LANCE_ERR_DATASET_ALREADY_EXISTS;
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

// ── batches that share one decoded batch ────────────────────────────────────────────────────────
//
// A slice of a batch (for batch_size, and for take's input order) is a new struct array whose
// children are views of the decoded batch's children at an offset. Every view holds a reference to
// the decoded batch, so any of them can be released in any order, children included.

struct SharedBatch {
    ArrowArray array{};
    ~SharedBatch() {
        if (array.release != nullptr) {
            array.release(&array);
        }
    }
};

struct ViewPrivate {
    std::shared_ptr<SharedBatch> base;
    std::vector<ArrowArray*> children;
    std::vector<const void*> buffers;
};

void release_child_view(ArrowArray* array) {
    delete static_cast<ViewPrivate*>(array->private_data);
    array->release = nullptr;
}

void release_view(ArrowArray* array) {
    auto* priv = static_cast<ViewPrivate*>(array->private_data);
    for (auto* child : priv->children) {
        if (child->release != nullptr) {
            child->release(child);
        }
        delete child;
    }
    delete priv;
    array->release = nullptr;
}

/// A view of rows [offset, offset + length) of the struct batch `base`, children in `order`.
ArrowArray view_of(const std::shared_ptr<SharedBatch>& base, int64_t offset, int64_t length,
                   const std::vector<int64_t>& order) {
    const ArrowArray& b = base->array;
    auto* priv = new ViewPrivate{base, {}, {}};
    ArrowArray out{};
    out.length = length;
    out.null_count = 0;
    out.offset = 0;
    out.n_buffers = 1;
    priv->buffers.assign(1, nullptr);  // no top-level validity: a record batch has no null rows
    out.buffers = priv->buffers.data();
    for (const auto index : order) {
        const ArrowArray* src = b.children[index];
        auto* child = new ArrowArray(*src);  // shares the buffers and grandchildren
        child->offset = src->offset + b.offset + offset;
        child->length = length;
        child->null_count = src->null_count == 0 ? 0 : -1;
        child->private_data = new ViewPrivate{base, {}, {}};
        child->release = &release_child_view;
        priv->children.push_back(child);
    }
    out.n_children = static_cast<int64_t>(priv->children.size());
    out.children = priv->children.data();
    out.dictionary = nullptr;
    out.private_data = priv;
    out.release = &release_view;
    return out;
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

bool open_scan(const LanceScanner& scanner, ArrowArrayStream& out) {
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
    auto& c = nano_lance::work_stats::counters();
    self->bytes_at_open = c.data_bytes_read.load(std::memory_order_relaxed);
    self->reads_at_open = c.data_reads.load(std::memory_order_relaxed);
    std::string error;
    // An offset past the end reads nothing, as in Lance (nanolance's reader calls it an error).
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
    request.range.offset = std::min<uint64_t>(request.range.offset, rows);
    if (!nano_lance::LanceTableStream::open_request(scanner.path, request, self->decoded_schema, self->stream, error)) {
        fail(error);
        return false;
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
        auto s = std::make_shared<SharedBatch>();
        s->array = b;
        b.release = nullptr;
        shared.push_back(std::move(s));
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
        // What lance-c records on create: every 20 versions, versions older than 14 days may be
        // reclaimed. nanolance reclaims nothing itself; Lance honors it when it next commits.
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
    if (filter != nullptr && filter[0] != '\0') {
        not_supported("filters (yet)");
        return nullptr;
    }
    auto scanner = std::make_unique<LanceScanner>();
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
int32_t lance_scanner_set_use_scalar_index(LanceScanner* scanner, bool) {
    return before_scan(scanner, [] { return true; });
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
        if (include_deleted_rows) {
            not_supported("include_deleted_rows");
            return false;
        }
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
    not_supported("filters (yet)");
    return -1;
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
    copy->limit = scanner->limit;
    copy->offset = scanner->offset;
    copy->batch_size = scanner->batch_size;
    copy->with_row_id = scanner->with_row_id;
    copy->with_row_address = scanner->with_row_address;
    copy->has_fragments = scanner->has_fragments;
    copy->fragment_ids = scanner->fragment_ids;
    copy->stats_callback = scanner->stats_callback;
    copy->stats_ctx = scanner->stats_ctx;
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

int32_t lance_dataset_delete(LanceDataset*, const char*, uint64_t*) { NL_UNSUPPORTED_INT("lance_dataset_delete (yet)"); }
int32_t lance_dataset_update(LanceDataset*, const char*, const char* const*, const char* const*, size_t, uint64_t*) {
    NL_UNSUPPORTED_INT("lance_dataset_update (yet)");
}
int32_t lance_dataset_merge_insert(LanceDataset*, const char* const*, size_t, struct ArrowArrayStream* source,
                                   const LanceMergeInsertParams*, LanceMergeInsertResult*) {
    if (source != nullptr && source->release != nullptr) {
        source->release(source);
    }
    NL_UNSUPPORTED_INT("lance_dataset_merge_insert (yet)");
}
int32_t lance_dataset_compact_files(LanceDataset*, const LanceCompactionOptions*, LanceCompactionMetrics*) {
    NL_UNSUPPORTED_INT("lance_dataset_compact_files (yet)");
}
int32_t lance_dataset_drop_columns(LanceDataset*, const char* const*, size_t) {
    NL_UNSUPPORTED_INT("lance_dataset_drop_columns (yet)");
}
int32_t lance_dataset_alter_columns(LanceDataset*, const LanceColumnAlteration*, size_t) {
    NL_UNSUPPORTED_INT("lance_dataset_alter_columns (yet)");
}
int32_t lance_dataset_add_columns_sql(LanceDataset*, const LanceSqlColumn*, size_t, uint64_t) {
    NL_UNSUPPORTED_INT("lance_dataset_add_columns_sql (yet)");
}
int32_t lance_dataset_add_columns_nulls(LanceDataset*, const struct ArrowSchema*) {
    NL_UNSUPPORTED_INT("lance_dataset_add_columns_nulls (yet)");
}
int32_t lance_dataset_add_columns_stream(LanceDataset*, struct ArrowArrayStream* stream, uint64_t) {
    if (stream != nullptr && stream->release != nullptr) {
        stream->release(stream);
    }
    NL_UNSUPPORTED_INT("lance_dataset_add_columns_stream (yet)");
}

int32_t lance_dataset_take_blobs(const LanceDataset*, const uint64_t*, size_t, const char*, LanceBlobFile**) {
    NL_UNSUPPORTED_INT("blob files");
}
int32_t lance_dataset_take_blobs_by_indices(const LanceDataset*, const uint64_t*, size_t, const char*,
                                            LanceBlobFile**) {
    NL_UNSUPPORTED_INT("blob files");
}
uint64_t lance_blob_file_size(const LanceBlobFile*) {
    not_supported("blob files");
    return 0;
}
int32_t lance_blob_file_read(LanceBlobFile*, uint8_t*, size_t) { NL_UNSUPPORTED_INT("blob files"); }
int32_t lance_blob_file_read_up_to(LanceBlobFile*, uint8_t*, size_t, size_t*) { NL_UNSUPPORTED_INT("blob files"); }
int32_t lance_blob_file_read_range(const LanceBlobFile*, uint64_t, uint8_t*, size_t) {
    NL_UNSUPPORTED_INT("blob files");
}
int32_t lance_blob_file_seek(LanceBlobFile*, uint64_t) { NL_UNSUPPORTED_INT("blob files"); }
int32_t lance_blob_file_tell(const LanceBlobFile*, uint64_t*) { NL_UNSUPPORTED_INT("blob files"); }
void lance_blob_file_close(LanceBlobFile* blob) { delete blob; }

int32_t lance_dataset_create_vector_index(LanceDataset*, const char*, const char*, const LanceVectorIndexParams*,
                                          bool) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_dataset_create_scalar_index(LanceDataset*, const char*, const char*, LanceScalarIndexType, const char*,
                                          bool) {
    NL_UNSUPPORTED_INT("indexes");
}
LanceIndexSegmentBuilder* lance_index_segment_builder_new_scalar(const LanceDataset*, const char*, const char*,
                                                                 int32_t, const char*,
                                                                 const LanceIndexSegmentBuildOptions*) {
    not_supported("indexes");
    return nullptr;
}
LanceIndexSegmentBuilder* lance_index_segment_builder_new_vector(const LanceDataset*, const char*, const char*,
                                                                 const LanceVectorIndexSegmentParams*,
                                                                 const LanceIndexSegmentBuildOptions*) {
    not_supported("indexes");
    return nullptr;
}
int32_t lance_index_train_ivf_model(const LanceDataset*, const char*, uint32_t, int32_t, const uint32_t*, size_t,
                                    struct ArrowArray*, struct ArrowSchema*) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_index_train_pq_model(const LanceDataset*, const char*, uint32_t, uint32_t, int32_t, const uint32_t*,
                                   size_t, struct ArrowArray*, const struct ArrowSchema*, struct ArrowArray*,
                                   struct ArrowSchema*) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_index_segment_builder_execute_uncommitted(LanceIndexSegmentBuilder*, uint8_t**, size_t*) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_index_segment_builder_set_progress_callback(LanceIndexSegmentBuilder*,
                                                          LanceIndexBuildProgressCallback, void*) {
    NL_UNSUPPORTED_INT("indexes");
}
void lance_index_segment_builder_free(LanceIndexSegmentBuilder* builder) { delete builder; }
int32_t lance_index_segment_metadata_parse(const uint8_t*, size_t, LanceIndexSegmentMetadata**) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_index_segment_metadata_uuid(const LanceIndexSegmentMetadata*, uint8_t*) {
    NL_UNSUPPORTED_INT("indexes");
}
const char* lance_index_segment_metadata_name(const LanceIndexSegmentMetadata*) {
    not_supported("indexes");
    return nullptr;
}
uint64_t lance_index_segment_metadata_dataset_version(const LanceIndexSegmentMetadata*) {
    not_supported("indexes");
    return 0;
}
int32_t lance_index_segment_metadata_index_version(const LanceIndexSegmentMetadata*) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_index_segment_metadata_index_type(const LanceIndexSegmentMetadata*) { NL_UNSUPPORTED_INT("indexes"); }
const char* lance_index_segment_metadata_index_details_type_url(const LanceIndexSegmentMetadata*) {
    not_supported("indexes");
    return nullptr;
}
size_t lance_index_segment_metadata_field_count(const LanceIndexSegmentMetadata*) {
    not_supported("indexes");
    return 0;
}
int32_t lance_index_segment_metadata_field_ids(const LanceIndexSegmentMetadata*, int32_t*, size_t, size_t*) {
    NL_UNSUPPORTED_INT("indexes");
}
size_t lance_index_segment_metadata_fragment_count(const LanceIndexSegmentMetadata*) {
    not_supported("indexes");
    return 0;
}
int32_t lance_index_segment_metadata_fragment_ids(const LanceIndexSegmentMetadata*, uint32_t*, size_t, size_t*) {
    NL_UNSUPPORTED_INT("indexes");
}
void lance_index_segment_metadata_free(LanceIndexSegmentMetadata* metadata) { delete metadata; }
int32_t lance_dataset_commit_index_segments(LanceDataset*, const char*, const char*, const uint8_t* const*,
                                            const size_t*, size_t) {
    NL_UNSUPPORTED_INT("indexes");
}
int32_t lance_dataset_drop_index(LanceDataset*, const char*) { NL_UNSUPPORTED_INT("indexes"); }
uint64_t lance_dataset_index_count(const LanceDataset* dataset) {
    // A dataset nanolance opens has no indexes it can use.
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return 0;
    }
    clear_error();
    return 0;
}
const char* lance_dataset_index_list_json(const LanceDataset* dataset) {
    if (dataset == nullptr) {
        invalid("dataset must not be NULL");
        return nullptr;
    }
    clear_error();
    char* out = static_cast<char*>(std::malloc(3));
    if (out != nullptr) {
        std::memcpy(out, "[]", 3);
    }
    return out;
}
uint64_t lance_dataset_index_segment_count(const LanceDataset*, const char*) {
    not_supported("indexes");
    return 0;
}
int32_t lance_dataset_index_segments(const LanceDataset*, const char*, uint8_t*, size_t, uint64_t*) {
    NL_UNSUPPORTED_INT("indexes");
}

int32_t lance_scanner_nearest(LanceScanner*, const char*, const void*, size_t, LanceDataType, uint32_t) {
    NL_UNSUPPORTED_INT("vector search");
}
int32_t lance_scanner_nearest_multivector(LanceScanner*, const char*, const void*, size_t, size_t, LanceDataType,
                                          uint32_t) {
    NL_UNSUPPORTED_INT("vector search");
}
int32_t lance_scanner_set_nprobes(LanceScanner*, uint32_t) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_minimum_nprobes(LanceScanner*, uint32_t) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_maximum_nprobes(LanceScanner*, uint32_t) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_approx_mode(LanceScanner*, LanceApproxMode) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_query_parallelism(LanceScanner*, int32_t) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_refine_factor(LanceScanner*, uint32_t) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_ef(LanceScanner*, uint32_t) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_metric(LanceScanner*, LanceMetricType) { NL_UNSUPPORTED_INT("vector search"); }
int32_t lance_scanner_set_use_index(LanceScanner* scanner, bool) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_prefilter(LanceScanner* scanner, bool) {
    return before_scan(scanner, [] { return true; });
}
int32_t lance_scanner_set_index_segments(LanceScanner*, const uint8_t*, size_t) { NL_UNSUPPORTED_INT("indexes"); }
int32_t lance_scanner_set_scalar_index_segment(LanceScanner*, const uint8_t*) { NL_UNSUPPORTED_INT("indexes"); }

LanceFtsQueryContext* lance_dataset_prepare_fts_query(const LanceDataset*, const char*, const char*, uint32_t,
                                                      int32_t) {
    not_supported("full-text search");
    return nullptr;
}
LanceFtsQueryContext* lance_dataset_prepare_fts_match_query(const LanceDataset*, const char*, const char*, int32_t,
                                                            uint32_t, int32_t) {
    not_supported("full-text search");
    return nullptr;
}
LanceFtsQueryContext* lance_dataset_prepare_fts_phrase_query(const LanceDataset*, const char*, const char*, int32_t,
                                                             int32_t) {
    not_supported("full-text search");
    return nullptr;
}
void lance_fts_query_context_close(LanceFtsQueryContext* context) { delete context; }
int32_t lance_scanner_full_text_search(LanceScanner*, const char*, const char* const*, uint32_t) {
    NL_UNSUPPORTED_INT("full-text search");
}
int32_t lance_scanner_set_fts_query_context(LanceScanner*, const LanceFtsQueryContext*) {
    NL_UNSUPPORTED_INT("full-text search");
}
int32_t lance_scanner_set_fts_index_segments(LanceScanner*, const uint8_t*, size_t) {
    NL_UNSUPPORTED_INT("full-text search");
}

}  // extern "C"
