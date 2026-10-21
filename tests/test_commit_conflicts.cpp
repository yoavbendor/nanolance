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
#include "nanolance/deletion_vector.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <limits>
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

/// Pylance 12 writes a version's transaction inline in its manifest (field 21) as well as in a file.
/// Rewrite version 2 that way and drop its file: the race is then decided by the inline transaction
/// (Lance's read order) rather than as a version without one, which is retryable.
void inline_transaction_decides_the_verdict() {
    const fs::path path = "commit_conflicts_inline_transaction";
    check(create_dataset(path), "create the dataset");
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_update_config(path, {{"k", "1"}}, {}, version, error) && version == 2,
          "theirs: update_config commits version 2");
    pb::Manifest theirs;
    check(nano_lance::load_manifest_version(path, 2, theirs, error) && !theirs.transaction_file.empty(),
          "version 2 names its transaction file");
    std::vector<std::uint8_t> transaction;
    {
        std::ifstream in(path / "_transactions" / theirs.transaction_file, std::ios::binary);
        transaction.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    check(!transaction.empty(), "version 2's transaction file is read");
    std::vector<std::uint8_t> protobuf;
    // nanolance writes V2 names: 20 digits of u64::max - version.
    const fs::path manifest_2 =
        path / "_versions" / (std::to_string(std::numeric_limits<std::uint64_t>::max() - 2) + ".manifest");
    const bool read_ok = nano_lance::read_manifest_file(manifest_2, protobuf, error);
    check(read_ok, "read version 2's manifest: " + error);
    // Field 21 (varint, wire type 0) = 0: the inline transaction is at offset 0 of the file.
    protobuf.insert(protobuf.end(), {0xA8, 0x01, 0x00});
    const auto put_u32 = [](std::vector<std::uint8_t>& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    // [u32 length][Transaction] [u32 length][Manifest] [trailer: u64 manifest position, u32, "LANC"]
    std::vector<std::uint8_t> file;
    put_u32(file, static_cast<std::uint32_t>(transaction.size()));
    file.insert(file.end(), transaction.begin(), transaction.end());
    const auto manifest_at = static_cast<std::uint64_t>(file.size());
    put_u32(file, static_cast<std::uint32_t>(protobuf.size()));
    file.insert(file.end(), protobuf.begin(), protobuf.end());
    for (int i = 0; i < 8; ++i) {
        file.push_back(static_cast<std::uint8_t>(manifest_at >> (8 * i)));
    }
    put_u32(file, 0);
    file.insert(file.end(), {'L', 'A', 'N', 'C'});
    {
        std::ofstream out(manifest_2, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    remove_transaction_files(path, 1);

    std::vector<std::uint8_t> read_back;
    bool found = false;
    const bool read_back_ok = nano_lance::dataset_transaction(path, 2, read_back, found, error);
    check(read_back_ok && found && read_back == transaction,
          "version 2's transaction is read from its manifest: " + error);
    nano_lance::reset_commit_conflict_stats();
    check(!nano_lance::dataset_update_config(path, {{"k", "2"}}, {}, version, error, 1U),
          "ours from version 1: refused by the inline transaction");
    check(error == "Incompatible transaction: This UpdateConfig transaction is incompatible with concurrent "
                   "transaction UpdateConfig at version 2.",
          "decided by the inline transaction, got: " + error);
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.retried == 0 && stats.refused == 1, "an inline transaction is not treated as missing");
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

/// The offsets the latest version deletes from fragment `id` (its deletion file's rows, sorted).
std::vector<std::uint32_t> latest_deleted_offsets(const fs::path& path, std::uint64_t id) {
    pb::Manifest m;
    std::uint64_t v = 0;
    std::string error;
    check(nano_lance::load_latest_manifest(path, m, v, error), "load the latest manifest: " + error);
    for (const auto& f : m.fragments) {
        if (f.id != id) {
            continue;
        }
        std::vector<std::uint32_t> rows;
        if (f.deletion_file.present) {
            check(nano_lance::read_manifest_deletion_vector(path, m, id, f.deletion_file, rows, error),
                  "read the deletion file: " + error);
        }
        return rows;
    }
    check(false, "fragment " + std::to_string(id) + " is in the latest version");
    return {};
}

/// Delete raced by a Delete on the same fragment: the rows merged (rebased, Lance's finish_delete_update).
void delete_rebased_over_delete_same_fragment() {
    const fs::path path = "commit_conflicts_delete_same_fragment";
    check(create_dataset(path), "create the dataset");
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 1", deleted, version, error) && version == 2,
          "theirs: delete commits version 2");
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_delete(path, "id = 2", deleted, version, error, 1U) && version == 3 && deleted == 1,
          "ours from version 1 is committed as version 3: " + error);
    check_rebased("delete over delete of the same fragment");
    check(latest_deleted_offsets(path, 0) == std::vector<std::uint32_t>({1, 2}),
          "the merged deletion file holds both deletes");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Delete raced by a Delete of the same row: an overlap of rows is retryable, so the delete runs again.
void delete_same_row_is_run_again() {
    const fs::path path = "commit_conflicts_delete_same_row";
    check(create_dataset(path), "create the dataset");
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 1", deleted, version, error) && version == 2,
          "theirs: delete commits version 2");
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_delete(path, "id = 1", deleted, version, error, 1U) && version == 3 && deleted == 0,
          "ours from version 1 runs again on version 2: " + error);
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.rebased == 0 && stats.retried == 1 && stats.refused == 0,
          "delete of the same row: run again (rebased 0, retried 1, refused 0)");
    check(latest_deleted_offsets(path, 0) == std::vector<std::uint32_t>({1}), "the row stays deleted once");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Delete raced by a Delete on another fragment: compatible, rebased over it.
