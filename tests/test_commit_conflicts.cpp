// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lance 12's verdict (check_txn) for pairs of operations, as nanolance classifies a commit that lost its
// race: compatible, retryable, or incompatible. Operations are built by hand; the rows of the
// pylance probe (CONFLICTS.md section 2) are checked through the same verdicts in the Python tests.
#include "commit_conflicts.hpp"

#include "nanolance/dataset.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/dataset_ops.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace pb = nano_lance::pb;
namespace fs = std::filesystem;
using nano_lance::Operation;
using nano_lance::Verdict;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

Operation op(std::uint32_t kind) {
    Operation o;
    o.kind = kind;
    return o;
}

pb::DataFragment fragment(std::uint64_t id) {
    pb::DataFragment f;
    f.id = id;
    return f;
}

/// A Delete (or Update) that removes or changes the given existing fragments.
Operation delete_of(std::uint32_t kind, std::initializer_list<std::uint64_t> fragments) {
    Operation o = op(kind);
    for (const auto id : fragments) {
        o.updated_fragments.push_back(fragment(id));
    }
    return o;
}

Operation rewrite_of(std::initializer_list<std::uint64_t> old_fragments) {
    Operation o = op(nano_lance::kRewrite);
    nano_lance::RewriteGroup group;
    for (const auto id : old_fragments) {
        group.old_fragments.push_back(fragment(id));
    }
    o.groups.push_back(std::move(group));
    return o;
}

pb::IndexMetadata index_named(const std::string& name, std::initializer_list<std::uint32_t> covers) {
    pb::IndexMetadata index;
    index.name = name;
    index.has_fragment_bitmap = true;
    index.fragment_ids.assign(covers.begin(), covers.end());
    return index;
}

Operation create_index(const std::string& name, std::initializer_list<std::uint32_t> covers) {
    Operation o = op(nano_lance::kCreateIndex);
    o.new_indices.push_back(index_named(name, covers));
    return o;
}

nano_lance::UpdateMap config_key(const std::string& key, const std::string& value) {
    nano_lance::UpdateMap m;
    m.entries.emplace_back(key, value);
    return m;
}

Operation update_config(const std::string& key, const std::string& value) {
    Operation o = op(nano_lance::kUpdateConfig);
    o.config_updates = config_key(key, value);
    return o;
}

Operation overwrite_with(const std::string& key) {
    Operation o = op(nano_lance::kOverwrite);
    o.config_upserts[key] = "x";
    return o;
}

void expect(Verdict got, Verdict want, const std::string& what) {
    check(got == want, what + ": got verdict " + std::to_string(static_cast<int>(got)) + ", want " +
                           std::to_string(static_cast<int>(want)));
}

// Section 2 rows (the ones decided by the classifier; the outcome of each is checked in Python).
void probe_rows() {
    using V = Verdict;
    // delete id=2 vs delete id=1 (same fragment): overlap, no affected rows -> retryable (re-run).
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {0}), delete_of(nano_lance::kDelete, {0})), V::Retryable,
           "delete vs delete, same fragment");
    // delete id=70 vs delete id=1 (other fragment) -> compatible.
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {1}), delete_of(nano_lance::kDelete, {0})),
           V::Compatible, "delete vs delete, other fragment");
    // update vs delete on the same fragment -> retryable; update vs delete on another -> compatible.
    expect(nano_lance::check_txn(delete_of(nano_lance::kUpdate, {0}), delete_of(nano_lance::kDelete, {0})), V::Retryable,
           "update vs delete, same fragment");
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {0}), delete_of(nano_lance::kUpdate, {1})),
           V::Compatible, "delete vs update, other fragment");
    // delete vs overwrite -> incompatible.
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {0}), op(nano_lance::kOverwrite)), V::Incompatible,
           "delete vs overwrite");
    // delete vs compaction of its fragment -> retryable; of another -> compatible.
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {0}), rewrite_of({0})), V::Retryable,
           "delete vs compaction, same fragment");
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {0}), rewrite_of({1})), V::Compatible,
           "delete vs compaction, other fragment");
    // compaction vs delete of its fragment -> retryable.
    expect(nano_lance::check_txn(rewrite_of({0}), delete_of(nano_lance::kDelete, {0})), V::Retryable,
           "compaction vs delete, same fragment");
    // create index vs delete -> compatible (rebased by C2); create index vs compaction of its fragments -> retryable.
    expect(nano_lance::check_txn(create_index("id_idx", {0}), delete_of(nano_lance::kDelete, {0})), V::Compatible,
           "create index vs delete");
    expect(nano_lance::check_txn(create_index("id_idx", {0}), rewrite_of({0})), V::Retryable,
           "create index vs compaction of its fragment");
    // create index vs create index: a new name -> compatible; the same name -> retryable.
    expect(nano_lance::check_txn(create_index("id_idx", {0}), create_index("v_idx", {0})), V::Compatible,
           "create index vs create index, other name");
    expect(nano_lance::check_txn(create_index("id_idx", {0}), create_index("id_idx", {0})), V::Retryable,
           "create index vs create index, same name");
    // add columns (a Merge) vs delete -> retryable; drop columns (a Project) vs append -> compatible.
    Operation merge = op(nano_lance::kMerge);
    merge.preserves_nullability = true;
    expect(nano_lance::check_txn(merge, delete_of(nano_lance::kDelete, {0})), V::Retryable, "merge vs delete");
    Operation project = op(nano_lance::kProject);
    project.preserves_nullability = true;
    expect(nano_lance::check_txn(project, op(nano_lance::kAppend)), V::Compatible, "project vs append");
    // update_config k=2 vs update_config k=1 -> compatible; the same key -> incompatible.
    expect(nano_lance::check_txn(update_config("k2", "b"), update_config("k1", "a")), V::Compatible,
           "update_config, other keys");
    expect(nano_lance::check_txn(update_config("k", "b"), update_config("k", "a")), V::Incompatible,
           "update_config, same key");
    // restore vs append -> compatible.
    expect(nano_lance::check_txn(op(nano_lance::kRestore), op(nano_lance::kAppend)), V::Compatible, "restore vs append");
}

