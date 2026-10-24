// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
// rebase_operation: a change that lost its commit race, applied again on top of the version the other
// writers left (Lance's rebase), in memory. An Append's fragments are renumbered after the latest
// version's, so two appends never share an id; a Delete removes its fragment from the latest version and
// keeps the other writer's fragments; a Restore publishes its own manifest with the latest high-water
// marks (max_fragment_id, next_row_id); a CreateIndex drops the fragments another writer removed from its
// coverage and the fragments a concurrent Update rewrote in an indexed field (Lance's finish_create_index);
// and the cases that cannot be rebased fail with "commit conflict" so the caller runs the operation again:
// a Rewrite of a fragment that is gone, an Append on a dataset with stable row ids.

#include "table/dataset_transaction_ops.hpp"

#include "lance_minimal.pb.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace nl = nano_lance;
namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

pb::DataFragment fragment(std::uint64_t id, std::uint64_t rows) {
    pb::DataFragment f;
    f.id = id;
    f.physical_rows = rows;
    pb::DataFile file;
    file.path = "f" + std::to_string(id) + ".lance";
    file.fields = {0};
    file.column_indices = {0};
    file.file_major_version = 2;
    file.file_minor_version = 2;
    f.files.push_back(file);
    return f;
}

pb::Manifest manifest(std::uint64_t version, std::vector<std::uint64_t> ids) {
    pb::Manifest m;
    m.version = version;
    m.data_format.file_format = "lance";
    m.data_format.version = "2.2";
    pb::Field id;
    id.id = 0;
    id.name = "id";
    id.logical_type = "int64";
    m.fields.push_back(id);
    for (const auto i : ids) {
        m.fragments.push_back(fragment(i, 10));
    }
    if (!ids.empty()) {
        m.has_max_fragment_id = true;
        m.max_fragment_id = static_cast<std::uint32_t>(ids.back());
    }
    return m;
}

std::vector<std::uint64_t> ids_of(const pb::Manifest& m) {
    std::vector<std::uint64_t> out;
    for (const auto& f : m.fragments) {
        out.push_back(f.id);
    }
    return out;
}

}  // namespace

int main() {
    std::string error;

    // Append: ours numbered its new fragment 2 after base {0, 1}; another writer appended fragment 2 since.
    // Rebased, ours takes 3 and both survive.
    {
        const auto base = manifest(1, {0, 1});
        const auto latest = manifest(2, {0, 1, 2});
        auto ours = base;
        ours.fragments.push_back(fragment(2, 5));
        nl::Operation op;
        op.kind = nl::kAppend;
        op.fragments = {fragment(2, 5)};
        nl::Operation theirs;
        theirs.kind = nl::kAppend;
        theirs.fragments = {fragment(2, 10)};
        pb::Manifest next;
        require(nl::rebase_operation(latest, ours, op, {theirs}, 3, next, error), "append rebases: " + error);
        require(ids_of(next) == std::vector<std::uint64_t>({0, 1, 2, 3}), "the rebased fragment is renumbered");
        require(next.fragments[3].physical_rows == 5 && next.fragments[2].physical_rows == 10,
                "each writer's fragment keeps its rows");
        require(next.has_max_fragment_id && next.max_fragment_id == 3, "max_fragment_id follows");
    }

    // Delete of fragment 0, rebased over an Append: the appended fragment stays.
    {
        const auto latest = manifest(2, {0, 1, 2});
        nl::Operation op;
        op.kind = nl::kDelete;
        op.removed_ids = {0};
        nl::Operation theirs;
        theirs.kind = nl::kAppend;
        pb::Manifest next;
        require(nl::rebase_operation(latest, manifest(1, {0, 1}), op, {theirs}, 3, next, error),
                "delete rebases: " + error);
        require(ids_of(next) == std::vector<std::uint64_t>({1, 2}), "the deleted fragment goes, theirs stays");
    }

    // Restore: our manifest (the restored one) stands; the latest's high-water marks carry over.
    {
        auto latest = manifest(5, {0, 1, 2, 3, 4});
        latest.next_row_id = 50;
        auto ours = manifest(6, {0});
        ours.next_row_id = 10;
        nl::Operation op;
        op.kind = nl::kRestore;
        op.restore_version = 1;
        pb::Manifest next;
        require(nl::rebase_operation(latest, ours, op, {}, 6, next, error), "restore rebases: " + error);
        require(ids_of(next) == std::vector<std::uint64_t>({0}), "the restored fragments");
        require(next.max_fragment_id == 4, "max_fragment_id never goes down");
        require(next.next_row_id == 50, "next_row_id never goes down");
    }

    // CreateIndex over {0, 1, 2} on field 0, while another writer removed fragment 2 and an Update rewrote
    // field 0 in fragment 1: the index covers only fragment 0.
    {
        const auto latest = manifest(3, {0, 1});
        std::array<std::uint8_t, 16> uuid{};
        uuid[0] = 7;
        const auto index = pb::make_index_metadata(uuid, {0}, "id_idx", 1, {0, 1, 2}, "/lance.table.BTreeIndexDetails", 0,
                                                   0, {});
        nl::Operation op;
        op.kind = nl::kCreateIndex;
        op.new_indices = {index};
        nl::Operation update;
        update.kind = nl::kUpdate;
        update.updated_fragments = {fragment(1, 10)};
        update.fields_modified = {0};
        pb::Manifest next;
        require(nl::rebase_operation(latest, manifest(1, {0, 1, 2}), op, {update}, 4, next, error),
                "create index rebases: " + error);
        require(next.indices.size() == 1 && next.indices[0].name == "id_idx", "the index is added");
        require(next.indices[0].fragment_ids == std::vector<std::uint32_t>({0}),
                "removed and rewritten fragments leave the coverage");

        // An Update of another field leaves the coverage alone.
        update.fields_modified = {5};
        require(nl::rebase_operation(latest, manifest(1, {0, 1, 2}), op, {update}, 4, next, error),
                "create index rebases: " + error);
        require(next.indices[0].fragment_ids == std::vector<std::uint32_t>({0, 1}),
                "an update of an unindexed field keeps fragment 1 covered");
    }

    // Not rebased: a Rewrite of a fragment another writer removed.
    {
        const auto latest = manifest(3, {1});
        nl::Operation op;
        op.kind = nl::kRewrite;
        nl::RewriteGroup group;
        group.old_fragments = {fragment(0, 10)};
        group.new_fragments = {fragment(0, 10)};
        op.groups = {group};
        pb::Manifest next;
        require(!nl::rebase_operation(latest, manifest(1, {0, 1}), op, {}, 4, next, error),
                "a rewrite of a gone fragment does not rebase");
        require(error.rfind("commit conflict", 0) == 0, "it is a conflict to run again: " + error);
    }

    // Not rebased: an Append on a dataset with stable row ids (its row ids came from the base's counter).
    {
        auto latest = manifest(2, {0, 1});
        latest.reader_feature_flags |= pb::kFlagStableRowIds;
        latest.writer_feature_flags |= pb::kFlagStableRowIds;
        nl::Operation op;
        op.kind = nl::kAppend;
        op.fragments = {fragment(0, 5)};
        pb::Manifest next;
        require(!nl::rebase_operation(latest, manifest(1, {0}), op, {}, 3, next, error),
                "an append with stable row ids does not rebase");
        require(error.rfind("commit conflict: stable row ids", 0) == 0, "it is a conflict to run again: " + error);
    }

    std::cout << "dataset_transaction_ops: ok\n";
    return 0;
}
