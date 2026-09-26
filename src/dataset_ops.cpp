// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Dataset changes (dataset_ops.hpp), each one version: read what the change needs through the scan
// machinery (filters, row addresses), write new data through the staged writer, and commit the
// latest manifest with the changed fragments and schema.

#include "nanolance/dataset_ops.hpp"

#include "nanolance/arrow_slice.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"
#include "nanolance/writer_internal.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace nano_lance {
namespace {

// ── Arrow helpers ───────────────────────────────────────────────────────────────────────────────

struct OwnedSchema {
    ArrowSchema s{};
    ~OwnedSchema() {
        if (s.release != nullptr) {
            s.release(&s);
        }
    }
};

struct OwnedBatches {
    std::vector<ArrowArray> v;
    ~OwnedBatches() {
        for (auto& a : v) {
            if (a.release != nullptr) {
                a.release(&a);
            }
        }
    }
};

int64_t child_index(const ArrowSchema& schema, const std::string& name) {
    for (int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name != nullptr && name == schema.children[i]->name) {
            return i;
        }
    }
    return -1;
}

/// A struct array of `children` (moved in), `length` rows.
bool make_struct(std::vector<ArrowArray>& children, int64_t length, ArrowArray& out, std::string& error) {
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_STRUCT) != NANOARROW_OK ||
        ArrowArrayAllocateChildren(&out, static_cast<int64_t>(children.size())) != NANOARROW_OK) {
        if (out.release != nullptr) {
            out.release(&out);
        }
        error = "failed to build a batch";
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        ArrowArrayMove(&children[i], out.children[i]);
    }
    out.length = length;
    out.null_count = 0;
    return true;
}

/// A struct schema of copies of `fields`.
bool make_struct_schema(const std::vector<const ArrowSchema*>& fields, ArrowSchema& out, std::string& error) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, static_cast<int64_t>(fields.size())) != NANOARROW_OK) {
        out.release(&out);
        error = "failed to build a schema";
        return false;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out.children[i]->release(out.children[i]);
        if (ArrowSchemaDeepCopy(fields[i], out.children[i]) != NANOARROW_OK) {
            out.release(&out);
            error = "failed to copy a field";
            return false;
        }
    }
    return true;
}

// ── reading ─────────────────────────────────────────────────────────────────────────────────────

bool scan(const std::filesystem::path& path, const LanceScanRequest& request, OwnedSchema& schema, OwnedBatches& batches,
          std::string& error) {
    return lance_dataset_scan(path, request, schema.s, batches.v, error);
}

/// The physical offsets, by fragment, of the rows of version `version` where `predicate` is TRUE
/// (every row when null).
bool matching_rows(const std::filesystem::path& path, std::uint64_t version, const std::string* predicate,
                   std::map<std::uint64_t, std::vector<std::uint32_t>>& out, std::uint64_t& count, std::string& error) {
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    const std::vector<std::string> none;
    request.columns = &none;
    request.with_row_address = true;
    request.filter = predicate;
    OwnedSchema schema;
    OwnedBatches batches;
    if (!scan(path, request, schema, batches, error)) {
        return false;
    }
    count = 0;
    for (const auto& b : batches.v) {
        const ArrowArray* addr = b.children[b.n_children - 1];
        const auto* values = static_cast<const std::uint64_t*>(addr->buffers[1]) + addr->offset;
        for (int64_t i = 0; i < b.length; ++i) {
            out[values[i] >> 32U].push_back(static_cast<std::uint32_t>(values[i] & 0xFFFFFFFFULL));
        }
        count += static_cast<std::uint64_t>(b.length);
    }
    return true;
}

bool load_latest(const std::filesystem::path& path, pb::Manifest& manifest, std::uint64_t& version, std::string& error) {
    return load_latest_manifest(path, manifest, version, error);
}

/// Mark `deleted` rows of `manifest`'s fragments deleted: each touched fragment gets a new deletion
/// file with its old deletions and the new ones; a fragment left with no rows is removed.
bool apply_deletions(const std::filesystem::path& path, pb::Manifest& manifest, std::uint64_t read_version,
                     const std::map<std::uint64_t, std::vector<std::uint32_t>>& deleted, std::string& error) {
    std::vector<pb::DataFragment> kept;
    bool any_deletion_file = false;
    for (auto& fragment : manifest.fragments) {
        const auto it = deleted.find(fragment.id);
        if (it != deleted.end() && !it->second.empty()) {
            std::vector<std::uint32_t> rows;
            if (!read_deletion_vector(path, fragment.id, fragment.deletion_file, rows, error)) {
                return false;
            }
            rows.insert(rows.end(), it->second.begin(), it->second.end());
            std::sort(rows.begin(), rows.end());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            if (rows.size() >= fragment.physical_rows) {
                continue;  // every row deleted: the fragment goes
            }
            if (!write_deletion_file(path, fragment.id, read_version, rows, fragment.deletion_file, error)) {
                return false;
            }
        }
        any_deletion_file = any_deletion_file || fragment.deletion_file.present;
        kept.push_back(std::move(fragment));
    }
    manifest.fragments = std::move(kept);
    if (any_deletion_file) {
        manifest.reader_feature_flags |= pb::kFlagDeletionFiles;
        manifest.writer_feature_flags |= pb::kFlagDeletionFiles;
    }
    return true;
}

