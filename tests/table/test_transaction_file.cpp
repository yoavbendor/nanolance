// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The transaction of a commit: which operation a change from one version to the next is recorded as
// (derive_transaction_operation), and the transaction file that records it on disk
// (write_transaction_file, under _transactions/ with Lance's name). The derived operation must be one
// the operation decoder reads back to the same change, since that decoder is what conflict checks use.

#include "lance_minimal.pb.hpp"
#include "table/dataset_operation.hpp"
#include "table/transaction_file.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

pb::DataFragment fragment(std::uint64_t id, std::uint64_t rows) {
    pb::DataFragment f;
    f.id = id;
    f.physical_rows = rows;
    return f;
}

pb::Manifest manifest(std::uint64_t version, std::vector<pb::DataFragment> fragments) {
    pb::Manifest m;
    m.version = version;
    m.fragments = std::move(fragments);
    return m;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    // An append records the new fragments, as a lance.table.Append.
    {
        auto parent = manifest(1, {fragment(0, 5)});
        auto next = manifest(2, {fragment(0, 5), fragment(1, 3)});
        next.operation = pb::Manifest::Operation::Append;
        next.first_new_fragment = 1;

        const auto [field, message] = nano_lance::derive_transaction_operation(parent, next);
        require(field == nano_lance::kAppend, "an append is recorded as Append");

        nano_lance::Operation op;
        std::string error;
        require(nano_lance::decode_operation(field, message, op, error), "the derived append decodes: ", error);
        require(op.fragments.size() == 1 && op.fragments[0].id == 1 && op.fragments[0].physical_rows == 3,
                "the derived append carries exactly the new fragment");
    }

    // A restore names the version it restores; a reserve names how many ids it takes.
    {
        auto parent = manifest(3, {});
        auto next = manifest(4, {});
        next.operation = pb::Manifest::Operation::Restore;
        next.restored_version = 2;
        auto [field, message] = nano_lance::derive_transaction_operation(parent, next);
        require(field == nano_lance::kRestore, "a restore is recorded as Restore");
        nano_lance::Operation op;
        std::string error;
        require(nano_lance::decode_operation(field, message, op, error) && op.restore_version == 2,
                "the restored version survives the round trip");

        next.operation = pb::Manifest::Operation::Reserve;
        next.reserved_fragments = 6;
        const auto reserve = nano_lance::derive_transaction_operation(parent, next);
        field = reserve.first;
        message = reserve.second;
        require(field == nano_lance::kReserveFragments, "a reserve is recorded as ReserveFragments");
        require(nano_lance::decode_operation(field, message, op, error) && op.reserve == 6,
                "the reserved count survives the round trip");
    }

    // A change that removes a fragment and adds nothing is a delete of that fragment.
    {
        auto parent = manifest(1, {fragment(0, 4), fragment(1, 4), fragment(2, 4)});
        auto next = manifest(2, {fragment(0, 4), fragment(2, 4)});
        const auto [field, message] = nano_lance::derive_transaction_operation(parent, next);
        require(field == nano_lance::kDelete, "a removed fragment is recorded as Delete");
        nano_lance::Operation op;
        std::string error;
        require(nano_lance::decode_operation(field, message, op, error), "the derived delete decodes: ", error);
        require(op.removed_ids == std::vector<std::uint64_t>({1}), "the delete names the removed fragment");
    }

    // An overwrite keeps its schema in the operation.
    {
        auto parent = manifest(1, {fragment(0, 4)});
        pb::Field column;
        column.name = "a";
        column.id = 0;
        auto next = manifest(2, {fragment(1, 2)});
        next.fields = {column};
        next.operation = pb::Manifest::Operation::Overwrite;
        const auto [field, message] = nano_lance::derive_transaction_operation(parent, next);
        require(field == nano_lance::kOverwrite, "an overwrite is recorded as Overwrite");
        nano_lance::Operation op;
        std::string error;
        require(nano_lance::decode_operation(field, message, op, error), "the derived overwrite decodes: ", error);
        require(op.schema.size() == 1 && op.schema[0].name == "a", "the overwrite keeps the schema");
    }

    // write_transaction_file: the file is named <read version>-<uuid>.txn under _transactions/, and holds
    // the operation. remove_transaction_file takes it away again.
    {
        const auto root = std::filesystem::temp_directory_path() / "nano_lance_transaction_file_test";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "_transactions");

        auto next = manifest(2, {fragment(0, 5), fragment(1, 3)});
        next.operation = pb::Manifest::Operation::Append;
        next.first_new_fragment = 1;

        std::string name;
        std::string error;
        require(nano_lance::write_transaction_file(root, next, name, error), "write: ", error);
        require(name.rfind("1-", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".txn",
                "the name is <read version>-<uuid>.txn: " + name);
        const auto path = root / "_transactions" / name;
        require(std::filesystem::exists(path), "the transaction file is written");

        std::ifstream in(path, std::ios::binary);
        const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::uint32_t field = 0;
        Bytes message;
        require(nano_lance::transaction_file_operation(bytes, field, message, error), "the file holds an operation");
        require(field == nano_lance::kAppend, "the file records an Append");

        nano_lance::remove_transaction_file(root, name);
        require(!std::filesystem::exists(path), "remove_transaction_file removes it");
        nano_lance::remove_transaction_file(root, "");  // no name: nothing to remove, no failure

        std::filesystem::remove_all(root, ec);
    }

    std::cout << "transaction file: derived append, restore, reserve, delete, overwrite; file name and removal OK\n";
    return 0;
}
