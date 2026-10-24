// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
// dataset_transaction: commits built by hand, as pylance's LanceDataset.commit takes them -- one
// lance.table.Transaction operation message applied to a version's manifest. Covers Overwrite creating a
// dataset (fragment ids assigned from 0, the schema, the config upserts), Append (a new fragment numbered
// after the latest), Delete (a fragment removed, another replaced), ReserveFragments (max_fragment_id
// raised), UpdateConfig (an upsert and a removal), Project (a field and the data files left with no live
// field dropped), Restore (an older version's fragments, the high-water mark kept), a detached commit
// (numbered with the high bit, never the latest), the transaction file recording the operation exactly as
// given, and the refusals: a commit on a version another writer took, an operation on no dataset, an
// Overwrite carrying a deletion file, a Merge that drops a fragment, and an operation nanolance does not
// read. Also dataset_fragment_messages / dataset_field_messages, which hand the version's messages back.

#include "nanolance/dataset_transaction.hpp"
#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"
#include "table/dataset_operation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
namespace nl = nano_lance;
namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

void varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void key(Bytes& out, std::uint32_t field, std::uint32_t wire) { varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire); }

void uint_field(Bytes& out, std::uint32_t field, std::uint64_t value) {
    key(out, field, 0);
    varint(out, value);
}

void bytes_field(Bytes& out, std::uint32_t field, const Bytes& value) {
    key(out, field, 2);
    varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void string_field(Bytes& out, std::uint32_t field, const std::string& s) { bytes_field(out, field, Bytes(s.begin(), s.end())); }

/// A map<string, string> entry (key 1, value 2).
Bytes entry(const std::string& k, const std::string& v) {
    Bytes e;
    string_field(e, 1, k);
    string_field(e, 2, v);
    return e;
}

pb::DataFragment fragment(std::uint64_t id, std::uint64_t rows, const std::string& path, std::vector<std::int32_t> fields) {
    pb::DataFragment f;
    f.id = id;
    f.physical_rows = rows;
    pb::DataFile file;
    file.path = path;
    file.fields = fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        file.column_indices.push_back(static_cast<std::int32_t>(i));
    }
    file.file_major_version = 2;
    file.file_minor_version = 2;
    f.files.push_back(file);
    return f;
}

pb::Field field(std::int32_t id, const std::string& name) {
    pb::Field f;
    f.id = id;
    f.name = name;
    f.logical_type = "int64";
    return f;
}

nl::HandBuiltCommit commit_of(std::uint32_t field_number, Bytes message, std::uint64_t base) {
    nl::HandBuiltCommit c;
    c.operation_field = field_number;
    c.operation = std::move(message);
    c.base_version = base;
    c.read_version = base;
    return c;
}