std::uint64_t next_fragment_id(const pb::Manifest& manifest) {
    std::uint64_t next = manifest.has_max_fragment_id ? static_cast<std::uint64_t>(manifest.max_fragment_id) + 1U : 0U;
    for (const auto& f : manifest.fragments) {
        next = std::max(next, f.id + 1U);
    }
    return next;
}

void note_fragment_id(pb::Manifest& manifest, std::uint64_t id) {
    if (!manifest.has_max_fragment_id || id > manifest.max_fragment_id) {
        manifest.has_max_fragment_id = true;
        manifest.max_fragment_id = static_cast<std::uint32_t>(id);
    }
}

// ── writing ─────────────────────────────────────────────────────────────────────────────────────

/// A staged writer: data files under the dataset, no manifest; the caller commits them.
class StagedFiles {
public:
    ~StagedFiles() {
        if (open_) {
            nano_lance_writer_close(&writer_);
        }
    }

    /// `append`: rows of the dataset's own schema. Otherwise new columns, numbered from `field_id_base`.
    bool open(const std::filesystem::path& path, bool append, std::int32_t field_id_base, std::string& error) {
        NanoLanceWriteOptions options{};
        options.append = append;
        options.stage_fragments = true;
        if (nano_lance_writer_open(&writer_, path.string().c_str(), &options) != NANO_LANCE_OK) {
            error = writer_.last_error;
            return false;
        }
        open_ = true;
        nano_lance_writer_set_ignore_nullability(&writer_, true);
        return append || writer_set_field_id_base(&writer_, field_id_base, error);
    }

    bool write(ArrowArray& batch, ArrowSchema& schema, std::string& error) {
        if (nano_lance_write_batch(&writer_, &batch, &schema) != NANO_LANCE_OK) {
            error = writer_.last_error;
            return false;
        }
        rows_ += static_cast<std::uint64_t>(batch.length);
        return true;
    }

    /// End the current data file (the next write starts another).
    bool cut(std::string& error, bool keep_empty = false) {
        if (rows_ == 0U && !keep_empty) {
            return true;
        }
        std::vector<NewFragment> files;
        if (!writer_take_staged(&writer_, files, mapping_, error, keep_empty)) {
            return false;
        }
        for (auto& f : files) {
            files_.push_back(std::move(f));
        }
        rows_ = 0;
        return true;
    }

    bool finish(std::string& error) { return cut(error); }

    std::vector<NewFragment>& files() { return files_; }
    const LanceSchemaMapping& mapping() const { return mapping_; }

private:
    NanoLanceWriter writer_{};
    bool open_ = false;
    std::uint64_t rows_ = 0;
    std::vector<NewFragment> files_;
    LanceSchemaMapping mapping_;
};

/// Append `files` (rows of the dataset's schema, written with `mapping`) as new fragments.
void add_fragments(pb::Manifest& manifest, const LanceSchemaMapping& mapping, const std::vector<NewFragment>& files) {
    auto id = next_fragment_id(manifest);
    for (const auto& f : files) {
        if (f.rows == 0U) {
            continue;
        }
        manifest.fragments.push_back(make_data_fragment(mapping, f, id));
        note_fragment_id(manifest, id);
        ++id;
    }
}

bool commit(const std::filesystem::path& path, pb::Manifest manifest, std::uint64_t& new_version, std::string& error) {
    return commit_next_version(path, std::move(manifest), new_version, error);
}

// ── keys (merge insert) ─────────────────────────────────────────────────────────────────────────

/// A row's key over `columns` of a viewed batch, as bytes that compare equal exactly when the values
/// are equal (integers of any width and signedness alike). False when a key value is null.
bool key_of(const ArrowArrayView& batch, const std::vector<int64_t>& columns, int64_t row, std::string& key) {
    key.clear();
    for (const auto c : columns) {
        const ArrowArrayView* v = batch.children[c];
        const int64_t at = row + batch.offset;
        if (ArrowArrayViewIsNull(v, at)) {
            return false;
        }
        char tag = 0;
        switch (v->storage_type) {
            case NANOARROW_TYPE_INT8:
            case NANOARROW_TYPE_INT16:
            case NANOARROW_TYPE_INT32:
            case NANOARROW_TYPE_INT64:
            case NANOARROW_TYPE_DATE32:
            case NANOARROW_TYPE_DATE64:
            case NANOARROW_TYPE_TIMESTAMP:
            case NANOARROW_TYPE_BOOL: {
                const std::int64_t x = ArrowArrayViewGetIntUnsafe(v, at);
                tag = 'i';
                key += tag;
                key.append(reinterpret_cast<const char*>(&x), sizeof(x));
                break;
            }
            case NANOARROW_TYPE_UINT8:
            case NANOARROW_TYPE_UINT16:
            case NANOARROW_TYPE_UINT32:
            case NANOARROW_TYPE_UINT64: {
                const std::uint64_t x = ArrowArrayViewGetUIntUnsafe(v, at);
                key += x <= static_cast<std::uint64_t>(INT64_MAX) ? 'i' : 'u';
                key.append(reinterpret_cast<const char*>(&x), sizeof(x));
                break;
            }
            case NANOARROW_TYPE_FLOAT:
            case NANOARROW_TYPE_DOUBLE:
            case NANOARROW_TYPE_HALF_FLOAT: {
                const double x = ArrowArrayViewGetDoubleUnsafe(v, at);
                key += 'f';
                key.append(reinterpret_cast<const char*>(&x), sizeof(x));
                break;
            }
            default: {
                const auto bv = ArrowArrayViewGetBytesUnsafe(v, at);
                const auto n = static_cast<std::uint64_t>(bv.size_bytes);
                key += 's';
                key.append(reinterpret_cast<const char*>(&n), sizeof(n));
                key.append(static_cast<const char*>(bv.data.data), static_cast<std::size_t>(n));
            }
        }
    }
    return true;
}