void incompatible_and_retryable_cases() {
    using V = Verdict;
    // The delete raced by an overwrite: incompatible, whatever the fragments.
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {2}), op(nano_lance::kOverwrite)), V::Incompatible,
           "delete raced by overwrite");
    // An overwrite and a config update on one key: incompatible.
    expect(nano_lance::check_txn(update_config("k", "b"), overwrite_with("k")), V::Incompatible,
           "update_config vs overwrite on its key");
    expect(nano_lance::check_txn(update_config("k", "b"), overwrite_with("other")), V::Compatible,
           "update_config vs overwrite on another key");
    // A non-preserving projection against a concurrent append: retryable (values may be null).
    Operation tighten = op(nano_lance::kProject);
    tighten.preserves_nullability = false;
    expect(nano_lance::check_txn(tighten, op(nano_lance::kAppend)), V::Retryable, "tightening project vs append");
    // A merge against a metadata update: retryable.
    Operation metadata = op(nano_lance::kUpdateConfig);
    metadata.schema_metadata_updates = config_key("m", "v");
    Operation merge = op(nano_lance::kMerge);
    merge.preserves_nullability = true;
    expect(nano_lance::check_txn(merge, metadata), V::Retryable, "merge vs schema metadata update");
    // Clone is compatible with everything.
    expect(nano_lance::check_txn(delete_of(nano_lance::kDelete, {0}), op(nano_lance::kCloneField)), V::Compatible,
           "delete vs clone");
}

// ── two writers: a change made from version 1 after another writer committed ────────────────────

/// A fresh dataset of ten int64 ids (`id`), one fragment: version 1.
bool create_dataset(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    std::vector<std::int64_t> vals(10);
    for (std::size_t i = 0; i < vals.size(); ++i) {
        vals[i] = static_cast<std::int64_t>(i);
    }
    NanoLanceWriter w{};
    if (nano_lance_writer_init(&w, path.string().c_str(), 3) != NANO_LANCE_OK) {
        return false;
    }
    ArrowArray batch{};
    batch.length = static_cast<std::int64_t>(vals.size());
    const void* buffers[] = {nullptr, vals.data()};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = "l";  // int64
    field.name = "id";
    bool ok = nano_lance_write_batch(&w, &batch, &field) == NANO_LANCE_OK;
    ok = ok && nano_lance_writer_commit(&w, /*is_append=*/false) == NANO_LANCE_OK;
    return nano_lance_writer_close(&w) == NANO_LANCE_OK && ok;
}

/// Remove the transaction files of the commits made from `read_version` (named `<read_version>-<uuid>.txn`).
void remove_transaction_files(const fs::path& path, std::uint64_t read_version) {
    const std::string prefix = std::to_string(read_version) + "-";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(path / "_transactions", ec)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            fs::remove(entry.path(), ec);
        }
    }
}

