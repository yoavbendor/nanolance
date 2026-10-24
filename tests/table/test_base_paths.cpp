// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Base paths: the directories outside a dataset where its files may live (manifest field 18), and the
// base_id that a data file, deletion file or index names to pick one. Where a file lives decides where
// a reader looks for it, so the directory each base_id resolves to is pinned here, as are the rules
// that only a dataset root can hold deletions and that a base_id of 0 is a real base, not "absent".

#include "lance_minimal.pb.hpp"
#include "nanolance/base_paths.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
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

// A protobuf length-delimited field, hand-encoded (the bytes a Lance writer puts in a BasePath message).
void bytes_field(std::vector<std::uint8_t>& out, std::uint8_t key, const std::string& value) {
    out.push_back(key);
    out.push_back(static_cast<std::uint8_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    const std::filesystem::path dataset = "/data/main.lance";

    // The manifest's base paths: written, then read back as they were.
    {
        pb::Manifest manifest;
        std::vector<nano_lance::BasePath> bases(2);
        bases[0].id = 1;
        bases[0].name = "archive";
        bases[0].has_name = true;
        bases[0].is_dataset_root = true;
        bases[0].path = "/mnt/archive";
        bases[1].id = 2;
        bases[1].path = "/mnt/files";  // no name, not a dataset root
        nano_lance::write_base_paths(manifest, bases);

        std::vector<nano_lance::BasePath> back;
        std::string error;
        require(nano_lance::read_base_paths(manifest, back, error), "read: ", error);
        require(back.size() == 2, "two base paths read back");
        require(back[0].id == 1 && back[0].has_name && back[0].name == "archive" && back[0].is_dataset_root &&
                    back[0].path == "/mnt/archive",
                "the first base path reads back field for field");
        require(back[1].id == 2 && !back[1].has_name && !back[1].is_dataset_root && back[1].path == "/mnt/files",
                "the second base path reads back field for field");

        nano_lance::write_base_paths(manifest, {});
        require(nano_lance::read_base_paths(manifest, back, error) && back.empty(),
                "writing no base paths leaves none");
    }

    // A manifest with no base paths reads as none.
    {
        pb::Manifest manifest;
        std::vector<nano_lance::BasePath> bases;
        std::string error;
        require(nano_lance::read_base_paths(manifest, bases, error) && bases.empty(), "no base paths is empty");
    }

    // A BasePath message as a Transaction's UpdateBases carries it: id 3, name "x", a dataset root, "/p".
    {
        std::vector<std::uint8_t> msg = {0x08, 0x03};  // id = 3
        bytes_field(msg, 0x12, "x");                    // name
        msg.push_back(0x18);
        msg.push_back(0x01);                            // is_dataset_root = true
        bytes_field(msg, 0x22, "/p");                   // path

        nano_lance::BasePath base;
        std::string error;
        require(nano_lance::decode_base_path_message(msg.data(), msg.size(), base, error), "decode: ", error);
        require(base.id == 3 && base.has_name && base.name == "x" && base.is_dataset_root && base.path == "/p",
                "a BasePath message decodes field for field");

        const std::vector<std::uint8_t> truncated = {0x12, 0x05, 'x'};  // claims 5 bytes, has 1
        require(!nano_lance::decode_base_path_message(truncated.data(), truncated.size(), base, error),
                "a truncated BasePath message is refused");
        require(error == "malformed base path", "the refusal says so: ", error);
    }

    // The branch of a manifest: none for the main branch, else its name.
    {
        pb::Manifest manifest;
        std::string branch;
        std::string error;
        require(nano_lance::read_manifest_branch(manifest, branch, error) && branch.empty(),
                "a manifest with no branch is the main branch");
        nano_lance::write_manifest_branch(manifest, "feature-x");
        require(nano_lance::read_manifest_branch(manifest, branch, error) && branch == "feature-x",
                "the branch round-trips");
        nano_lance::write_manifest_branch(manifest, "");
        require(nano_lance::read_manifest_branch(manifest, branch, error) && branch.empty(),
                "an empty branch is the main branch again");
        require(manifest.unknown.empty(), "the main branch writes no field at all");
    }

    // base_id on a data file and a deletion file: absent means the dataset's own; 0 is base path 0.
    {
        pb::DataFile file;
        std::optional<std::uint32_t> id;
        std::string error;
        require(nano_lance::data_file_base_id(file, id, error) && !id.has_value(), "absent base_id");
        nano_lance::set_data_file_base_id(file, 0U);
        require(nano_lance::data_file_base_id(file, id, error) && id == 0U, "base_id 0 is present, not absent");
        nano_lance::set_data_file_base_id(file, 7U);
        require(nano_lance::data_file_base_id(file, id, error) && id == 7U, "base_id is set");
        nano_lance::set_data_file_base_id(file, std::nullopt);
        require(nano_lance::data_file_base_id(file, id, error) && !id.has_value(), "setting none removes it");

        pb::DeletionFile deletion;
        nano_lance::set_deletion_file_base_id(deletion, 4U);
        require(nano_lance::deletion_file_base_id(deletion, id, error) && id == 4U,
                "a deletion file's base_id is set");
        nano_lance::set_deletion_file_base_id(deletion, std::nullopt);
        require(nano_lance::deletion_file_base_id(deletion, id, error) && !id.has_value(),
                "a deletion file's base_id can be removed");
    }

    // base_id on an index (its own field, 9): set, read, removed.
    {
        pb::IndexMetadata index;
        std::optional<std::uint32_t> id;
        std::string error;
        require(nano_lance::index_base_id(index, id, error) && !id.has_value(), "an index with no base_id");
        nano_lance::set_index_base_id(index, 5U);
        require(nano_lance::index_base_id(index, id, error) && id == 5U, "an index's base_id is set");
        nano_lance::set_index_base_id(index, std::nullopt);
        require(nano_lance::index_base_id(index, id, error) && !id.has_value(), "an index's base_id is removed");
    }

    // Where a data file lives: the dataset's own data directory, a base that is a dataset root (its
    // data directory), or a plain base (the directory itself). An unknown base_id is refused.
    {
        pb::Manifest manifest;
        std::vector<nano_lance::BasePath> bases(2);
        bases[0].id = 1;
        bases[0].is_dataset_root = true;
        bases[0].path = "/elsewhere/root.lance";
        bases[1].id = 2;
        bases[1].is_dataset_root = false;
        bases[1].path = "/mnt/files";

        std::filesystem::path dir;
        std::string error;
        pb::DataFile own;
        require(nano_lance::data_file_dir(dataset, bases, own, dir, error), "own data dir: ", error);
        require(dir == dataset / "data", "a file with no base is in the dataset's data directory");

        pb::DataFile in_root;
        nano_lance::set_data_file_base_id(in_root, 1U);
        require(nano_lance::data_file_dir(dataset, bases, in_root, dir, error), "root base: ", error);
        require(dir == std::filesystem::path("/elsewhere/root.lance") / "data",
                "a dataset-root base keeps its files under data/");

        pb::DataFile plain;
        nano_lance::set_data_file_base_id(plain, 2U);
        require(nano_lance::data_file_dir(dataset, bases, plain, dir, error), "plain base: ", error);
        require(dir == std::filesystem::path("/mnt/files"), "a plain base is the directory itself");

        pb::DataFile unknown;
        nano_lance::set_data_file_base_id(unknown, 9U);
        require(!nano_lance::data_file_dir(dataset, bases, unknown, dir, error), "an unknown base is refused");
        require(error.find("refers to unknown base path 9") != std::string::npos, "the error names it: ", error);
    }

    // Where deletions live: the dataset's _deletions, or a base's _deletions when that base is a
    // dataset root. A plain base cannot hold deletions.
    {
        std::vector<nano_lance::BasePath> bases(2);
        bases[0].id = 1;
        bases[0].is_dataset_root = true;
        bases[0].path = "/elsewhere/root.lance";
        bases[1].id = 2;
        bases[1].path = "/mnt/files";

        std::filesystem::path dir;
        std::string error;
        require(nano_lance::deletion_dir(dataset, bases, std::nullopt, dir, error), "own deletions: ", error);
        require(dir == dataset / "_deletions", "deletions with no base are in _deletions");

        require(nano_lance::deletion_dir(dataset, bases, 1U, dir, error), "root deletions: ", error);
        require(dir == std::filesystem::path("/elsewhere/root.lance") / "_deletions",
                "a dataset-root base keeps deletions in its _deletions");

        require(!nano_lance::deletion_dir(dataset, bases, 2U, dir, error), "a plain base cannot hold deletions");
        require(error.find("not a dataset root") != std::string::npos, "the error says why: ", error);
    }

    // Where indices live: _indices for the dataset, the base's _indices for a dataset root, and the
    // base's directory itself for a plain base.
    {
        std::vector<nano_lance::BasePath> bases(2);
        bases[0].id = 1;
        bases[0].is_dataset_root = true;
        bases[0].path = "/elsewhere/root.lance";
        bases[1].id = 2;
        bases[1].path = "/mnt/files";

        std::filesystem::path dir;
        std::string error;
        require(nano_lance::indices_dir(dataset, bases, std::nullopt, dir, error) && dir == dataset / "_indices",
                "indices with no base are in _indices");
        require(nano_lance::indices_dir(dataset, bases, 1U, dir, error) &&
                    dir == std::filesystem::path("/elsewhere/root.lance") / "_indices",
                "a dataset-root base keeps indices in its _indices");
        require(nano_lance::indices_dir(dataset, bases, 2U, dir, error) && dir == std::filesystem::path("/mnt/files"),
                "a plain base is the indices directory itself");
    }

    // An index's directory is <indices>/<uuid>, in the manifest's base or the dataset's own.
    {
        pb::Manifest manifest;
        std::vector<nano_lance::BasePath> bases(1);
        bases[0].id = 1;
        bases[0].is_dataset_root = true;
        bases[0].path = "/elsewhere/root.lance";
        nano_lance::write_base_paths(manifest, bases);

        std::array<std::uint8_t, 16> uuid{};
        for (std::size_t i = 0; i < uuid.size(); ++i) {
            uuid[i] = static_cast<std::uint8_t>(i + 1);
        }
        const auto index = pb::make_index_metadata(uuid, {1}, "idx", 2, {0}, "/lance.table.BTreeIndexDetails", 1,
                                                   0, {});
        require(pb::uuid_string(uuid) == "01020304-0506-0708-090a-0b0c0d0e0f10", "the uuid's directory name");

        std::filesystem::path dir;
        std::string error;
        require(nano_lance::index_dir_of(dataset, manifest, index, dir, error), "own index dir: ", error);
        require(dir == dataset / "_indices" / "01020304-0506-0708-090a-0b0c0d0e0f10", "an index with no base");

        pb::IndexMetadata in_root = index;
        nano_lance::set_index_base_id(in_root, 1U);
        require(nano_lance::index_dir_of(dataset, manifest, in_root, dir, error), "root index dir: ", error);
        require(dir == std::filesystem::path("/elsewhere/root.lance") / "_indices" /
                           "01020304-0506-0708-090a-0b0c0d0e0f10",
                "an index in a dataset-root base");

        // The manifest-level deletion dir follows the same base table.
        pb::DeletionFile deletion;
        nano_lance::set_deletion_file_base_id(deletion, 1U);
        require(nano_lance::deletion_file_dir(dataset, manifest, deletion, dir, error), "manifest deletion dir");
        require(dir == std::filesystem::path("/elsewhere/root.lance") / "_deletions",
                "a deletion file's dir comes from the manifest's base paths");
    }

    std::cout << "base paths: base_id resolution, dataset-root rules, round trips OK\n";
    return 0;
}