struct BatchView {
    ArrowArrayView view{};
    bool init = false;
    ~BatchView() {
        if (init) {
            ArrowArrayViewReset(&view);
        }
    }
    bool set(const ArrowSchema& schema, const ArrowArray& array, std::string& error) {
        ArrowError aerr{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &aerr) != NANOARROW_OK) {
            error = aerr.message;
            return false;
        }
        init = true;
        if (ArrowArrayViewSetArray(&view, &array, &aerr) != NANOARROW_OK) {
            error = aerr.message;
            return false;
        }
        return true;
    }
};

// ── new columns ─────────────────────────────────────────────────────────────────────────────────

std::int32_t max_field_id(const pb::Manifest& manifest) {
    std::int32_t max_id = -1;
    for (const auto& f : manifest.fields) {
        max_id = std::max(max_id, f.id);
    }
    return max_id;
}

/// Add the files `staged` wrote -- one per fragment of `fragments`, in order, new columns with ids from
/// the writer's mapping -- to those fragments, and their fields to the schema.
bool attach_columns(pb::Manifest& manifest, const std::vector<std::uint64_t>& fragments, StagedFiles& staged,
                    std::string& error) {
    auto& files = staged.files();
    if (files.size() != fragments.size()) {
        error = "wrote " + std::to_string(files.size()) + " column files for " + std::to_string(fragments.size()) +
                " fragments";
        return false;
    }
    const auto& mapping = staged.mapping();
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        auto it = std::find_if(manifest.fragments.begin(), manifest.fragments.end(),
                               [&](const pb::DataFragment& f) { return f.id == fragments[i]; });
        if (it == manifest.fragments.end()) {
            error = "fragment " + std::to_string(fragments[i]) + " disappeared";
            return false;
        }
        if (files[i].rows != it->physical_rows) {
            error = "new columns have " + std::to_string(files[i].rows) + " rows for a fragment of " +
                    std::to_string(it->physical_rows);
            return false;
        }
        auto fragment = make_data_fragment(mapping, files[i], it->id);
        it->files.push_back(std::move(fragment.files.front()));
    }
    for (const auto& f : mapping.fields) {
        manifest.fields.push_back(make_manifest_field(f));
    }
    return true;
}

bool check_new_names(const pb::Manifest& manifest, const std::vector<std::string>& names, std::string& error) {
    std::set<std::string> seen;
    for (const auto& f : manifest.fields) {
        if (f.parent_id == -1) {
            seen.insert(f.name);
        }
    }
    for (const auto& n : names) {
        if (!seen.insert(n).second) {
            error = "column '" + n + "' already exists";
            return false;
        }
    }
    return true;
}

std::vector<std::uint64_t> fragment_ids(const pb::Manifest& manifest) {
    std::vector<std::uint64_t> ids;
    for (const auto& f : manifest.fragments) {
        ids.push_back(f.id);
    }
    return ids;
}

/// The manifest field for `path` ("a" or "a.b").
pb::Field* find_field(pb::Manifest& manifest, const std::string& path) {
    std::int32_t parent = -1;
    pb::Field* found = nullptr;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto dot = path.find('.', start);
        const auto part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        found = nullptr;
        for (auto& f : manifest.fields) {
            if (f.parent_id == parent && f.name == part) {
                found = &f;
                break;
            }
        }
        if (found == nullptr) {
            // A top-level name with a dot in it.
            if (parent == -1) {
                for (auto& f : manifest.fields) {
                    if (f.parent_id == -1 && f.name == path) {
                        return &f;
                    }
                }
            }
            return nullptr;
        }
        if (dot == std::string::npos) {
            break;
        }
        parent = found->id;
        start = dot + 1;
    }
    return found;
}

}  // namespace

// ── delete ──────────────────────────────────────────────────────────────────────────────────────