void incompatible_is_not_rerun() {
    const fs::path path = "commit_conflicts_incompatible";
    check(create_dataset(path), "create the dataset");
    std::uint64_t version = 0;
    std::string error;
    // Theirs commits version 2 (k = 1); ours is made from version 1 (k = 2): the same key, incompatible.
    check(nano_lance::dataset_update_config(path, {{"k", "1"}}, {}, version, error) && version == 2,
          "theirs: update_config commits version 2");
    nano_lance::reset_commit_conflict_stats();
    check(!nano_lance::dataset_update_config(path, {{"k", "2"}}, {}, version, error, 1U),
          "ours from version 1: refused");
    check(error == "Incompatible transaction: This UpdateConfig transaction is incompatible with concurrent "
                   "transaction UpdateConfig at version 2.",
          "update_config on the same key: Lance's message, got: " + error);
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.retried == 0 && stats.refused == 1 && stats.rebased == 0,
          "an incompatible verdict is not run again (retried 0, refused 1)");
    std::error_code ec;
    fs::remove_all(path, ec);
}

void delete_raced_by_restore_is_not_rerun() {
    const fs::path path = "commit_conflicts_restore";
    check(create_dataset(path), "create the dataset");
    std::uint64_t version = 0;
    std::uint64_t deleted = 0;
    std::string error;
    check(nano_lance::dataset_restore(path, 1, version, error) && version == 2, "theirs: restore commits version 2");
    nano_lance::reset_commit_conflict_stats();
    check(!nano_lance::dataset_delete(path, "id = 2", deleted, version, error, 1U), "ours from version 1: refused");
    check(error.rfind("Incompatible transaction: This Delete transaction is incompatible with concurrent "
                      "transaction Restore at version 2.", 0) == 0,
          "delete raced by a restore: Lance's message, got: " + error);
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.retried == 0 && stats.refused == 1, "delete raced by a restore is not run again");
    std::error_code ec;
    fs::remove_all(path, ec);
}

void version_without_transaction_file_is_retryable() {
    const fs::path path = "commit_conflicts_no_transaction_file";
    check(create_dataset(path), "create the dataset");
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_update_config(path, {{"k", "1"}}, {}, version, error) && version == 2,
          "theirs: update_config commits version 2");
    remove_transaction_files(path, 1);  // what a version written before transaction files looks like to us
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_update_config(path, {{"k", "2"}}, {}, version, error, 1U) && version == 3,
          "ours from version 1 is re-run on the latest and committed: " + error);
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.retried == 1 && stats.refused == 0, "a version without a transaction file is retryable");
    std::error_code ec;
    fs::remove_all(path, ec);
}


/// Append `count` rows (ids from `first`) to the dataset at `path`, as a writer commits one.
bool append_ids(const fs::path& path, std::int64_t first, std::size_t count) {
    std::vector<std::int64_t> vals(count);
    for (std::size_t i = 0; i < count; ++i) {
        vals[i] = first + static_cast<std::int64_t>(i);
    }
    NanoLanceWriter w{};
    if (nano_lance_writer_init(&w, path.string().c_str(), 3) != NANO_LANCE_OK) {
        return false;
    }
    ArrowArray batch{};
    batch.length = static_cast<std::int64_t>(vals.size());
    const void* buffers[] = {nullptr, vals.data()};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = "l";  // int64
    field.name = "id";
    bool ok = nano_lance_write_batch(&w, &batch, &field) == NANO_LANCE_OK;
    ok = ok && nano_lance_writer_commit(&w, /*is_append=*/true) == NANO_LANCE_OK;
    return nano_lance_writer_close(&w) == NANO_LANCE_OK && ok;
}

/// The fragment ids of the latest manifest.
std::vector<std::uint64_t> latest_fragment_ids(const fs::path& path) {
    pb::Manifest m;
    std::uint64_t v = 0;
    std::string error;
    check(nano_lance::load_latest_manifest(path, m, v, error), "load the latest manifest: " + error);
    std::vector<std::uint64_t> ids;
    for (const auto& f : m.fragments) {
        ids.push_back(f.id);
    }
    return ids;
}

/// The fragment ids the index `name` covers, at the latest version.
std::vector<std::uint32_t> index_fragment_ids(const fs::path& path, const std::string& name) {
    std::vector<nano_lance::IndexInfo> infos;
    std::string error;
    check(nano_lance::dataset_list_indices(path, false, 0, infos, error), "list indices: " + error);
    for (const auto& info : infos) {
        if (info.name == name) {
            return info.fragment_ids;
        }
    }
    check(false, "index " + name + " is listed");
    return {};
}

/// Ours (made from version 1) is rebased, not run again, when theirs is a compatible change. `check_result`
/// looks at the outcome.
void check_rebased(const char* what) {
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.rebased == 1 && stats.retried == 0 && stats.refused == 0,
          std::string(what) + ": rebased, not run again (rebased 1, retried 0, refused 0)");
}