void delete_rebased_over_delete_other_fragment() {
    const fs::path path = "commit_conflicts_delete_other_fragment";
    check(create_dataset(path), "create the dataset");
    check(append_ids(path, 100, 5), "two fragments: append commits version 2");
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 1", deleted, version, error) && version == 3,
          "theirs: delete on fragment 0 commits version 3");
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_delete(path, "id = 101", deleted, version, error, 2U) && version == 4 && deleted == 1,
          "ours from version 2 (fragment 1) is committed as version 4: " + error);
    check_rebased("delete over delete of another fragment");
    check(latest_deleted_offsets(path, 0) == std::vector<std::uint32_t>({1}) &&
              latest_deleted_offsets(path, 1) == std::vector<std::uint32_t>({1}),
          "each fragment keeps its own deletes");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// The merged deletes cover every row of a fragment: the fragment goes, as Lance removes it.
void delete_merge_removes_fragment_when_all_rows_go() {
    const fs::path path = "commit_conflicts_delete_whole_fragment";
    check(create_dataset(path), "create the dataset");
    check(append_ids(path, 100, 5), "two fragments: append commits version 2");
    std::uint64_t deleted = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id >= 5 AND id < 10", deleted, version, error) && version == 3 &&
              deleted == 5,
          "theirs: deletes ids 5..9 of fragment 0, version 3: " + error);
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_delete(path, "id < 5", deleted, version, error, 2U) && version == 4 && deleted == 5,
          "ours from version 2 deletes ids 0..4, version 4: " + error);
    check_rebased("delete of the other half of a fragment");
    check(latest_fragment_ids(path) == std::vector<std::uint64_t>({1}),
          "fragment 0 has no live rows left, so it is removed");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Delete raced by an Update that only changed the deletion file of the same fragment: rebased.