bool dataset_delete(const std::filesystem::path& dataset_path, const std::string& predicate, std::uint64_t& deleted,
                    std::uint64_t& new_version, std::string& error) {
    error.clear();
    deleted = 0;
    if (predicate.empty()) {
        error = "a delete needs a predicate";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    std::map<std::uint64_t, std::vector<std::uint32_t>> rows;
    if (!matching_rows(dataset_path, version, &predicate, rows, deleted, error) ||
        !apply_deletions(dataset_path, manifest, version, rows, error)) {
        return false;
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

// ── update ──────────────────────────────────────────────────────────────────────────────────────

bool dataset_update(const std::filesystem::path& dataset_path, const std::string* predicate,
                    const std::vector<std::pair<std::string, std::string>>& assignments, std::uint64_t& updated,
                    std::uint64_t& new_version, std::string& error) {
    error.clear();
    updated = 0;
    if (assignments.empty()) {
        error = "an update needs at least one column to set";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    request.with_row_address = true;
    request.filter = predicate != nullptr && !predicate->empty() ? predicate : nullptr;
    OwnedSchema schema;
    OwnedBatches batches;
    if (!scan(dataset_path, request, schema, batches, error)) {
        return false;
    }
    const auto data_columns = schema.s.n_children - 1;  // the last is _rowaddr
    std::vector<std::pair<int64_t, expr::Expression>> sets;
    for (const auto& [column, value] : assignments) {
        const auto index = child_index(schema.s, column);
        if (index < 0 || index >= data_columns) {
            error = "column '" + column + "' not found";
            return false;
        }
        expr::Expression e;
        if (!expr::Expression::parse(value, e, error) || !e.bind(schema.s, error)) {
            return false;
        }
        sets.emplace_back(index, std::move(e));
    }
    std::vector<const ArrowSchema*> fields;
    for (int64_t i = 0; i < data_columns; ++i) {
        fields.push_back(schema.s.children[i]);
    }
    OwnedSchema out_schema;
    if (!make_struct_schema(fields, out_schema.s, error)) {
        return false;
    }
    std::map<std::uint64_t, std::vector<std::uint32_t>> rewritten;
    StagedFiles staged;
    bool opened = false;
    for (auto& batch : batches.v) {
        if (batch.length == 0) {
            continue;
        }
        const ArrowArray* addr = batch.children[data_columns];
        const auto* values = static_cast<const std::uint64_t*>(addr->buffers[1]) + addr->offset;
        for (int64_t i = 0; i < batch.length; ++i) {
            rewritten[values[i] >> 32U].push_back(static_cast<std::uint32_t>(values[i] & 0xFFFFFFFFULL));
        }
        std::vector<ArrowArray> replaced(static_cast<std::size_t>(data_columns));
        for (auto& [index, e] : sets) {
            ArrowArray value{};
            if (!e.evaluate(batch, *schema.s.children[index], value, error)) {
                for (auto& r : replaced) {
                    if (r.release != nullptr) {
                        r.release(&r);
                    }
                }
                return false;
            }
            if (replaced[static_cast<std::size_t>(index)].release != nullptr) {
                replaced[static_cast<std::size_t>(index)].release(&replaced[static_cast<std::size_t>(index)]);
            }
            replaced[static_cast<std::size_t>(index)] = value;
        }
        for (int64_t i = 0; i < data_columns; ++i) {
            if (replaced[static_cast<std::size_t>(i)].release == nullptr) {
                ArrowArrayMove(batch.children[i], &replaced[static_cast<std::size_t>(i)]);
            }
        }
        ArrowArray rewritten_batch{};
        if (!make_struct(replaced, batch.length, rewritten_batch, error)) {
            return false;
        }
        if (!opened && !staged.open(dataset_path, true, 0, error)) {
            rewritten_batch.release(&rewritten_batch);
            return false;
        }
        opened = true;
        const bool ok = staged.write(rewritten_batch, out_schema.s, error);
        rewritten_batch.release(&rewritten_batch);
        if (!ok) {
            return false;
        }
        updated += static_cast<std::uint64_t>(batch.length);
    }
    if (opened && !staged.finish(error)) {
        return false;
    }
    if (!apply_deletions(dataset_path, manifest, version, rewritten, error)) {
        return false;
    }
    if (opened) {
        add_fragments(manifest, staged.mapping(), staged.files());
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

// ── merge insert ────────────────────────────────────────────────────────────────────────────────

bool dataset_merge_insert(const std::filesystem::path& dataset_path, const MergeInsertSpec& spec,
                          ArrowArrayStream& source, MergeInsertStats& stats, std::uint64_t& new_version,
                          std::string& error) {
    struct Release {
        ArrowArrayStream* s;
        ~Release() {
            if (s->release != nullptr) {
                s->release(s);
            }
        }
    } release_source{&source};
    error.clear();
    stats = {};
    if (spec.on.empty()) {
        error = "merge insert needs at least one key column";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    // The dataset's schema, to put the source's columns in its order.
    OwnedSchema target_schema;
    {
        LanceScanRequest request;
        request.has_version = true;
        request.version = version;
        if (!lance_dataset_schema(dataset_path, request, target_schema.s, error)) {
            return false;
        }
    }
    // The source, whole.
    OwnedSchema source_schema;
    if (source.get_schema(&source, &source_schema.s) != 0) {
        error = "failed to read the source's schema";
        return false;
    }
    std::vector<int64_t> order;  // source column for each dataset column
    for (int64_t i = 0; i < target_schema.s.n_children; ++i) {
        const auto k = child_index(source_schema.s, target_schema.s.children[i]->name);
        if (k < 0) {
            error = std::string("the source has no column '") + target_schema.s.children[i]->name + "'";
            return false;
        }
        order.push_back(k);
    }
    if (source_schema.s.n_children != target_schema.s.n_children) {
        error = "the source's columns are not the dataset's";
        return false;
    }
    std::vector<std::shared_ptr<SharedBatch>> source_batches;
    for (;;) {
        ArrowArray b{};
        if (source.get_next(&source, &b) != 0) {
            const char* why = source.get_last_error != nullptr ? source.get_last_error(&source) : nullptr;
            error = std::string("reading the source failed") + (why != nullptr ? std::string(": ") + why : "");
            return false;
        }
        if (b.release == nullptr) {
            break;
        }
        source_batches.push_back(std::make_shared<SharedBatch>(std::move(b)));
    }
    std::vector<int64_t> source_keys;
    for (const auto& k : spec.on) {
        const auto index = child_index(source_schema.s, k);
        if (index < 0) {
            error = "the source has no key column '" + k + "'";
            return false;
        }
        source_keys.push_back(index);
    }

    // The dataset's keys and row addresses.
    std::unordered_map<std::string, std::uint64_t> target;
    std::vector<std::uint64_t> all_targets;
    {
        LanceScanRequest request;
        request.has_version = true;
        request.version = version;
        request.columns = &spec.on;
        request.with_row_address = true;
        OwnedSchema schema;
        OwnedBatches batches;
        if (!scan(dataset_path, request, schema, batches, error)) {
            return false;
        }
        std::vector<int64_t> keys;
        for (const auto& k : spec.on) {
            keys.push_back(child_index(schema.s, k));
        }
        std::string key;
        for (const auto& b : batches.v) {
            BatchView view;
            if (!view.set(schema.s, b, error)) {
                return false;
            }
            const ArrowArray* addr = b.children[b.n_children - 1];
            const auto* values = static_cast<const std::uint64_t*>(addr->buffers[1]) + addr->offset;
            for (int64_t i = 0; i < b.length; ++i) {
                all_targets.push_back(values[i]);
                if (key_of(view.view, keys, i, key)) {
                    target[key] = values[i];
                }
            }
        }
    }

    // Which source rows are written, which target rows go.
    std::set<std::uint64_t> matched;
    std::map<std::uint64_t, std::vector<std::uint32_t>> deletions;
    StagedFiles staged;
    bool opened = false;
    std::string key;
    for (const auto& shared : source_batches) {
        BatchView view;
        if (!view.set(source_schema.s, shared->array, error)) {
            return false;
        }
        std::vector<std::uint8_t> keep(static_cast<std::size_t>(shared->array.length), 0U);
        for (int64_t i = 0; i < shared->array.length; ++i) {
            const bool has_key = key_of(view.view, source_keys, i, key);
            const auto it = has_key ? target.find(key) : target.end();
            if (it != target.end()) {
                matched.insert(it->second);
                using WM = MergeInsertSpec::WhenMatched;
                if (spec.when_matched == WM::Fail) {
                    error = "merge insert: a source row matches an existing row, and when_matched is fail";
                    return false;
                }
                if (spec.when_matched == WM::UpdateAll || spec.when_matched == WM::Delete) {
                    keep[static_cast<std::size_t>(i)] = spec.when_matched == WM::UpdateAll ? 1U : 0U;
                    deletions[it->second >> 32U].push_back(static_cast<std::uint32_t>(it->second & 0xFFFFFFFFULL));
                    ++(spec.when_matched == WM::UpdateAll ? stats.updated : stats.deleted);
                }
            } else if (spec.when_not_matched_insert_all) {
                keep[static_cast<std::size_t>(i)] = 1U;
                ++stats.inserted;
            }
        }
        // Runs of kept rows, as slices in the dataset's column order.
        int64_t i = 0;
        while (i < shared->array.length) {
            if (keep[static_cast<std::size_t>(i)] == 0U) {
                ++i;
                continue;
            }
            int64_t j = i;
            while (j < shared->array.length && keep[static_cast<std::size_t>(j)] != 0U) {
                ++j;
            }
            ArrowArray piece = slice_batch(shared, i, j - i, order);
            if (!opened && !staged.open(dataset_path, true, 0, error)) {
                piece.release(&piece);
                return false;
            }
            opened = true;
            const bool ok = staged.write(piece, target_schema.s, error);
            piece.release(&piece);
            if (!ok) {
                return false;
            }
            i = j;
        }
    }
    if (spec.when_not_matched_by_source_delete) {
        std::unordered_set<std::uint64_t> eligible;
        if (!spec.when_not_matched_by_source_condition.empty()) {
            std::map<std::uint64_t, std::vector<std::uint32_t>> rows;
            std::uint64_t count = 0;
            if (!matching_rows(dataset_path, version, &spec.when_not_matched_by_source_condition, rows, count, error)) {
                return false;
            }
            for (const auto& [fragment, offsets] : rows) {
                for (const auto o : offsets) {
                    eligible.insert((fragment << 32U) | o);
                }
            }
        }
        for (const auto address : all_targets) {
            if (matched.count(address) == 0U &&
                (spec.when_not_matched_by_source_condition.empty() || eligible.count(address) != 0U)) {
                deletions[address >> 32U].push_back(static_cast<std::uint32_t>(address & 0xFFFFFFFFULL));
                ++stats.deleted;
            }
        }
    }
    if (opened && !staged.finish(error)) {
        return false;
    }
    if (!apply_deletions(dataset_path, manifest, version, deletions, error)) {
        return false;
    }
    if (opened) {
        add_fragments(manifest, staged.mapping(), staged.files());
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

// ── columns ─────────────────────────────────────────────────────────────────────────────────────

bool dataset_add_columns_sql(const std::filesystem::path& dataset_path,
                             const std::vector<std::pair<std::string, std::string>>& columns,
                             std::uint64_t& new_version, std::string& error) {
    error.clear();
    if (columns.empty()) {
        error = "no columns to add";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    std::vector<std::string> names;
    std::vector<expr::Expression> exprs(columns.size());
    std::vector<std::string> needed;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        names.push_back(columns[i].first);
        if (!expr::Expression::parse(columns[i].second, exprs[i], error)) {
            return false;
        }
        for (const auto& c : exprs[i].columns()) {
            const auto top = c.substr(0, c.find('.'));
            const bool whole = std::any_of(manifest.fields.begin(), manifest.fields.end(),
                                           [&](const pb::Field& f) { return f.parent_id == -1 && f.name == c; });
            const auto& name = whole ? c : top;
            if (std::find(needed.begin(), needed.end(), name) == needed.end()) {
                needed.push_back(name);
            }
        }
    }
    if (!check_new_names(manifest, names, error)) {
        return false;
    }
    StagedFiles staged;
    if (!staged.open(dataset_path, false, max_field_id(manifest) + 1, error)) {
        return false;
    }
    const auto ids = fragment_ids(manifest);
    OwnedSchema new_schema;
    bool typed = false;
    for (const auto id : ids) {
        LanceScanRequest request;
        request.has_version = true;
        request.version = version;
        const std::vector<std::uint64_t> one = {id};
        request.fragment_ids = &one;
        request.columns = &needed;
        request.include_deleted_rows = true;
        request.with_row_address = needed.empty();  // rows to count when no column is read
        OwnedSchema schema;
        OwnedBatches batches;
        if (!scan(dataset_path, request, schema, batches, error)) {
            return false;
        }
        for (auto& e : exprs) {
            if (!e.bind(schema.s, error)) {
                return false;
            }
        }
        if (!typed) {
            std::vector<OwnedSchema> types(exprs.size());
            std::vector<const ArrowSchema*> fields;
            for (std::size_t i = 0; i < exprs.size(); ++i) {
                if (!exprs[i].result_type(names[i], types[i].s, error)) {
                    return false;
                }
                fields.push_back(&types[i].s);
            }
            if (!make_struct_schema(fields, new_schema.s, error)) {
                return false;
            }
            typed = true;
        }
        for (auto& batch : batches.v) {
            std::vector<ArrowArray> values(exprs.size());
            for (std::size_t i = 0; i < exprs.size(); ++i) {
                if (!exprs[i].evaluate(batch, *new_schema.s.children[i], values[i], error)) {
                    for (auto& v : values) {
                        if (v.release != nullptr) {
                            v.release(&v);
                        }
                    }
                    return false;
                }
            }
            ArrowArray out{};
            if (!make_struct(values, batch.length, out, error)) {
                return false;
            }
            const bool ok = staged.write(out, new_schema.s, error);
            out.release(&out);
            if (!ok) {
                return false;
            }
        }
        if (!staged.cut(error, true)) {
            return false;
        }
    }
    if (!attach_columns(manifest, ids, staged, error)) {
        return false;
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_add_columns_nulls(const std::filesystem::path& dataset_path, const ArrowSchema& schema,
                               std::uint64_t& new_version, std::string& error) {
    error.clear();
    if (schema.n_children <= 0) {
        error = "no columns to add";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    std::vector<std::string> names;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        names.emplace_back(schema.children[i]->name != nullptr ? schema.children[i]->name : "");
    }
    if (!check_new_names(manifest, names, error)) {
        return false;
    }
    // Schema only, as Lance does it: no data file is written, and every fragment reads the new
    // columns as nulls because none of its files holds them.
    LanceSchemaMapping added;
    if (!map_arrow_schema(schema, added, error)) {
        return false;
    }
    const auto base = max_field_id(manifest) + 1;
    for (auto& f : added.fields) {
        f.id += base;
        if (f.parent_id >= 0) {
            f.parent_id += base;
        }
        f.nullable = true;
        manifest.fields.push_back(make_manifest_field(f));
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_add_columns_stream(const std::filesystem::path& dataset_path, ArrowArrayStream& stream,
                                std::uint64_t& new_version, std::string& error) {
    struct Release {
        ArrowArrayStream* s;
        ~Release() {
            if (s->release != nullptr) {
                s->release(s);
            }
        }
    } release{&stream};
    error.clear();
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    for (const auto& f : manifest.fragments) {
        if (f.deletion_file.present) {
            error = "adding columns from a stream needs a dataset without deleted rows (compact it first)";
            return false;
        }
    }
    OwnedSchema schema;
    if (stream.get_schema(&stream, &schema.s) != 0) {
        error = "failed to read the stream's schema";
        return false;
    }
    std::vector<std::string> names;
    for (int64_t i = 0; i < schema.s.n_children; ++i) {
        names.emplace_back(schema.s.children[i]->name != nullptr ? schema.s.children[i]->name : "");
    }
    if (!check_new_names(manifest, names, error)) {
        return false;
    }
    StagedFiles staged;
    if (!staged.open(dataset_path, false, max_field_id(manifest) + 1, error)) {
        return false;
    }
    const auto ids = fragment_ids(manifest);
    std::size_t fragment = 0;
    std::uint64_t in_fragment = 0;  // rows written into the current fragment's file
    for (;;) {
        ArrowArray b{};
        if (stream.get_next(&stream, &b) != 0) {
            error = "reading the stream failed";
            return false;
        }
        if (b.release == nullptr) {
            break;
        }
        auto shared = std::make_shared<SharedBatch>(std::move(b));
        int64_t at = 0;
        while (at < shared->array.length) {
            if (fragment >= manifest.fragments.size()) {
                error = "the stream has more rows than the dataset";
                return false;
            }
            const auto want = manifest.fragments[fragment].physical_rows - in_fragment;
            const auto take = std::min<std::uint64_t>(want, static_cast<std::uint64_t>(shared->array.length - at));
            ArrowArray piece = slice_batch(shared, at, static_cast<int64_t>(take));
            const bool ok = staged.write(piece, schema.s, error);
            piece.release(&piece);
            if (!ok) {
                return false;
            }
            at += static_cast<int64_t>(take);
            in_fragment += take;
            if (in_fragment == manifest.fragments[fragment].physical_rows) {
                if (!staged.cut(error, true)) {
                    return false;
                }
                ++fragment;
                in_fragment = 0;
            }
        }
    }
    if (fragment != manifest.fragments.size()) {
        error = "the stream has fewer rows than the dataset";
        return false;
    }
    if (!attach_columns(manifest, ids, staged, error)) {
        return false;
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_drop_columns(const std::filesystem::path& dataset_path, const std::vector<std::string>& columns,
                          std::uint64_t& new_version, std::string& error) {
    error.clear();
    if (columns.empty()) {
        error = "no columns to drop";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    std::unordered_set<std::int32_t> gone;
    for (const auto& name : columns) {
        const auto* f = find_field(manifest, name);
        if (f == nullptr) {
            error = "column '" + name + "' not found";
            return false;
        }
        std::vector<std::int32_t> queue = {f->id};
        while (!queue.empty()) {
            const auto id = queue.back();
            queue.pop_back();
            gone.insert(id);
            for (const auto& c : manifest.fields) {
                if (c.parent_id == id) {
                    queue.push_back(c.id);
                }
            }
        }
    }
    std::vector<pb::Field> kept;
    for (auto& f : manifest.fields) {
        if (gone.count(f.id) == 0U) {
            kept.push_back(std::move(f));
        }
    }
    if (std::none_of(kept.begin(), kept.end(), [](const pb::Field& f) { return f.parent_id == -1; })) {
        error = "cannot drop every column of a dataset";
        return false;
    }
    manifest.fields = std::move(kept);
    return commit(dataset_path, std::move(manifest), new_version, error);
}

bool dataset_alter_columns(const std::filesystem::path& dataset_path, const std::vector<ColumnAlteration>& alterations,
                           std::uint64_t& new_version, std::string& error) {
    error.clear();
    if (alterations.empty()) {
        error = "no alterations";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    for (const auto& a : alterations) {
        auto* field = find_field(manifest, a.path);
        if (field == nullptr) {
            error = "column '" + a.path + "' not found";
            return false;
        }
        if (a.nullable.has_value()) {
            if (!*a.nullable && field->nullable) {
                // Only when no row holds a null.
                const std::string predicate = "`" + a.path + "` IS NULL";
                std::map<std::uint64_t, std::vector<std::uint32_t>> rows;
                std::uint64_t count = 0;
                std::string ignored;
                if (!matching_rows(dataset_path, version, &predicate, rows, count, error)) {
                    return false;
                }
                if (count != 0U) {
                    error = "column '" + a.path + "' holds nulls; it cannot be made non-nullable";
                    return false;
                }
            }
            field->nullable = *a.nullable;
        }
        if (a.data_type != nullptr) {
            if (field->parent_id != -1) {
                error = "changing the type of a nested field is not supported";
                return false;
            }
            const std::string name = field->name;
            const std::int32_t old_id = field->id;
            // Rewrite the column as the new type: CAST, fragment by fragment, into new files.
            OwnedSchema target;
            if (ArrowSchemaDeepCopy(a.data_type, &target.s) != NANOARROW_OK ||
                ArrowSchemaSetName(&target.s, name.c_str()) != NANOARROW_OK) {
                error = "failed to copy the new type";
                return false;
            }
            target.s.flags = field->nullable ? ARROW_FLAG_NULLABLE : 0;
            std::vector<const ArrowSchema*> one_field = {&target.s};
            OwnedSchema out_schema;
            if (!make_struct_schema(one_field, out_schema.s, error)) {
                return false;
            }
            expr::Expression cast;
            if (!expr::Expression::parse("`" + name + "`", cast, error)) {
                return false;
            }
            StagedFiles staged;
            if (!staged.open(dataset_path, false, max_field_id(manifest) + 1, error)) {
                return false;
            }
            const std::vector<std::string> column = {name};
            const auto ids = fragment_ids(manifest);
            for (const auto id : ids) {
                LanceScanRequest request;
                request.has_version = true;
                request.version = version;
                const std::vector<std::uint64_t> just = {id};
                request.fragment_ids = &just;
                request.columns = &column;
                request.include_deleted_rows = true;
                OwnedSchema schema;
                OwnedBatches batches;
                if (!scan(dataset_path, request, schema, batches, error) || !cast.bind(schema.s, error)) {
                    return false;
                }
                for (auto& batch : batches.v) {
                    std::vector<ArrowArray> values(1);
                    if (!cast.evaluate(batch, target.s, values[0], error)) {
                        return false;
                    }
                    ArrowArray out{};
                    if (!make_struct(values, batch.length, out, error)) {
                        return false;
                    }
                    const bool ok = staged.write(out, out_schema.s, error);
                    out.release(&out);
                    if (!ok) {
                        return false;
                    }
                }
                if (!staged.cut(error, true)) {
                    return false;
                }
            }
            // The new field takes the old one's place in the schema.
            const auto position = std::find_if(manifest.fields.begin(), manifest.fields.end(),
                                               [&](const pb::Field& f) { return f.id == old_id; }) -
                                  manifest.fields.begin();
            manifest.fields.erase(manifest.fields.begin() + position);
            const auto before = manifest.fields.size();
            if (!attach_columns(manifest, ids, staged, error)) {
                return false;
            }
            std::rotate(manifest.fields.begin() + position, manifest.fields.begin() + static_cast<std::ptrdiff_t>(before),
                        manifest.fields.end());
            field = nullptr;
        }
        if (a.rename.has_value()) {
            if (field == nullptr) {
                field = find_field(manifest, a.path);
            }
            if (field == nullptr) {
                error = "column '" + a.path + "' not found";
                return false;
            }
            for (const auto& f : manifest.fields) {
                if (f.parent_id == field->parent_id && f.name == *a.rename && f.id != field->id) {
                    error = "column '" + *a.rename + "' already exists";
                    return false;
                }
            }
            field->name = *a.rename;
        }
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

// ── compaction ──────────────────────────────────────────────────────────────────────────────────

bool dataset_compact_files(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                           CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error) {
    error.clear();
    metrics = {};
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    new_version = version;
    const auto target = std::max<std::uint64_t>(options.target_rows_per_fragment, 1U);
    auto rows_of = [](const pb::DataFragment& f) {
        return f.physical_rows - (f.deletion_file.present ? f.deletion_file.num_deleted_rows : 0U);
    };
    auto candidate = [&](const pb::DataFragment& f) {
        if (rows_of(f) < target) {
            return true;
        }
        return options.materialize_deletions && f.deletion_file.present && f.physical_rows != 0U &&
               static_cast<double>(f.deletion_file.num_deleted_rows) / static_cast<double>(f.physical_rows) >
                   options.materialize_deletions_threshold;
    };
    // Runs of adjacent candidates, cut into bins of about `target` rows.
    std::vector<std::vector<std::size_t>> bins;
    std::vector<std::size_t> bin;
    std::uint64_t bin_rows = 0;
    auto close_bin = [&] {
        if (bin.size() > 1U || (bin.size() == 1U && manifest.fragments[bin.front()].deletion_file.present &&
                                options.materialize_deletions)) {
            bins.push_back(bin);
        }
        bin.clear();
        bin_rows = 0;
    };
    for (std::size_t i = 0; i < manifest.fragments.size(); ++i) {
        const auto& f = manifest.fragments[i];
        if (!candidate(f)) {
            close_bin();
            continue;
        }
        if (!bin.empty() && bin_rows + rows_of(f) > target) {
            close_bin();
        }
        bin.push_back(i);
        bin_rows += rows_of(f);
    }
    close_bin();
    if (bins.empty()) {
        return true;
    }
    std::vector<pb::DataFragment> rebuilt;
    std::size_t next_bin = 0;
    auto id = next_fragment_id(manifest);
    std::vector<pb::DataFragment> old = manifest.fragments;
    for (std::size_t i = 0; i < old.size();) {
        if (next_bin < bins.size() && bins[next_bin].front() == i) {
            const auto& members = bins[next_bin];
            std::vector<std::uint64_t> ids;
            for (const auto m : members) {
                ids.push_back(old[m].id);
                // Lance counts a fragment's deletion file among the files a compaction removes.
                metrics.files_removed += old[m].files.size() + (old[m].deletion_file.present ? 1U : 0U);
            }
            metrics.fragments_removed += members.size();
            LanceScanRequest request;
            request.has_version = true;
            request.version = version;
            request.fragment_ids = &ids;
            OwnedSchema schema;
            OwnedBatches batches;
            if (!scan(dataset_path, request, schema, batches, error)) {
                return false;
            }
            StagedFiles staged;
            if (!staged.open(dataset_path, true, 0, error)) {
                return false;
            }
            for (auto& b : batches.v) {
                if (b.length != 0 && !staged.write(b, schema.s, error)) {
                    return false;
                }
            }
            if (!staged.finish(error)) {
                return false;
            }
            for (const auto& f : staged.files()) {
                if (f.rows == 0U) {
                    continue;
                }
                rebuilt.push_back(make_data_fragment(staged.mapping(), f, id));
                note_fragment_id(manifest, id);
                ++id;
                ++metrics.fragments_added;
                ++metrics.files_added;
            }
            i = members.back() + 1U;
            ++next_bin;
        } else {
            rebuilt.push_back(old[i]);
            ++i;
        }
    }
    manifest.fragments = std::move(rebuilt);
    const bool deletions = std::any_of(manifest.fragments.begin(), manifest.fragments.end(),
                                       [](const pb::DataFragment& f) { return f.deletion_file.present; });
    if (!deletions) {
        manifest.reader_feature_flags &= ~pb::kFlagDeletionFiles;
        manifest.writer_feature_flags &= ~pb::kFlagDeletionFiles;
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

}  // namespace nano_lance
