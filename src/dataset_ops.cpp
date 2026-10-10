// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Dataset changes (dataset_ops.hpp), each one version: read what the change needs through the scan
// machinery (filters, row addresses), write new data through the staged writer, and commit the
// latest manifest with the changed fragments and schema.

#include "nanolance/dataset_ops.hpp"
#include "nanolance/row_ids.hpp"
#include "nanolance/index_optimize.hpp"

#include "nanolance/arrow_slice.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"
#include "nanolance/writer_internal.hpp"

#include "lance_minimal.pb.hpp"
#include "transaction_file.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <random>
#include <thread>

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
    next = std::max(next, first_fragment_id_after_indices(manifest.indices));
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

    /// On an append writer: write only these top-level columns, with the dataset's field ids.
    bool project(const std::vector<std::string>& columns, std::string& error) {
        return writer_project_append(&writer_, columns, error);
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

    /// End a data file whenever the rows buffered reach `bytes` (0: never).
    void set_max_bytes(std::uint64_t bytes) {
        if (bytes > 0U) {
            nano_lance_writer_set_max_pending_bytes(&writer_, bytes);
        }
    }

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

/// Publish `manifest` as the next version. With `retain_indices`, an index on a field the change
/// removed goes too -- Lance's retain_relevant_indices, which its Delete, Update, Merge and Project
/// commits apply (a compaction narrows coverage instead; see index_maintenance.hpp).
bool commit(const std::filesystem::path& path, pb::Manifest manifest, std::uint64_t& new_version, std::string& error,
            bool retain_indices = true) {
    if (retain_indices) {
        retain_relevant_indices(manifest.indices, manifest.fields, manifest.fragments);
    }
    // Lance keeps fragments in id order, and validate() refuses a dataset whose fragments are not: a
    // compaction's new fragments (the highest ids) go last, not where the fragments they replace were.
    std::stable_sort(manifest.fragments.begin(), manifest.fragments.end(),
                     [](const pb::DataFragment& a, const pb::DataFragment& b) { return a.id < b.id; });
    return commit_next_version(path, std::move(manifest), new_version, error);
}

bool is_stable(const pb::Manifest& manifest) {
    return (manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U;
}

/// The version each row at `addresses` was created at, from the fragments' created-at sequences
/// (1 for a fragment that carries none, as Lance reads it).
bool created_versions_of(const pb::Manifest& manifest, const std::vector<std::uint64_t>& addresses,
                         std::vector<std::uint64_t>& out, std::string& error) {
    std::map<std::uint64_t, const pb::DataFragment*> fragments;
    for (const auto& f : manifest.fragments) {
        fragments.emplace(f.id, &f);
    }
    std::map<std::uint64_t, std::optional<RowVersionSequence>> cache;
    out.clear();
    out.reserve(addresses.size());
    for (const auto address : addresses) {
        const auto fragment_id = address >> 32U;
        auto slot = cache.find(fragment_id);
        if (slot == cache.end()) {
            std::optional<RowVersionSequence> seq;
            const auto it = fragments.find(fragment_id);
            if (it != fragments.end()) {
                FragmentRowMeta meta;
                if (!read_fragment_row_meta(*it->second, meta, error)) {
                    return false;
                }
                if (meta.has_created) {
                    RowVersionSequence decoded;
                    if (!RowVersionSequence::decode(meta.created.data(), meta.created.size(), decoded, error)) {
                        return false;
                    }
                    seq = std::move(decoded);
                }
            }
            slot = cache.emplace(fragment_id, std::move(seq)).first;
        }
        const auto offset = address & 0xFFFFFFFFULL;
        const std::uint64_t version = slot->second ? slot->second->at(offset) : 0U;
        out.push_back(version == 0U ? 1U : version);
    }
    return true;
}

constexpr std::uint64_t kNewRow = UINT64_MAX;  // an address in a list of moved rows that is no row yet

/// For rows at `addresses` (kNewRow: a row that does not exist yet), their ids and creation versions.
/// New rows are marked kNewRow in `ids`, for stamp_new_fragments to number.
bool moved_row_meta(const pb::Manifest& manifest, const std::vector<std::uint64_t>& addresses,
                    std::vector<std::uint64_t>& ids, std::vector<std::uint64_t>& created, std::string& error) {
    std::map<std::uint64_t, const pb::DataFragment*> fragments;
    for (const auto& f : manifest.fragments) {
        fragments.emplace(f.id, &f);
    }
    struct Known {
        std::optional<RowIdSequence> ids;
        std::optional<RowVersionSequence> created;
    };
    std::map<std::uint64_t, Known> cache;
    ids.clear();
    created.clear();
    for (const auto address : addresses) {
        if (address == kNewRow) {
            ids.push_back(kNewRow);
            created.push_back(0);
            continue;
        }
        const auto fragment_id = address >> 32U;
        auto slot = cache.find(fragment_id);
        if (slot == cache.end()) {
            Known known;
            const auto it = fragments.find(fragment_id);
            if (it == fragments.end()) {
                error = "internal error: a moved row's fragment is gone";
                return false;
            }
            RowIdSequence seq;
            if (!fragment_row_ids(*it->second, seq, error)) {
                return false;
            }
            known.ids = std::move(seq);
            FragmentRowMeta meta;
            if (!read_fragment_row_meta(*it->second, meta, error)) {
                return false;
            }
            if (meta.has_created) {
                RowVersionSequence decoded;
                if (!RowVersionSequence::decode(meta.created.data(), meta.created.size(), decoded, error)) {
                    return false;
                }
                known.created = std::move(decoded);
            }
            slot = cache.emplace(fragment_id, std::move(known)).first;
        }
        const auto offset = address & 0xFFFFFFFFULL;
        if (offset >= slot->second.ids->size()) {
            error = "internal error: a moved row is past its fragment";
            return false;
        }
        ids.push_back(slot->second.ids->at(offset));
        const std::uint64_t version = slot->second.created ? slot->second.created->at(offset) : 0U;
        created.push_back(version == 0U ? 1U : version);
    }
    return true;
}

/// Give `fragments` from index `first` on -- new files holding rows that moved or arrived -- their rows'
/// ids and versions. `ids`, `created` and `updated` are per row, in the order the rows were written; a
/// row whose id is kNewRow is a new row: the next id from `next_row_id`, created and updated at
/// `new_version`.
bool stamp_fragments(std::vector<pb::DataFragment>& fragments, std::size_t first, const std::vector<std::uint64_t>& ids,
                     const std::vector<std::uint64_t>& created, const std::vector<std::uint64_t>& updated,
                     std::uint64_t& next_row_id, std::uint64_t new_version, std::string& error) {
    std::size_t at = 0;
    for (std::size_t f = first; f < fragments.size(); ++f) {
        auto& fragment = fragments[f];
        std::vector<std::uint64_t> frag_ids;
        std::vector<std::uint64_t> frag_created;
        std::vector<std::uint64_t> frag_updated;
        frag_ids.reserve(static_cast<std::size_t>(fragment.physical_rows));
        for (std::uint64_t r = 0; r < fragment.physical_rows; ++r, ++at) {
            if (at < ids.size() && ids[at] != kNewRow) {
                frag_ids.push_back(ids[at]);
                frag_created.push_back(created[at]);
                frag_updated.push_back(updated[at]);
            } else {
                frag_ids.push_back(next_row_id++);
                frag_created.push_back(new_version);
                frag_updated.push_back(new_version);
            }
        }
        FragmentRowMeta meta;
        meta.has_row_ids = true;
        meta.row_ids = RowIdSequence::from_values(std::move(frag_ids)).encode();
        meta.has_created = meta.has_last_updated = true;
        meta.created = RowVersionSequence::from_values(frag_created).encode();
        meta.last_updated = RowVersionSequence::from_values(frag_updated).encode();
        write_fragment_row_meta(fragment, meta);
    }
    if (at < ids.size()) {
        error = "internal error: rows moved to new fragments are missing";
        return false;
    }
    return true;
}

/// stamp_fragments for an update: the moved rows were last updated now.
bool stamp_new_fragments(pb::Manifest& manifest, std::size_t first, const std::vector<std::uint64_t>& ids,
                         const std::vector<std::uint64_t>& created, std::uint64_t new_version, std::string& error) {
    const std::vector<std::uint64_t> updated(ids.size(), new_version);
    return stamp_fragments(manifest.fragments, first, ids, created, updated, manifest.next_row_id, new_version, error);
}

/// The live rows of `fragment` in order: their ids, creation and last update versions.
bool live_row_meta(const std::filesystem::path& dataset_path, const pb::DataFragment& fragment,
                   std::vector<std::uint64_t>& ids, std::vector<std::uint64_t>& created,
                   std::vector<std::uint64_t>& updated, std::string& error) {
    RowIdSequence sequence;
    if (!fragment_row_ids(fragment, sequence, error)) {
        return false;
    }
    FragmentRowMeta meta;
    if (!read_fragment_row_meta(fragment, meta, error)) {
        return false;
    }
    std::vector<std::uint64_t> created_all;
    std::vector<std::uint64_t> updated_all;
    RowVersionSequence decoded;
    if (meta.has_created) {
        if (!RowVersionSequence::decode(meta.created.data(), meta.created.size(), decoded, error)) {
            return false;
        }
        created_all = decoded.to_vector();
    }
    if (meta.has_last_updated) {
        if (!RowVersionSequence::decode(meta.last_updated.data(), meta.last_updated.size(), decoded, error)) {
            return false;
        }
        updated_all = decoded.to_vector();
    }
    std::vector<std::uint32_t> deleted;
    if (fragment.deletion_file.present &&
        !read_deletion_vector(dataset_path, fragment.id, fragment.deletion_file, deleted, error)) {
        return false;
    }
    std::size_t next_deleted = 0;
    for (std::uint64_t i = 0; i < sequence.size(); ++i) {
        while (next_deleted < deleted.size() && deleted[next_deleted] < i) {
            ++next_deleted;
        }
        if (next_deleted < deleted.size() && deleted[next_deleted] == i) {
            continue;
        }
        ids.push_back(sequence.at(i));
        created.push_back(i < created_all.size() ? created_all[i] : 1U);
        updated.push_back(i < updated_all.size() ? updated_all[i] : 1U);
    }
    return true;
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


// One attempt of each, defined below.
bool delete_once(const std::filesystem::path& dataset_path, const std::string& predicate, std::uint64_t& deleted,
                 std::uint64_t& new_version, std::string& error);
bool update_once(const std::filesystem::path& dataset_path, const std::string* predicate,
                 const std::vector<std::pair<std::string, std::string>>& assignments, std::uint64_t& updated,
                 std::uint64_t& new_version, std::string& error);
bool merge_insert_once(const std::filesystem::path& dataset_path, const MergeInsertSpec& spec,
                       ArrowArrayStream& source, MergeInsertStats& stats, std::uint64_t& new_version,
                       std::string& error);

bool dataset_delete(const std::filesystem::path& dataset_path, const std::string& predicate, std::uint64_t& deleted,
                    std::uint64_t& new_version, std::string& error) {
    return retry_on_conflict([&] { return delete_once(dataset_path, predicate, deleted, new_version, error); }, error);
}

bool dataset_update(const std::filesystem::path& dataset_path, const std::string* predicate,
                    const std::vector<std::pair<std::string, std::string>>& assignments, std::uint64_t& updated,
                    std::uint64_t& new_version, std::string& error) {
    return retry_on_conflict(
        [&] { return update_once(dataset_path, predicate, assignments, updated, new_version, error); }, error);
}

bool dataset_merge_insert(const std::filesystem::path& dataset_path, const MergeInsertSpec& spec,
                          ArrowArrayStream& source, MergeInsertStats& stats, std::uint64_t& new_version,
                          std::string& error) {
    // The source is read once and replayed to each attempt.
    struct Release {
        ArrowArrayStream* s;
        ~Release() {
            if (s->release != nullptr) {
                s->release(s);
            }
        }
    } release_source{&source};
    OwnedSchema schema;
    if (source.get_schema(&source, &schema.s) != 0) {
        error = "failed to read the source's schema";
        return false;
    }
    std::vector<std::shared_ptr<SharedBatch>> batches;
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
        batches.push_back(std::make_shared<SharedBatch>(std::move(b)));
    }
    return retry_on_conflict(
        [&] {
            ArrowSchema copy{};
            ArrowArrayStream replay{};
            if (ArrowSchemaDeepCopy(&schema.s, &copy) != NANOARROW_OK ||
                ArrowBasicArrayStreamInit(&replay, &copy, static_cast<int64_t>(batches.size())) != NANOARROW_OK) {
                if (copy.release != nullptr) {
                    copy.release(&copy);
                }
                error = "out of memory";
                return false;
            }
            for (std::size_t i = 0; i < batches.size(); ++i) {
                ArrowArray view = slice_batch(batches[i], 0, batches[i]->array.length);
                ArrowBasicArrayStreamSetArray(&replay, static_cast<int64_t>(i), &view);
            }
            const bool ok = merge_insert_once(dataset_path, spec, replay, stats, new_version, error);
            return ok;
        },
        error);
}

bool delete_once(const std::filesystem::path& dataset_path, const std::string& predicate, std::uint64_t& deleted,
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

bool update_once(const std::filesystem::path& dataset_path, const std::string* predicate,
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
    const bool stable = is_stable(manifest);
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    request.with_row_address = true;
    request.with_row_id = stable;  // after the data columns: _rowid, then _rowaddr
    request.filter = predicate != nullptr && !predicate->empty() ? predicate : nullptr;
    OwnedSchema schema;
    OwnedBatches batches;
    if (!scan(dataset_path, request, schema, batches, error)) {
        return false;
    }
    const auto data_columns = schema.s.n_children - (stable ? 2 : 1);  // then _rowid (stable) and _rowaddr
    std::vector<std::uint64_t> moved_ids;
    std::vector<std::uint64_t> moved_addresses;
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
        const ArrowArray* addr = batch.children[data_columns + (stable ? 1 : 0)];
        const auto* values = static_cast<const std::uint64_t*>(addr->buffers[1]) + addr->offset;
        for (int64_t i = 0; i < batch.length; ++i) {
            rewritten[values[i] >> 32U].push_back(static_cast<std::uint32_t>(values[i] & 0xFFFFFFFFULL));
            if (stable) {
                moved_addresses.push_back(values[i]);
            }
        }
        if (stable) {
            const ArrowArray* idc = batch.children[data_columns];
            const auto* idv = static_cast<const std::uint64_t*>(idc->buffers[1]) + idc->offset;
            moved_ids.insert(moved_ids.end(), idv, idv + batch.length);
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
    std::vector<std::uint64_t> moved_created;
    if (stable && !created_versions_of(manifest, moved_addresses, moved_created, error)) {
        return false;
    }
    if (!apply_deletions(dataset_path, manifest, version, rewritten, error)) {
        return false;
    }
    const auto first_new = manifest.fragments.size();
    if (opened) {
        add_fragments(manifest, staged.mapping(), staged.files());
    }
    if (stable && !stamp_new_fragments(manifest, first_new, moved_ids, moved_created, version + 1U, error)) {
        return false;
    }
    manifest.operation = pb::Manifest::Operation::Update;  // its transaction: Lance's Update
    return commit(dataset_path, std::move(manifest), new_version, error);
}

// ── merge insert ────────────────────────────────────────────────────────────────────────────────

namespace {

/// A two-child struct batch {source, target} over slices it owns.
struct PairBatch {
    ArrowArray source{};
    ArrowArray target{};
    ArrowArray* children[2] = {&source, &target};
    const void* buffers[1] = {nullptr};
};

void release_pair(ArrowArray* array) {
    auto* self = static_cast<PairBatch*>(array->private_data);
    if (self->source.release != nullptr) {
        self->source.release(&self->source);
    }
    if (self->target.release != nullptr) {
        self->target.release(&self->target);
    }
    delete self;
    array->release = nullptr;
}

/// merge_insert's UPDATE_IF: for each source row matching `targets[row]` (UINT64_MAX: no match),
/// whether `condition` holds for the pair -- source columns as `source.*`, the target row's as
/// `target.*`. Evaluated over runs of consecutive source rows matching consecutive target rows.
bool update_if_passes(const std::filesystem::path& dataset_path, std::uint64_t version, const std::string& condition,
                      const ArrowSchema& dataset_schema, const std::vector<std::shared_ptr<SharedBatch>>& source_batches,
                      const std::vector<int64_t>& order, const std::vector<std::vector<std::uint64_t>>& targets,
                      std::vector<std::vector<std::uint8_t>>& passes, std::string& error) {
    if (condition.empty()) {
        error = "merge insert: when_matched UPDATE_IF needs a condition";
        return false;
    }
    expr::Expression expression;
    if (!expr::Expression::parse(condition, expression, error)) {
        return false;
    }
    std::vector<std::uint64_t> sorted;
    for (const auto& t : targets) {
        for (const auto a : t) {
            if (a != UINT64_MAX) {
                sorted.push_back(a);
            }
        }
    }
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    passes.assign(targets.size(), {});
    for (std::size_t b = 0; b < targets.size(); ++b) {
        passes[b].assign(targets[b].size(), 0U);
    }
    if (sorted.empty()) {
        return true;
    }
    // The matched target rows, every column.
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    OwnedSchema target_schema;
    OwnedBatches taken;
    if (!lance_dataset_take_rows(dataset_path, request, sorted, target_schema.s, taken.v, error)) {
        return false;
    }
    std::vector<std::shared_ptr<SharedBatch>> target_batches;
    std::vector<std::pair<std::size_t, int64_t>> where;  // sorted[k] is row .second of batch .first
    for (auto& t : taken.v) {
        target_batches.push_back(std::make_shared<SharedBatch>(std::move(t)));
        for (int64_t r = 0; r < target_batches.back()->array.length; ++r) {
            where.emplace_back(target_batches.size() - 1U, r);
        }
    }
    taken.v.clear();
    if (where.size() != sorted.size()) {
        error = "merge insert: the matched rows could not be read";
        return false;
    }
    std::vector<int64_t> identity;
    for (int64_t i = 0; i < target_schema.s.n_children; ++i) {
        identity.push_back(i);
    }
    // {source: <the dataset's columns>, target: <the dataset's columns>}
    OwnedSchema pair_schema;
    ArrowSchemaInit(&pair_schema.s);
    if (ArrowSchemaSetTypeStruct(&pair_schema.s, 2) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        ArrowSchema* child = pair_schema.s.children[i];
        child->release(child);
        if (ArrowSchemaDeepCopy(i == 0 ? &dataset_schema : &target_schema.s, child) != NANOARROW_OK ||
            ArrowSchemaSetName(child, i == 0 ? "source" : "target") != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
    }
    if (!expression.bind(pair_schema.s, error)) {
        return false;
    }
    for (std::size_t b = 0; b < targets.size(); ++b) {
        const auto& t = targets[b];
        std::size_t i = 0;
        while (i < t.size()) {
            if (t[i] == UINT64_MAX) {
                ++i;
                continue;
            }
            const auto k = static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), t[i]) - sorted.begin());
            std::size_t run = 1;
            while (i + run < t.size() && t[i + run] != UINT64_MAX) {
                const auto next =
                    static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), t[i + run]) - sorted.begin());
                if (next != k + run || where[next].first != where[k].first) {
                    break;
                }
                ++run;
            }
            auto pair = std::make_unique<PairBatch>();
            pair->source = slice_batch(source_batches[b], static_cast<int64_t>(i), static_cast<int64_t>(run), order);
            pair->target = slice_batch(target_batches[where[k].first], where[k].second, static_cast<int64_t>(run),
                                       identity);
            ArrowArray batch{};
            batch.length = static_cast<int64_t>(run);
            batch.n_buffers = 1;
            batch.n_children = 2;
            batch.buffers = pair->buffers;
            batch.children = pair->children;
            batch.release = &release_pair;
            batch.private_data = pair.release();
            std::vector<std::uint8_t> keep;
            const bool ok = expression.filter(batch, keep, error);
            batch.release(&batch);
            if (!ok) {
                return false;
            }
            std::copy(keep.begin(), keep.end(), passes[b].begin() + static_cast<std::ptrdiff_t>(i));
            i += run;
        }
    }
    return true;
}

}  // namespace