pb::Manifest load(const fs::path& path, std::uint64_t version) {
    pb::Manifest m;
    std::string error;
    require(nl::load_manifest_version(path, version, m, error), "load version " + std::to_string(version) + ": " + error);
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
    const fs::path dir = fs::temp_directory_path() / ("nanolance_dataset_transaction_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    std::string error;
    std::uint64_t version = 0;

    // Overwrite with no dataset (base 0): the dataset is created. Every fragment of an overwrite is new, so
    // ids are assigned from 0 whatever the message says; the schema and the config upserts are taken.
    Bytes overwrite;
    bytes_field(overwrite, 1, pb::encode_data_fragment(fragment(7, 10, "a.lance", {0, 1})));
    bytes_field(overwrite, 1, pb::encode_data_fragment(fragment(9, 20, "b.lance", {0, 1})));
    bytes_field(overwrite, 2, pb::encode_field(field(0, "id")));
    bytes_field(overwrite, 2, pb::encode_field(field(1, "x")));
    bytes_field(overwrite, 4, entry("user.key", "v1"));
    {
        auto c = commit_of(nl::kOverwrite, overwrite, 0);
        c.transaction_properties["note"] = "created";
        require(nl::dataset_commit_hand_built(dir, c, version, error), "overwrite creates the dataset: " + error);
        require(version == 1, "the first version is 1");
        const auto m = load(dir, 1);
        require(ids_of(m) == std::vector<std::uint64_t>({0, 1}), "an overwrite numbers its fragments from 0");
        require(m.fragments[1].physical_rows == 20 && m.fragments[1].files[0].path == "b.lance", "the fragments as given");
        require(m.fields.size() == 2 && m.fields[1].name == "x", "the schema as given");
        require(m.config.count("user.key") == 1 && m.config.at("user.key") == "v1", "the config upsert");
        require(m.data_format.file_format == "lance" && m.data_format.version == "2.2",
                "a new dataset's storage version is its data files' (" + m.data_format.version + ")");
        require(m.has_max_fragment_id && m.max_fragment_id == 1, "max_fragment_id is the highest id");
        require(m.writer_library == "nanolance" && m.has_timestamp, "the version is stamped");

        // The transaction file records the operation as given, and the read version and properties.
        require(!m.transaction_file.empty(), "the manifest names its transaction file");
        require(m.transaction_file.rfind("1-", 0) != 0 && m.transaction_file.rfind("0-", 0) == 0,
                "the transaction file is named {read_version}-{uuid}.txn: " + m.transaction_file);
        std::ifstream in(dir / "_transactions" / m.transaction_file, std::ios::binary);
        const Bytes file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::uint32_t op_field = 0;
        Bytes op_message;
        require(nl::transaction_file_operation(file, op_field, op_message, error), "the transaction file reads: " + error);
        require(op_field == nl::kOverwrite && op_message == overwrite, "the operation is recorded byte for byte");
        const std::string note = "created";
        require(std::search(file.begin(), file.end(), note.begin(), note.end()) != file.end(),
                "the transaction properties are recorded");
    }

    // A commit on version 1 again, once another writer took version 2, changes nothing.
    {
        Bytes append;
        bytes_field(append, 1, pb::encode_data_fragment(fragment(0, 5, "c.lance", {0, 1})));
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kAppend, append, 1), version, error),
                "append: " + error);
        require(version == 2, "the append is version 2");
        const auto m = load(dir, 2);
        require(ids_of(m) == std::vector<std::uint64_t>({0, 1, 2}), "an appended fragment without id takes the next one");
        require(m.max_fragment_id == 2, "max_fragment_id follows");

        require(!nl::dataset_commit_hand_built(dir, commit_of(nl::kAppend, append, 1), version, error),
                "a second commit on version 1 is refused");
        require(error.rfind("commit conflict", 0) == 0, "with a commit conflict: " + error);
        std::uint64_t latest = 0;
        pb::Manifest m2;
        require(nl::load_latest_manifest(dir, m2, latest, error) && latest == 2, "the refused commit published nothing");
    }

    // Delete: fragment 1 goes, fragment 0 is replaced by the message's (here: a new row count stands for
    // a new deletion file); the predicate is only recorded.
    {
        Bytes del;
        auto updated = fragment(0, 10, "a.lance", {0, 1});
        bytes_field(del, 1, pb::encode_data_fragment(updated));
        uint_field(del, 2, 1);
        string_field(del, 3, "id > 3");
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kDelete, del, 2), version, error), "delete: " + error);
        const auto m = load(dir, 3);
        require(ids_of(m) == std::vector<std::uint64_t>({0, 2}), "the deleted fragment is gone");
        require(m.max_fragment_id == 2, "max_fragment_id never goes down");
    }

    // ReserveFragments raises max_fragment_id; an append afterwards numbers past the reservation.
    {
        Bytes reserve;
        uint_field(reserve, 1, 5);
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kReserveFragments, reserve, 3), version, error),
                "reserve: " + error);
        require(load(dir, 4).max_fragment_id == 7, "five ids reserved after 2");
        Bytes append;
        bytes_field(append, 1, pb::encode_data_fragment(fragment(0, 5, "d.lance", {0, 1})));
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kAppend, append, 4), version, error), "append: " + error);
        require(ids_of(load(dir, 5)) == std::vector<std::uint64_t>({0, 2, 8}), "the next fragment id is past the reservation");
    }

    // UpdateConfig: config_updates (field 6) upserts one key and removes another (an entry with no value).
    {
        Bytes map;
        bytes_field(map, 1, entry("other", "x"));
        Bytes removal;
        string_field(removal, 1, "user.key");
        bytes_field(map, 1, removal);
        Bytes update;
        bytes_field(update, 6, map);
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kUpdateConfig, update, 5), version, error),
                "update config: " + error);
        const auto m = load(dir, 6);
        require(m.config.count("user.key") == 0 && m.config.count("other") == 1 && m.config.at("other") == "x",
                "the upsert and the removal");
        require(ids_of(m) == std::vector<std::uint64_t>({0, 2, 8}), "the data is untouched");
    }

    // Project: the schema loses field 1; data files holding only dropped fields go, the others stay.
    {
        Bytes append;
        bytes_field(append, 1, pb::encode_data_fragment(fragment(0, 5, "x_only.lance", {1})));
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kAppend, append, 6), version, error), "append: " + error);
        Bytes project;
        bytes_field(project, 1, pb::encode_field(field(0, "id")));
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kProject, project, 7), version, error),
                "project: " + error);
        const auto m = load(dir, 8);
        require(m.fields.size() == 1 && m.fields[0].name == "id", "the projected schema");
        require(m.fragments.size() == 4 && m.fragments[3].files.empty(), "a file of dropped fields only is gone");
        require(m.fragments[0].files.size() == 1, "a file with a live field stays");
    }

    // Restore version 1: its fragments and schema come back; the high-water mark of the latest stays.
    {
        Bytes restore;
        uint_field(restore, 1, 1);
        require(nl::dataset_commit_hand_built(dir, commit_of(nl::kRestore, restore, 8), version, error),
                "restore: " + error);
        require(version == 9, "a restore is a new version");
        const auto m = load(dir, 9);
        require(ids_of(m) == std::vector<std::uint64_t>({0, 1}) && m.fields.size() == 2, "version 1's content");
        require(m.max_fragment_id == 9, "max_fragment_id is kept from the latest");
    }

    // A detached commit: numbered with the high bit set, outside the lineage, reachable by its number.
    {
        Bytes append;
        bytes_field(append, 1, pb::encode_data_fragment(fragment(0, 1, "e.lance", {0, 1})));
        auto c = commit_of(nl::kAppend, append, 9);
        c.detached = true;
        require(nl::dataset_commit_hand_built(dir, c, version, error), "detached: " + error);
        require((version >> 63U) == 1U, "a detached version has the high bit set");
        std::uint64_t latest = 0;
        pb::Manifest m;
        require(nl::load_latest_manifest(dir, m, latest, error) && latest == 9, "the latest version is unchanged");
        require(load(dir, version).fragments.size() == 3, "the detached version opens by its number");
    }

    // The version's messages, as a hand-built transaction carries them back.
    {
        std::vector<Bytes> fragments;
        require(nl::dataset_fragment_messages(dir, true, 2, fragments, error), "fragment messages: " + error);
        require(fragments.size() == 3, "three fragments at version 2");
        pb::DataFragment f;
        require(pb::decode_data_fragment(fragments[2], f) && f.id == 2 && f.files[0].path == "c.lance",
                "a fragment message decodes to the fragment");
        std::vector<Bytes> fields;
        std::map<std::string, Bytes> metadata;
        require(nl::dataset_field_messages(dir, false, 0, fields, metadata, error), "field messages: " + error);
        require(fields.size() == 2, "the latest version's two fields");
        require(!nl::dataset_fragment_messages(dir, true, 99, fragments, error), "a missing version is an error");
    }

    // Refusals, each leaving the dataset as it was.
    {
        Bytes append;
        bytes_field(append, 1, pb::encode_data_fragment(fragment(0, 1, "f.lance", {0})));
        const fs::path none = dir.string() + "_none";
        require(!nl::dataset_commit_hand_built(none, commit_of(nl::kAppend, append, 0), version, error),
                "an append to no dataset is refused");
        require(error.find("Cannot apply operation Append to non-existent dataset") != std::string::npos, error);

        auto with_deletions = fragment(0, 4, "g.lance", {0, 1});
        with_deletions.deletion_file.present = true;
        with_deletions.deletion_file.num_deleted_rows = 1;
        Bytes bad_overwrite;
        bytes_field(bad_overwrite, 1, pb::encode_data_fragment(with_deletions));
        bytes_field(bad_overwrite, 2, pb::encode_field(field(0, "id")));
        require(!nl::dataset_commit_hand_built(dir, commit_of(nl::kOverwrite, bad_overwrite, 9), version, error),
                "an overwrite carrying a deletion file is refused");
        require(error.find("Overwrite fragments must be newly written") != std::string::npos, error);

        Bytes no_fields;
        bytes_field(no_fields, 1, pb::encode_data_fragment(fragment(0, 1, "h.lance", {})));
        require(!nl::dataset_commit_hand_built(dir, commit_of(nl::kAppend, no_fields, 9), version, error),
                "a data file with no fields is refused");
        require(error.find("does not contain any fields") != std::string::npos, error);

        Bytes merge;
        bytes_field(merge, 1, pb::encode_data_fragment(fragment(0, 10, "a.lance", {0, 1})));
        bytes_field(merge, 2, pb::encode_field(field(0, "id")));
        require(!nl::dataset_commit_hand_built(dir, commit_of(nl::kMerge, merge, 9), version, error),
                "a merge that drops a fragment is refused");
        require(error.find("Merge operation reduced fragment count") != std::string::npos, error);

        require(!nl::dataset_commit_hand_built(dir, commit_of(112, {}, 9), version, error),
                "an operation nanolance does not read is refused");

        std::uint64_t latest = 0;
        pb::Manifest m;
        require(nl::load_latest_manifest(dir, m, latest, error) && latest == 9, "no refusal published a version");
    }

    fs::remove_all(dir);
    std::cout << "dataset_transaction: ok\n";
    return 0;
}