void delete_rebased_over_append() {
    const fs::path path = "commit_conflicts_delete_append";
    check(create_dataset(path), "create the dataset");
    check(append_ids(path, 100, 5), "theirs: append commits version 2");
    nano_lance::reset_commit_conflict_stats();
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 2", deleted, version, error, 1U) && version == 3 && deleted == 1,
          "ours from version 1 is committed as version 3: " + error);
    check_rebased("delete over append");
    check(latest_fragment_ids(path) == std::vector<std::uint64_t>({0, 1}),
          "the rebased delete keeps the appended fragment");
    std::error_code ec;
    fs::remove_all(path, ec);
}

void update_config_rebased_over_delete() {
    const fs::path path = "commit_conflicts_config_delete";
    check(create_dataset(path), "create the dataset");
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 3", deleted, version, error) && version == 2,
          "theirs: delete commits version 2");
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_update_config(path, {{"b", "1"}}, {}, version, error, 1U) && version == 3,
          "ours (other key) from version 1 is committed as version 3: " + error);
    check_rebased("update_config over delete");
    std::error_code ec;
    fs::remove_all(path, ec);
}

void create_index_rebased_over_append() {
    const fs::path path = "commit_conflicts_index_append";
    check(create_dataset(path), "create the dataset");
    check(append_ids(path, 100, 5), "theirs: append commits version 2");
    nano_lance::reset_commit_conflict_stats();
    nano_lance::ScalarIndexOptions options;
    options.name = "id_idx";
    options.read_version = 1U;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_create_scalar_index(path, "id", nano_lance::ScalarIndexType::BTree, options, version,
                                                  error) && version == 3,
          "ours (built from version 1) is published as version 3: " + error);
    check_rebased("create index over append");
    // The index covers the fragments it read (fragment 0), not the one theirs appended.
    check(index_fragment_ids(path, "id_idx") == std::vector<std::uint32_t>({0}),
          "the index covers only the fragments it was built from");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Lance also prunes the rewritten fragment here (its fields_modified names the assigned column); nanolance's
/// derived fields_modified does not (see the report).
void create_index_rebased_over_update_of_its_field() {
    const fs::path path = "commit_conflicts_index_update";
    check(create_dataset(path), "create the dataset");
    std::uint64_t updated = 0;
    std::uint64_t version = 0;
    std::string error;
    const std::string predicate = "id = 2";
    check(nano_lance::dataset_update(path, &predicate, {{"id", "id + 100"}}, updated, version, error) && version == 2,
          "theirs: update of the indexed column commits version 2: " + error);
    nano_lance::reset_commit_conflict_stats();
    nano_lance::ScalarIndexOptions options;
    options.name = "id_idx";
    options.read_version = 1U;
    check(nano_lance::dataset_create_scalar_index(path, "id", nano_lance::ScalarIndexType::BTree, options, version,
                                                  error) && version == 3,
          "ours (built from version 1) is published as version 3: " + error);
    check_rebased("create index over update of its field");
    // The index covers only fragments it was built from (fragment 0; theirs' new rows are in a fragment
    // the index does not cover, and the deleted rows of fragment 0 are masked by its deletion file).
    const auto ids = index_fragment_ids(path, "id_idx");
    check(ids.size() <= 1U && (ids.empty() || ids[0] == 0U), "the index covers only the fragment it was built from");
    std::error_code ec;
    fs::remove_all(path, ec);
}

void restore_rebased_over_append() {
    const fs::path path = "commit_conflicts_restore_append";
    check(create_dataset(path), "create the dataset");
    check(append_ids(path, 100, 5), "theirs: append commits version 2");
    nano_lance::reset_commit_conflict_stats();
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_restore(path, 1, version, error, 1U) && version == 3,
          "ours (restore of version 1, made from version 1) is committed as version 3: " + error);
    check_rebased("restore over append");
    check(latest_fragment_ids(path) == std::vector<std::uint64_t>({0}),
          "the restore publishes the version it restores");
    std::error_code ec;
    fs::remove_all(path, ec);
}

}  // namespace

int main() {
    probe_rows();
    incompatible_and_retryable_cases();
    incompatible_is_not_rerun();
    delete_raced_by_restore_is_not_rerun();
    version_without_transaction_file_is_retryable();
    delete_rebased_over_append();
    update_config_rebased_over_delete();
    create_index_rebased_over_append();
    create_index_rebased_over_update_of_its_field();
    restore_rebased_over_append();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("commit conflict verdicts: ok\n");
    return 0;
}