bool merge_insert_once(const std::filesystem::path& dataset_path, const MergeInsertSpec& spec,
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
    const bool stable = is_stable(manifest);
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

    using WM = MergeInsertSpec::WhenMatched;
    // UPDATE_IF: which matched source rows meet the condition.
    std::vector<std::vector<std::uint8_t>> passes;
    if (spec.when_matched == WM::UpdateIf) {
        std::vector<std::vector<std::uint64_t>> targets(source_batches.size());
        std::string k;
        for (std::size_t b = 0; b < source_batches.size(); ++b) {
            BatchView view;
            if (!view.set(source_schema.s, source_batches[b]->array, error)) {
                return false;
            }
            targets[b].assign(static_cast<std::size_t>(source_batches[b]->array.length), UINT64_MAX);
            for (int64_t i = 0; i < source_batches[b]->array.length; ++i) {
                if (key_of(view.view, source_keys, i, k)) {
                    const auto it = target.find(k);
                    if (it != target.end()) {
                        targets[b][static_cast<std::size_t>(i)] = it->second;
                    }
                }
            }
        }
        if (!update_if_passes(dataset_path, version, spec.when_matched_condition, target_schema.s, source_batches,
                              order, targets, passes, error)) {
            return false;
        }
    }

    // Which source rows are written, which target rows go.
    std::set<std::uint64_t> matched;
    std::map<std::uint64_t, std::vector<std::uint32_t>> deletions;
    StagedFiles staged;
    bool opened = false;
    std::string key;
    std::vector<std::uint64_t> kept_targets;  // stable ids: each written row's target address, or kNewRow
    for (std::size_t sb = 0; sb < source_batches.size(); ++sb) {
        const auto& shared = source_batches[sb];
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
                if (spec.when_matched == WM::Fail) {
                    error = "Merge insert failed: a source row matches an existing row, and when_matched is fail";
                    return false;
                }
                const bool update_if =
                    spec.when_matched == WM::UpdateIf && passes[sb][static_cast<std::size_t>(i)] != 0U;
                if (update_if) {
                    keep[static_cast<std::size_t>(i)] = 1U;
                    deletions[it->second >> 32U].push_back(static_cast<std::uint32_t>(it->second & 0xFFFFFFFFULL));
                    ++stats.updated;
                } else if (spec.when_matched == WM::UpdateAll || spec.when_matched == WM::Delete) {
                    keep[static_cast<std::size_t>(i)] = spec.when_matched == WM::UpdateAll ? 1U : 0U;
                    deletions[it->second >> 32U].push_back(static_cast<std::uint32_t>(it->second & 0xFFFFFFFFULL));
                    ++(spec.when_matched == WM::UpdateAll ? stats.updated : stats.deleted);
                }
                if (stable && keep[static_cast<std::size_t>(i)] != 0U) {
                    kept_targets.push_back(it->second);
                }
            } else if (spec.when_not_matched_insert_all) {
                keep[static_cast<std::size_t>(i)] = 1U;
                ++stats.inserted;
                if (stable) {
                    kept_targets.push_back(kNewRow);
                }
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
    std::vector<std::uint64_t> moved_ids;
    std::vector<std::uint64_t> moved_created;
    if (stable && !moved_row_meta(manifest, kept_targets, moved_ids, moved_created, error)) {
        return false;
    }
    if (!apply_deletions(dataset_path, manifest, version, deletions, error)) {
        return false;
    }
    const auto first_new = manifest.fragments.size();
    if (opened) {
        add_fragments(manifest, staged.mapping(), staged.files());
    }
    if (stable && !stamp_new_fragments(manifest, first_new, moved_ids, moved_created, version + 1U, error)) {
        return false;
    }
    manifest.operation = pb::Manifest::Operation::Update;  // its transaction: Lance's Update
    if (spec.uncommitted != nullptr) {
        pb::Manifest parent;
        if (!load_manifest_version(dataset_path, version, parent, error)) {
            return false;
        }
        retain_relevant_indices(manifest.indices, manifest.fields, manifest.fragments);
        auto [field, op] = derive_transaction_operation(parent, manifest);
        spec.uncommitted->read_version = version;
        spec.uncommitted->operation_field = field;
        spec.uncommitted->operation = std::move(op);
        new_version = version;
        return true;
    }
    return commit(dataset_path, std::move(manifest), new_version, error);
}

// ── columns ─────────────────────────────────────────────────────────────────────────────────────

namespace {

/// New columns from SQL expressions over version `version`, one data file per fragment of `ids` (in
/// order, every physical row) written through `staged`.
bool write_sql_columns(const std::filesystem::path& dataset_path, const pb::Manifest& manifest, std::uint64_t version,
                       const std::vector<std::uint64_t>& ids,
                       const std::vector<std::pair<std::string, std::string>>& columns, StagedFiles& staged,
                       std::string& error) {
    if (columns.empty()) {
        error = "no columns to add";
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
    return true;
}

}  // namespace

bool dataset_add_columns_sql(const std::filesystem::path& dataset_path,
                             const std::vector<std::pair<std::string, std::string>>& columns,
                             std::uint64_t& new_version, std::string& error) {
    error.clear();
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    StagedFiles staged;
    if (!staged.open(dataset_path, false, max_field_id(manifest) + 1, error)) {
        return false;
    }
    const auto ids = fragment_ids(manifest);
    if (!write_sql_columns(dataset_path, manifest, version, ids, columns, staged, error) ||
        !attach_columns(manifest, ids, staged, error)) {
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
    // A data file left holding none of the schema's fields leaves its fragment, as Lance's Project
    // drops it (the file itself stays on disk for cleanup to find).
    std::unordered_set<std::int32_t> remaining;
    for (const auto& f : manifest.fields) {
        remaining.insert(f.id);
    }
    for (auto& fragment : manifest.fragments) {
        fragment.files.erase(std::remove_if(fragment.files.begin(), fragment.files.end(),
                                            [&](const pb::DataFile& file) {
                                                return std::none_of(file.fields.begin(), file.fields.end(),
                                                                    [&](std::int32_t id) { return remaining.count(id); });
                                            }),
                             fragment.files.end());
    }
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

bool compact_once(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                  CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error);

bool dataset_compact_files(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                           CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error) {
    // Planned again from the latest version when a delete or update rewrote a fragment meanwhile.
    return retry_on_conflict([&] { return compact_once(dataset_path, options, metrics, new_version, error); },
                             error);
}

bool compact_once(const std::filesystem::path& dataset_path, const CompactionOptions& options,
                  CompactionMetrics& metrics, std::uint64_t& new_version, std::string& error) {
    error.clear();
    metrics = {};
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest(dataset_path, manifest, version, error)) {
        return false;
    }
    const bool stable = is_stable(manifest);
    new_version = version;
    for (const auto& [name, value] :
         {std::pair<const char*, std::optional<std::uint64_t>>{"max_source_fragments", options.max_source_fragments},
          {"max_source_rows", options.max_source_rows},
          {"max_source_bytes", options.max_source_bytes}}) {
        if (value && *value == 0U) {
            error = std::string("Invalid user input: CompactionOptions::") + name +
                    " must be greater than 0 (use None for no limit)";
            return false;
        }
    }
    const pb::Manifest original = manifest;
    // A threshold of 100% is no materializing at all (Lance's CompactionOptions::validate).
    const bool materialize = options.materialize_deletions && options.materialize_deletions_threshold < 1.0;
    const std::set<std::uint64_t> excluded(options.excluded_fragment_ids.begin(), options.excluded_fragment_ids.end());
    const auto target = std::max<std::uint64_t>(options.target_rows_per_fragment, 1U);
    auto rows_of = [](const pb::DataFragment& f) {
        return f.physical_rows - (f.deletion_file.present ? f.deletion_file.num_deleted_rows : 0U);
    };
    // Lance's candidacy: a fragment with more of its rows deleted than the threshold is rewritten even
    // alone; one with fewer physical rows than the target, only together with neighbours.
    auto itself = [&](const pb::DataFragment& f) {
        return materialize && f.deletion_file.present && f.physical_rows != 0U &&
               static_cast<float>(f.deletion_file.num_deleted_rows) / static_cast<float>(f.physical_rows) >
                   static_cast<float>(options.materialize_deletions_threshold);
    };
    auto candidate = [&](const pb::DataFragment& f) { return itself(f) || f.physical_rows < target; };
    // The indexes covering a fragment: Lance never puts fragments covered by different ones in one bin
    // (an indexed fragment with an unindexed one).
    auto covering = [&](const pb::DataFragment& f) {
        std::vector<std::size_t> out;
        for (std::size_t k = 0; k < manifest.indices.size(); ++k) {
            const auto& ids = manifest.indices[k].fragment_ids;
            if (std::binary_search(ids.begin(), ids.end(), static_cast<std::uint32_t>(f.id))) {
                out.push_back(k);
            }
        }
        return out;
    };
    // Lance's planner: runs of adjacent candidates with the same index coverage (an excluded
    // fragment, or one that is no candidate, ends a run); a run of one fragment that only wants
    // neighbours is dropped; the rest are split where a piece has reached the target and what is
    // left can reach it too (split_for_size), so a task may hold more than the target, never less
    // when it can be avoided.
    std::vector<std::vector<std::size_t>> runs;
    std::vector<std::size_t> run;
    std::vector<std::size_t> run_indices;
    auto close_run = [&] {
        if (run.size() > 1U || (run.size() == 1U && itself(manifest.fragments[run.front()]))) {
            runs.push_back(run);
        }
        run.clear();
    };
    for (std::size_t i = 0; i < manifest.fragments.size(); ++i) {
        const auto& f = manifest.fragments[i];
        if (excluded.count(f.id) != 0U || !candidate(f)) {
            close_run();
            continue;
        }
        auto indices = covering(f);
        if (!run.empty() && indices != run_indices) {
            close_run();
        }
        run_indices = std::move(indices);
        run.push_back(i);
    }
    close_run();
    std::vector<std::vector<std::size_t>> bins;
    for (const auto& r : runs) {
        std::uint64_t remaining = 0;
        for (const auto m : r) {
            remaining += rows_of(manifest.fragments[m]);
        }
        std::vector<std::size_t> piece;
        std::uint64_t piece_rows = 0;
        for (const auto m : r) {
            const auto rows = rows_of(manifest.fragments[m]);
            piece.push_back(m);
            piece_rows += rows;
            remaining -= rows;
            if (piece_rows >= target && remaining > 0U && remaining >= target) {
                bins.push_back(std::move(piece));
                piece.clear();
                piece_rows = 0;
            }
        }
        if (!piece.empty()) {
            bins.push_back(std::move(piece));
        }
    }
    // Lance's source budgets: whole tasks, in order, until the next one would exceed any of them.
    if (options.max_source_fragments || options.max_source_rows || options.max_source_bytes) {
        std::set<std::int32_t> schema_ids;
        for (const auto& field : manifest.fields) {
            schema_ids.insert(field.id);
        }
        std::uint64_t fragments = 0;
        std::uint64_t rows = 0;
        std::uint64_t bytes = 0;
        std::size_t keep = 0;
        for (; keep < bins.size(); ++keep) {
            for (const auto m : bins[keep]) {
                const auto& f = manifest.fragments[m];
                ++fragments;
                rows += rows_of(f);
                for (const auto& file : f.files) {
                    // A file of dropped columns only is not read, so not counted.
                    if (std::any_of(file.fields.begin(), file.fields.end(),
                                    [&](std::int32_t id) { return schema_ids.count(id) != 0U; })) {
                        bytes += file.file_size_bytes;
                    }
                }
            }
            if ((options.max_source_fragments && fragments > *options.max_source_fragments) ||
                (options.max_source_rows && rows > *options.max_source_rows) ||
                (options.max_source_bytes && bytes > *options.max_source_bytes)) {
                break;
            }
        }
        bins.resize(keep);
    }
    if (bins.empty()) {
        return true;
    }
    std::vector<pb::DataFragment> created;  // ids assigned once reserved
    std::vector<std::uint64_t> rewritten_ids;
    std::size_t next_bin = 0;
    const std::vector<pb::DataFragment> old = manifest.fragments;
    const std::int64_t batch_rows = static_cast<std::int64_t>(options.batch_size != 0U ? options.batch_size : 8192U);
    for (std::size_t i = 0; i < old.size();) {
        if (next_bin < bins.size() && bins[next_bin].front() == i) {
            const auto& members = bins[next_bin];
            std::vector<std::uint64_t> ids;
            for (const auto m : members) {
                ids.push_back(old[m].id);
                rewritten_ids.push_back(old[m].id);
                // Lance counts a fragment's deletion file among the files a compaction removes.
                metrics.files_removed += old[m].files.size() + (old[m].deletion_file.present ? 1U : 0U);
            }
            metrics.fragments_removed += members.size();
            std::vector<std::uint64_t> bin_ids;
            std::vector<std::uint64_t> bin_created;
            std::vector<std::uint64_t> bin_updated;
            if (stable) {
                for (const auto m : members) {
                    if (!live_row_meta(dataset_path, old[m], bin_ids, bin_created, bin_updated, error)) {
                        return false;
                    }
                }
            }
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
            staged.set_max_bytes(options.max_bytes_per_file);
            for (auto& b : batches.v) {
                if (b.length == 0) {
                    continue;
                }
                if (options.max_bytes_per_file == 0U || b.length <= batch_rows) {
                    if (!staged.write(b, schema.s, error)) {
                        return false;
                    }
                    continue;
                }
                // `batch_size` rows at a time, so the byte limit can cut between them.
                auto shared = std::make_shared<SharedBatch>(std::move(b));
                for (std::int64_t at = 0; at < shared->array.length; at += batch_rows) {
                    ArrowArray piece = slice_batch(shared, at, std::min(batch_rows, shared->array.length - at));
                    const bool ok = staged.write(piece, schema.s, error);
                    if (piece.release != nullptr) {
                        piece.release(&piece);
                    }
                    if (!ok) {
                        return false;
                    }
                }
            }
            if (!staged.finish(error)) {
                return false;
            }
            const auto first_created = created.size();
            for (const auto& f : staged.files()) {
                if (f.rows == 0U) {
                    continue;
                }
                created.push_back(make_data_fragment(staged.mapping(), f, 0));
                ++metrics.fragments_added;
                ++metrics.files_added;
            }
            if (stable) {
                std::uint64_t unused_next_row_id = 0;  // every row of a compaction keeps its id
                if (!stamp_fragments(created, first_created, bin_ids, bin_created, bin_updated, unused_next_row_id,
                                     version + 1U, error)) {
                    return false;
                }
            }
            i = members.back() + 1U;
            ++next_bin;
        } else {
            ++i;
        }
    }

    // As Lance commits a compaction: first a version reserving the new fragments' ids, then the
    // rewrite, made on top of the reserving version. A fragment it rewrote that a writer changed in
    // between makes it a conflict, and it is planned again.
    pb::Manifest reserve = original;
    reserve.operation = pb::Manifest::Operation::Reserve;
    reserve.reserved_fragments = static_cast<std::uint32_t>(created.size());
    if (!created.empty()) {
        note_fragment_id(reserve, next_fragment_id(original) + created.size() - 1U);
    }
    std::uint64_t reserved_version = 0;
    if (!commit_next_version(dataset_path, std::move(reserve), reserved_version, error)) {
        return false;
    }
    new_version = reserved_version;
    if (!load_manifest_version(dataset_path, reserved_version, manifest, error)) {
        return false;
    }
    for (const auto rewritten : rewritten_ids) {
        const auto now = std::find_if(manifest.fragments.begin(), manifest.fragments.end(),
                                      [&](const pb::DataFragment& f) { return f.id == rewritten; });
        const auto then = std::find_if(original.fragments.begin(), original.fragments.end(),
                                       [&](const pb::DataFragment& f) { return f.id == rewritten; });
        if (now == manifest.fragments.end() || pb::encode_data_fragment(*now) != pb::encode_data_fragment(*then)) {
            error = "commit conflict: fragment " + std::to_string(rewritten) + " changed during the compaction";
            return false;
        }
    }
    std::uint64_t id = manifest.max_fragment_id + 1U - created.size();
    std::erase_if(manifest.fragments, [&](const pb::DataFragment& f) {
        return std::find(rewritten_ids.begin(), rewritten_ids.end(), f.id) != rewritten_ids.end();
    });
    for (auto& f : created) {
        f.id = id++;
        manifest.fragments.push_back(std::move(f));
    }
    const bool deletions = std::any_of(manifest.fragments.begin(), manifest.fragments.end(),
                                       [](const pb::DataFragment& f) { return f.deletion_file.present; });
    if (!deletions) {
        manifest.reader_feature_flags &= ~pb::kFlagDeletionFiles;
        manifest.writer_feature_flags &= ~pb::kFlagDeletionFiles;
    }
    // An index keeps covering the fragments the compaction left alone. Those it rewrote leave its
    // coverage -- it points at their rows' old addresses -- and Lance scans them instead.
    std::vector<std::string> affected;
    for (const auto& index : manifest.indices) {
        const bool touched = std::any_of(index.fragment_ids.begin(), index.fragment_ids.end(), [&](std::uint32_t id) {
            return std::find(rewritten_ids.begin(), rewritten_ids.end(), id) != rewritten_ids.end();
        });
        if (touched && !is_system_index(index) &&
            std::find(affected.begin(), affected.end(), index.name) == affected.end()) {
            affected.push_back(index.name);
        }
    }
    drop_fragments_from_indices(manifest.indices, rewritten_ids);
    manifest.operation = pb::Manifest::Operation::Rewrite;
    if (!commit(dataset_path, std::move(manifest), new_version, error, false)) {
        return false;
    }
    if (!options.reindex || affected.empty()) {
        return true;
    }
    // The indexes take the compacted fragments back in: all together, or one by one when one of them
    // cannot be rebuilt (its coverage then stays narrower; the rows are scanned, never lost).
    OptimizeIndicesOptions reindex;
    reindex.index_names = affected;
    OptimizeIndicesResult done;
    std::string why;
    if (dataset_optimize_indices(dataset_path, reindex, done, why)) {
        metrics.indexes_reindexed = done.optimized;
        new_version = done.version;
        return true;
    }
    for (const auto& name : affected) {
        reindex.index_names = {name};
        if (dataset_optimize_indices(dataset_path, reindex, done, why)) {
            metrics.indexes_reindexed.insert(metrics.indexes_reindexed.end(), done.optimized.begin(),
                                             done.optimized.end());
            new_version = done.version;
        } else {
            metrics.indexes_not_reindexed.push_back(name);
        }
    }
    return true;
}


// ── one fragment, uncommitted ────────────────────────────────────────────────────────────────────

namespace {

bool fragment_of(const std::filesystem::path& path, std::uint64_t version, std::uint64_t fragment_id,
                 pb::Manifest& manifest, pb::DataFragment*& fragment, std::string& error) {
    if (!load_manifest_version(path, version, manifest, error)) {
        return false;
    }
    fragment = nullptr;
    for (auto& f : manifest.fragments) {
        if (f.id == fragment_id) {
            fragment = &f;
        }
    }
    if (fragment == nullptr) {
        error = "Fragment " + std::to_string(fragment_id) + " not found";
        return false;
    }
    return true;
}

}  // namespace

bool fragment_delete_rows(const std::filesystem::path& dataset_path, std::uint64_t version, std::uint64_t fragment_id,
                          const std::string* predicate, const std::vector<std::uint32_t>& offsets,
                          std::vector<std::uint8_t>& fragment_out, bool& emptied, std::string& error) {
    error.clear();
    emptied = false;
    pb::Manifest manifest;
    pb::DataFragment* fragment = nullptr;
    if (!fragment_of(dataset_path, version, fragment_id, manifest, fragment, error)) {
        return false;
    }
    std::vector<std::uint32_t> rows;
    if (!read_deletion_vector(dataset_path, fragment->id, fragment->deletion_file, rows, error)) {
        return false;
    }
    const auto before = rows.size();
    if (predicate != nullptr) {
        LanceScanRequest request;
        request.has_version = true;
        request.version = version;
        const std::vector<std::string> none;
        request.columns = &none;
        request.with_row_address = true;
        request.filter = predicate;
        const std::vector<std::uint64_t> only{fragment_id};
        request.fragment_ids = &only;
        OwnedSchema schema;
        OwnedBatches batches;
        if (!scan(dataset_path, request, schema, batches, error)) {
            return false;
        }
        for (const auto& b : batches.v) {
            const ArrowArray* addr = b.children[b.n_children - 1];
            const auto* values = static_cast<const std::uint64_t*>(addr->buffers[1]) + addr->offset;
            for (int64_t i = 0; i < b.length; ++i) {
                rows.push_back(static_cast<std::uint32_t>(values[i] & 0xFFFFFFFFULL));
            }
        }
    } else {
        for (const auto o : offsets) {
            if (o >= fragment->physical_rows) {
                error = "Invalid user input: row offset " + std::to_string(o) + " is out of range for fragment " +
                        std::to_string(fragment_id) + " with " + std::to_string(fragment->physical_rows) +
                        " physical rows";
                return false;
            }
            rows.push_back(o);
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    if (rows.size() >= fragment->physical_rows) {
        emptied = true;
        return true;
    }
    if (rows.size() != before &&
        !write_deletion_file(dataset_path, fragment->id, version, rows, fragment->deletion_file, error)) {
        return false;
    }
    fragment_out = pb::encode_data_fragment(*fragment);
    return true;
}

bool fragment_add_columns_sql(const std::filesystem::path& dataset_path, std::uint64_t version,
                              std::uint64_t fragment_id,
                              const std::vector<std::pair<std::string, std::string>>& columns,
                              std::int32_t max_field_id, std::vector<std::uint8_t>& fragment_out,
                              std::vector<std::vector<std::uint8_t>>& new_fields, std::string& error) {
    error.clear();
    new_fields.clear();
    pb::Manifest manifest;
    pb::DataFragment* fragment = nullptr;
    if (!fragment_of(dataset_path, version, fragment_id, manifest, fragment, error)) {
        return false;
    }
    StagedFiles staged;
    if (!staged.open(dataset_path, false, std::max(max_field_id, nano_lance::max_field_id(manifest)) + 1, error) ||
        !write_sql_columns(dataset_path, manifest, version, {fragment_id}, columns, staged, error)) {
        return false;
    }
    if (staged.files().size() != 1U || staged.files().front().rows != fragment->physical_rows) {
        error = "the new columns do not cover fragment " + std::to_string(fragment_id);
        return false;
    }
    fragment->files.push_back(make_data_fragment(staged.mapping(), staged.files().front(), fragment->id).files.front());
    for (const auto& f : staged.mapping().fields) {
        new_fields.push_back(pb::encode_field(make_manifest_field(f)));
    }
    fragment_out = pb::encode_data_fragment(*fragment);
    return true;
}

bool fragment_write_columns(const std::filesystem::path& dataset_path, std::uint64_t version,
                            std::uint64_t fragment_id, ArrowArrayStream& stream, bool replace,
                            std::int32_t max_field_id, std::vector<std::uint8_t>& fragment_out,
                            std::vector<std::vector<std::uint8_t>>& new_fields,
                            std::vector<std::int32_t>& fields_written, std::string& error) {
    struct Release {
        ArrowArrayStream* s;
        ~Release() {
            if (s->release != nullptr) {
                s->release(s);
            }
        }
    } release{&stream};
    error.clear();
    new_fields.clear();
    fields_written.clear();
    pb::Manifest manifest;
    pb::DataFragment* fragment = nullptr;
    if (!fragment_of(dataset_path, version, fragment_id, manifest, fragment, error)) {
        return false;
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
    if (!replace && !check_new_names(manifest, names, error)) {
        return false;
    }
    StagedFiles staged;
    if (!staged.open(dataset_path, replace, std::max(max_field_id, nano_lance::max_field_id(manifest)) + 1, error)) {
        return false;
    }
    if (replace && !staged.project(names, error)) {
        return false;
    }
    std::uint64_t rows = 0;
    for (;;) {
        ArrowArray b{};
        if (stream.get_next(&stream, &b) != 0) {
            error = "reading the stream failed";
            return false;
        }
        if (b.release == nullptr) {
            break;
        }
        rows += static_cast<std::uint64_t>(b.length);
        const bool ok = staged.write(b, schema.s, error);
        if (b.release != nullptr) {
            b.release(&b);
        }
        if (!ok) {
            return false;
        }
    }
    if (rows != fragment->physical_rows) {
        error = "the new columns have " + std::to_string(rows) + " rows for fragment " + std::to_string(fragment_id) +
                " of " + std::to_string(fragment->physical_rows);
        return false;
    }
    if (!staged.cut(error, true)) {
        return false;
    }
    if (staged.files().size() != 1U) {
        error = "wrote " + std::to_string(staged.files().size()) + " files for one fragment";
        return false;
    }
    auto file = make_data_fragment(staged.mapping(), staged.files().front(), fragment->id).files.front();
    for (const auto& f : staged.mapping().fields) {
        fields_written.push_back(f.id);
        if (!replace) {
            new_fields.push_back(pb::encode_field(make_manifest_field(f)));
        }
    }
    if (replace) {
        const std::set<std::int32_t> rewritten(fields_written.begin(), fields_written.end());
        for (auto& old : fragment->files) {
            for (auto& id : old.fields) {
                if (rewritten.count(id) != 0U) {
                    id = -2;  // tombstoned: the new file answers for it
                }
            }
        }
    }
    fragment->files.push_back(std::move(file));
    fragment_out = pb::encode_data_fragment(*fragment);
    return true;
}

}  // namespace nano_lance