void delete_rebased_over_update_same_fragment() {
    const fs::path path = "commit_conflicts_delete_over_update";
    check(create_dataset(path), "create the dataset");
    std::uint64_t updated = 0;
    std::uint64_t version = 0;
    std::uint64_t deleted = 0;
    std::string error;
    const std::string theirs_predicate = "id = 1";
    check(nano_lance::dataset_update(path, &theirs_predicate, {{"id", "101"}}, updated, version, error) &&
              version == 2 && updated == 1,
          "theirs: update commits version 2: " + error);
    nano_lance::reset_commit_conflict_stats();
    check(nano_lance::dataset_delete(path, "id = 2", deleted, version, error, 1U) && version == 3 && deleted == 1,
          "ours from version 1 is committed as version 3: " + error);
    check_rebased("delete over update of the same fragment");
    check(latest_deleted_offsets(path, 0) == std::vector<std::uint32_t>({1, 2}),
          "the merged deletion file holds the update's row and the delete's");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Update raced by a Delete on the same fragment (other rows): rebased, its own rows kept.
void update_rebased_over_delete_same_fragment() {
    const fs::path path = "commit_conflicts_update_over_delete";
    check(create_dataset(path), "create the dataset");
    std::uint64_t deleted = 0;
    std::uint64_t updated = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 1", deleted, version, error) && version == 2,
          "theirs: delete commits version 2");
    nano_lance::reset_commit_conflict_stats();
    const std::string predicate = "id = 2";
    check(nano_lance::dataset_update(path, &predicate, {{"id", "102"}}, updated, version, error, nullptr, 1U) &&
              version == 3 && updated == 1,
          "ours from version 1 is committed as version 3: " + error);
    check_rebased("update over delete of the same fragment");
    check(latest_deleted_offsets(path, 0) == std::vector<std::uint32_t>({1, 2}),
          "the merged deletion file holds both deletes");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Update raced by a Delete of the row it updates: an overlap, so the update runs again.
void update_same_row_is_run_again() {
    const fs::path path = "commit_conflicts_update_same_row";
    check(create_dataset(path), "create the dataset");
    std::uint64_t deleted = 0;
    std::uint64_t updated = 0;
    std::uint64_t version = 0;
    std::string error;
    check(nano_lance::dataset_delete(path, "id = 2", deleted, version, error) && version == 2,
          "theirs: delete commits version 2");
    nano_lance::reset_commit_conflict_stats();
    const std::string predicate = "id = 2";
    check(nano_lance::dataset_update(path, &predicate, {{"id", "102"}}, updated, version, error, nullptr, 1U) &&
              updated == 0,
          "ours from version 1 runs again on version 2, where the row is gone: " + error);
    const auto stats = nano_lance::commit_conflict_stats();
    check(stats.rebased == 0 && stats.retried == 1 && stats.refused == 0,
          "update of the same row: run again (rebased 0, retried 1, refused 0)");
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Update rebased over an Append: the fragment the update adds takes an id after the latest version's, not the
/// id it chose on the version it was made from (the append took that one).
void update_rebased_over_append_takes_a_fresh_fragment_id() {
    const fs::path path = "commit_conflicts_update_over_append";
    check(create_dataset(path), "create the dataset");
    check(append_ids(path, 100, 5), "theirs: append commits version 2 (fragment 1)");
    nano_lance::reset_commit_conflict_stats();
    std::uint64_t updated = 0;
    std::uint64_t version = 0;
    std::string error;
    const std::string predicate = "id = 2";
    check(nano_lance::dataset_update(path, &predicate, {{"id", "102"}}, updated, version, error, nullptr, 1U) &&
              version == 3 && updated == 1,
          "ours from version 1 is committed as version 3: " + error);
    check_rebased("update over append");
    check(latest_fragment_ids(path) == std::vector<std::uint64_t>({0, 1, 2}),
          "the update's fragment is numbered after the append's");
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
    inline_transaction_decides_the_verdict();
    delete_rebased_over_append();
    update_config_rebased_over_delete();
    create_index_rebased_over_append();
    create_index_rebased_over_update_of_its_field();
    restore_rebased_over_append();
    delete_rebased_over_delete_same_fragment();
    delete_same_row_is_run_again();
    delete_rebased_over_delete_other_fragment();
    delete_merge_removes_fragment_when_all_rows_go();
    delete_rebased_over_update_same_fragment();
    update_rebased_over_delete_same_fragment();
    update_same_row_is_run_again();
    update_rebased_over_append_takes_a_fresh_fragment_id();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("commit conflict verdicts: ok\n");
    return 0;
}
