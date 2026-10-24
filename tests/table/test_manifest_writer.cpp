// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// manifest_writer -> manifest_reader: a dataset's versions are committed with commit_dataset_version (create,
// append, overwrite, stable row ids, base paths) and read back with the reader, and every field the writer sets
// is compared: schema fields (names, on-disk logical types, ids, parents, encodings, nullability, metadata with
// the per-file encoding hints dropped), fragments (ids, rows, data file path, versions, size, field and column
// lists, base id), deletion files (carried through an append, which raises the deletion feature flag), the
// version, timestamp, writer, table config and metadata, schema metadata, transaction file and the feature
// flags. Also the refusals: creating over a dataset, appending to nothing, stable row ids on a dataset without
// them, an unsupported URI, a version published twice.

#include "nanolance/base_paths.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/row_ids.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace nano_lance;

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

fs::path fresh_dir(const std::string& name) {
    auto p = fs::temp_directory_path() / ("nano_lance_manifest_writer_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

LanceField make_field(const std::string& name, const std::string& type, std::int32_t id, std::int32_t parent,
                      std::int32_t column, bool nullable = true) {
    LanceField f;
    f.name = name;
    f.logical_type = type;
    f.id = id;
    f.parent_id = parent;
    f.column_index = column;
    f.nullable = nullable;
    return f;
}

// id (int64), name (utf8, with metadata), s (struct, no column) holding s.x (float, not nullable), tag (dictionary).
LanceSchemaMapping make_mapping() {
    LanceSchemaMapping m;
    m.fields.push_back(make_field("id", "int64", 0, -1, 0, false));
    auto name = make_field("name", "utf8", 1, -1, 1);
    name.metadata["owner"] = "alice";
    name.metadata["nanolance:packing"] = "bitpacked";
    name.metadata["nanolance:const-value"] = "abc";
    m.fields.push_back(name);
    m.fields.push_back(make_field("s", "struct", 2, -1, -1));
    m.fields.push_back(make_field("x", "float", 3, 2, 2, false));
    auto tag = make_field("tag", "utf8", 4, -1, 3);
    tag.dictionary_index_format = "I";
    tag.dictionary_ordered = true;
    m.fields.push_back(tag);
    return m;
}

NewFragment make_fragment(const std::string& file, std::uint64_t rows, std::uint64_t size) {
    NewFragment f;
    f.data_file.relative_path = fs::path("data") / file;
    f.data_file.file_size_bytes = size;
    f.rows = rows;
    return f;
}

void check_field(const pb::Field& f, const std::string& name, const std::string& type, std::int32_t id,
                 std::int32_t parent, std::int32_t encoding, bool nullable) {
    require(f.name == name, "field name " + name);
    require(f.logical_type == type, "field " + name + " logical type " + f.logical_type);
    require(f.id == id && f.parent_id == parent, "field " + name + " id and parent");
    require(f.encoding == encoding, "field " + name + " encoding");
    require(f.nullable == nullable, "field " + name + " nullability");
    require(f.type == 2, "field " + name + " type");
}

void test_fields() {
    const auto m = make_mapping();
    const auto id = make_manifest_field(m.fields[0]);
    check_field(id, "id", "int64", 0, -1, 1, false);
    const auto name = make_manifest_field(m.fields[1]);
    check_field(name, "name", "string", 1, -1, 2, true);
    require(name.metadata.size() == 1U && name.metadata.at("owner") == std::vector<std::uint8_t>{'a', 'l', 'i', 'c', 'e'},
            "metadata kept, per-file encoding hints dropped");
    check_field(make_manifest_field(m.fields[2]), "s", "struct", 2, -1, 0, true);
    check_field(make_manifest_field(m.fields[3]), "x", "float", 3, 2, 1, false);
    check_field(make_manifest_field(m.fields[4]), "tag", "dict:string:uint32:true", 4, -1, 3, true);

    LanceField large = make_field("l", "large_utf8", 5, -1, 4);
    check_field(make_manifest_field(large), "l", "large_string", 5, -1, 2, true);
}

void test_data_fragment() {
    const auto m = make_mapping();
    auto frag = make_fragment("abc.lance", 10, 1234);
    frag.base_id = 7U;
    const auto out = make_data_fragment(m, frag, 42);
    require(out.id == 42U && out.physical_rows == 10U && out.files.size() == 1U, "fragment id, rows, one file");
    require(!out.deletion_file.present, "no deletion file");
    const auto& file = out.files[0];
    require(file.path == "abc.lance", "path is the file name only");
    require(file.file_major_version == 2U && file.file_minor_version == 2U, "files are 2.2");
    require(file.file_size_bytes == 1234U, "file size");
    // Physical fields only: the struct has no column.
    require(file.fields == std::vector<std::int32_t>({0, 1, 3, 4}), "field ids of physical fields");
    require(file.column_indices == std::vector<std::int32_t>({0, 1, 2, 3}), "column indices");
    std::optional<std::uint32_t> base;
    std::string err;
    require(data_file_base_id(file, base, err) && base && *base == 7U, "base id");
    const auto plain = make_data_fragment(m, make_fragment("p.lance", 0, 0), 0);
    require(data_file_base_id(plain.files[0], base, err) && !base, "no base id");
}

void test_commit_cycle() {
    const auto ds = fresh_dir("cycle");
    const auto mapping = make_mapping();
    std::map<std::string, std::vector<std::uint8_t>> schema_metadata{{"pandas", {1, 2, 3}}};

    CommitExtras create;
    create.schema_metadata = &schema_metadata;
    create.table_metadata = {{"owner", "team-a"}};
    create.initial_config = {{"lance.auto_cleanup.interval", "1000"}, {"k", "v"}};
    create.transaction_properties = {{"__lance_commit_message", "first"}};
    const std::vector<BasePath> bases{{1U, "other", true, false, "/tmp/other_root"}};
    create.initial_bases = &bases;

    auto f0 = make_fragment("a.lance", 5, 100);
    auto f1 = make_fragment("b.lance", 7, 200);
    f1.base_id = 1U;
    std::uint64_t version = 0;
    std::string err;
    require(commit_dataset_version(ds, mapping, {f0, f1}, CommitMode::Create, version, err, create), "create: " + err);
    require(version == 1U, "first version is 1");

    pb::Manifest m1;
    std::uint64_t latest = 0;
    require(load_latest_manifest(ds, m1, latest, err) && latest == 1U, "load latest: " + err);
    require(m1.version == 1U, "manifest version");
    require(m1.data_format.file_format == "lance" && m1.data_format.version == "2.2", "data format");
    require(m1.writer_library == "nanolance" && m1.writer_version == nanolance_writer_version(), "writer");
    require(m1.has_timestamp && m1.timestamp_seconds > 1600000000 && m1.timestamp_nanos >= 0 &&
                m1.timestamp_nanos < 1000000000,
            "timestamp");
    require(m1.fields.size() == mapping.fields.size(), "all schema fields");
    check_field(m1.fields[0], "id", "int64", 0, -1, 1, false);
    check_field(m1.fields[1], "name", "string", 1, -1, 2, true);
    check_field(m1.fields[2], "s", "struct", 2, -1, 0, true);
    check_field(m1.fields[3], "x", "float", 3, 2, 1, false);
    check_field(m1.fields[4], "tag", "dict:string:uint32:true", 4, -1, 3, true);
    require(m1.fields[1].metadata.size() == 1U && m1.fields[1].metadata.count("owner") == 1U, "field metadata");
    require(m1.schema_metadata == schema_metadata, "schema metadata");
    require(m1.table_metadata == std::map<std::string, std::string>({{"owner", "team-a"}}), "table metadata");
    require(m1.config.at("k") == "v" && m1.config.at("lance.auto_cleanup.interval") == "1000", "initial config");
    require(m1.fragments.size() == 2U, "two fragments");
    require(m1.fragments[0].id == 0U && m1.fragments[1].id == 1U, "fragment ids");
    require(m1.fragments[0].physical_rows == 5U && m1.fragments[1].physical_rows == 7U, "fragment rows");
    require(m1.fragments[0].files.size() == 1U && m1.fragments[0].files[0].path == "a.lance", "fragment 0 file");
    require(m1.fragments[1].files[0].file_size_bytes == 200U, "fragment 1 file size");
    require(m1.fragments[0].files[0].file_major_version == 2U && m1.fragments[0].files[0].file_minor_version == 2U,
            "file version");
    require(m1.fragments[1].files[0].column_indices == std::vector<std::int32_t>({0, 1, 2, 3}), "column indices");
    std::optional<std::uint32_t> base;
    require(data_file_base_id(m1.fragments[1].files[0], base, err) && base && *base == 1U, "fragment 1 base id");
    require(data_file_base_id(m1.fragments[0].files[0], base, err) && !base, "fragment 0 in the root");
    require(m1.has_max_fragment_id && m1.max_fragment_id == 1U, "max fragment id");
    require((m1.reader_feature_flags & pb::kFlagStableRowIds) == 0U, "no stable row ids");
    require(!m1.transaction_file.empty(), "a transaction file is named");
    require(fs::exists(ds / "_transactions" / m1.transaction_file), "the transaction file exists");
    std::vector<BasePath> read_bases;
    require(read_base_paths(m1, read_bases, err) && read_bases.size() == 1U && read_bases[0].id == 1U &&
                read_bases[0].name == "other" && read_bases[0].path == "/tmp/other_root",
            "base paths");
    require(m1.next_row_id == 0U, "no row ids handed out");

    // V2 naming: u64::MAX - version in 20 digits.
    require(fs::exists(ds / "_versions" / "18446744073709551614.manifest"), "version 1 uses the V2 name");

    // Append: schema, config and table metadata carry over; initial_config is for creation only.
    CommitExtras append;
    append.table_metadata = {{"owner", "team-b"}, {"extra", "1"}};
    append.initial_config = {{"ignored", "x"}};
    LanceSchemaMapping other;
    other.fields.push_back(make_field("zzz", "int8", 9, -1, 0));
    require(commit_dataset_version(ds, other, {make_fragment("c.lance", 3, 50)}, CommitMode::Append, version, err, append),
            "append: " + err);
    require(version == 2U, "second version is 2");
    pb::Manifest m2;
    require(load_manifest_version(ds, 2, m2, err), "load v2: " + err);
    require(m2.fields.size() == mapping.fields.size() && m2.fields[0].name == "id", "append keeps the dataset's schema");
    require(m2.schema_metadata == schema_metadata, "append keeps schema metadata");
    require(m2.config == m1.config && m2.config.count("ignored") == 0U, "config carries over, initial_config ignored");
    require(m2.table_metadata == std::map<std::string, std::string>({{"owner", "team-b"}, {"extra", "1"}}),
            "table metadata upserted");
    require(m2.fragments.size() == 3U && m2.fragments[2].id == 2U && m2.fragments[2].physical_rows == 3U &&
                m2.fragments[2].files[0].path == "c.lance",
            "appended fragment");
    require(m2.fragments[2].files[0].column_indices == std::vector<std::int32_t>({0}), "mapping of the new file");
    require(m2.fragments[0].physical_rows == 5U && m2.fragments[1].physical_rows == 7U, "old fragments kept");
    require(m2.max_fragment_id == 2U, "max fragment id grows");
    require(m2.transaction_file != m1.transaction_file && !m2.transaction_file.empty(), "own transaction file");
    require(m2.timestamp_seconds >= m1.timestamp_seconds, "timestamps do not go backwards");
    require(read_base_paths(m2, read_bases, err) && read_bases.size() == 1U, "base paths carry over");

    // Overwrite: a new schema and only the new fragments, numbered after the highest id ever used.
    CommitExtras over;
    require(commit_dataset_version(ds, other, {make_fragment("d.lance", 9, 70)}, CommitMode::Overwrite, version, err, over),
            "overwrite: " + err);
    require(version == 3U, "third version is 3");
    pb::Manifest m3;
    require(load_manifest_version(ds, 3, m3, err), "load v3: " + err);
    require(m3.fields.size() == 1U && m3.fields[0].name == "zzz" && m3.fields[0].logical_type == "int8", "new schema");
    require(m3.fragments.size() == 1U && m3.fragments[0].id == 3U && m3.fragments[0].physical_rows == 9U, "only the new fragment");
    require(m3.max_fragment_id == 3U, "max fragment id keeps rising");
    require(m3.config == m1.config, "config survives an overwrite");
    require(m3.table_metadata == m2.table_metadata, "table metadata survives an overwrite");

    // An empty commit: no fragments, no max fragment id on a fresh dataset.
    const auto empty_ds = fresh_dir("empty");
    require(commit_dataset_version(empty_ds, mapping, {}, CommitMode::Create, version, err), "empty create: " + err);
    pb::Manifest me;
    require(load_manifest_version(empty_ds, 1, me, err) && me.fragments.empty() && !me.has_max_fragment_id &&
                me.fields.size() == mapping.fields.size(),
            "empty dataset");

    require(list_manifest_versions(ds, err) == std::vector<std::uint64_t>({1, 2, 3}), "versions listed");
    require(highest_manifest_version(ds, err) == 3U, "highest version");
    fs::remove_all(ds);
    fs::remove_all(empty_ds);
}

void test_write_dataset_manifest() {
    const auto ds = fresh_dir("simple");
    const auto mapping = make_mapping();
    DataFileResult file;
    file.relative_path = "data/one.lance";
    file.file_size_bytes = 321;
    std::uint64_t version = 0;
    std::string err;
    require(write_dataset_manifest(ds, mapping, file, 11, false, version, err) && version == 1U, "write: " + err);
    file.relative_path = "data/two.lance";
    require(write_dataset_manifest(ds, mapping, file, 4, true, version, err) && version == 2U, "append: " + err);
    pb::Manifest m;
    require(load_manifest_version(ds, 2, m, err) && m.fragments.size() == 2U && m.fragments[0].files[0].path == "one.lance" &&
                m.fragments[1].files[0].path == "two.lance" && m.fragments[1].physical_rows == 4U,
            "append adds a fragment");
    require(write_dataset_manifest(ds, mapping, file, 2, false, version, err) && version == 3U, "overwrite: " + err);
    require(load_manifest_version(ds, 3, m, err) && m.fragments.size() == 1U && m.fragments[0].physical_rows == 2U,
            "a non-append replaces the fragments");
    fs::remove_all(ds);
}

void test_stable_row_ids() {
    const auto ds = fresh_dir("stable");
    const auto mapping = make_mapping();
    CommitExtras extras;
    extras.stable_row_ids = true;
    std::uint64_t version = 0;
    std::string err;
    require(commit_dataset_version(ds, mapping, {make_fragment("a.lance", 5, 10), make_fragment("e.lance", 0, 10),
                                                 make_fragment("b.lance", 3, 10)},
                                   CommitMode::Create, version, err, extras),
            "create stable: " + err);
    pb::Manifest m;
    require(load_manifest_version(ds, 1, m, err), "load: " + err);
    require((m.reader_feature_flags & pb::kFlagStableRowIds) != 0U && (m.writer_feature_flags & pb::kFlagStableRowIds) != 0U,
            "stable row id flags");
    require(m.next_row_id == 8U, "next row id");
    RowIdSequence ids;
    require(fragment_row_ids(m.fragments[0], ids, err) && ids.size() == 5U && ids.at(0) == 0U && ids.at(4) == 4U,
            "fragment 0 row ids");
    require(fragment_row_ids(m.fragments[2], ids, err) && ids.size() == 3U && ids.at(0) == 5U && ids.at(2) == 7U,
            "fragment 2 row ids");
    FragmentRowMeta meta;
    require(read_fragment_row_meta(m.fragments[0], meta, err) && meta.has_created && meta.has_last_updated, "row versions");
    RowVersionSequence created;
    require(RowVersionSequence::decode(meta.created.data(), meta.created.size(), created, err) && created.size() == 5U &&
                created.at(0) == 1U,
            "rows are stamped with version 1");
    require(read_fragment_row_meta(m.fragments[1], meta, err) && meta.has_row_ids && !meta.has_created,
            "an empty fragment has ids but no stamps");

    // A later append (stable row ids not asked for again) keeps them, from the high-water mark.
    require(commit_dataset_version(ds, mapping, {make_fragment("c.lance", 2, 10)}, CommitMode::Append, version, err),
            "append: " + err);
    require(load_manifest_version(ds, 2, m, err) && m.next_row_id == 10U &&
                (m.reader_feature_flags & pb::kFlagStableRowIds) != 0U,
            "append continues the row ids");
    require(fragment_row_ids(m.fragments[3], ids, err) && ids.at(0) == 8U && ids.at(1) == 9U, "appended row ids");
    require(read_fragment_row_meta(m.fragments[3], meta, err) && meta.has_created, "appended rows are stamped");
    RowVersionSequence stamped;
    require(RowVersionSequence::decode(meta.created.data(), meta.created.size(), stamped, err) && stamped.at(0) == 2U,
            "stamped with version 2");
    fs::remove_all(ds);
}

void test_deletion_files() {
    const auto ds = fresh_dir("deletions");
    const auto mapping = make_mapping();
    std::uint64_t version = 0;
    std::string err;
    require(commit_dataset_version(ds, mapping, {make_fragment("a.lance", 10, 10)}, CommitMode::Create, version, err),
            "create: " + err);
    pb::Manifest m;
    require(load_manifest_version(ds, 1, m, err), "load: " + err);
    require((m.reader_feature_flags & pb::kFlagDeletionFiles) == 0U, "no deletion flag yet");

    pb::Manifest next = m;
    next.version = 2;
    next.fragments[0].deletion_file.present = true;
    next.fragments[0].deletion_file.file_type = 1;
    next.fragments[0].deletion_file.read_version = 1;
    next.fragments[0].deletion_file.id = 77;
    next.fragments[0].deletion_file.num_deleted_rows = 4;
    next.reader_feature_flags |= pb::kFlagDeletionFiles;
    next.writer_feature_flags |= pb::kFlagDeletionFiles;
    next.transaction_file.clear();
    require(publish_manifest(ds, next, err), "publish with deletions: " + err);

    pb::Manifest read;
    require(load_manifest_version(ds, 2, read, err), "load v2: " + err);
    const auto& del = read.fragments[0].deletion_file;
    require(del.present && del.file_type == 1U && del.read_version == 1U && del.id == 77U && del.num_deleted_rows == 4U,
            "deletion file fields");
    require(!read.transaction_file.empty(), "publish names a transaction");

    // An append carries the deletion file and the flag along.
    require(commit_dataset_version(ds, mapping, {make_fragment("b.lance", 1, 10)}, CommitMode::Append, version, err),
            "append: " + err);
    require(load_manifest_version(ds, 3, read, err), "load v3: " + err);
    const auto& kept = read.fragments[0].deletion_file;
    require(kept.present && kept.id == 77U && kept.num_deleted_rows == 4U && !read.fragments[1].deletion_file.present,
            "deletion file carried through the append");
    require((read.reader_feature_flags & pb::kFlagDeletionFiles) != 0U, "deletion flag kept");
    fs::remove_all(ds);
}

void test_refusals() {
    const auto ds = fresh_dir("refusals");
    const auto mapping = make_mapping();
    std::uint64_t version = 0;
    std::string err;
    require(!commit_dataset_version(ds, mapping, {}, CommitMode::Append, version, err) &&
                err.find("no manifest version") != std::string::npos,
            "append to nothing: " + err);
    require(commit_dataset_version(ds, mapping, {make_fragment("a.lance", 1, 1)}, CommitMode::Create, version, err),
            "create: " + err);
    require(!commit_dataset_version(ds, mapping, {}, CommitMode::Create, version, err) &&
                err.find("already exists") != std::string::npos,
            "create over a dataset: " + err);
    CommitExtras stable;
    stable.stable_row_ids = true;
    require(!commit_dataset_version(ds, mapping, {}, CommitMode::Append, version, err, stable) &&
                err.find("stable row ids") != std::string::npos,
            "stable row ids on a dataset without them: " + err);
    require(!commit_dataset_version(fs::path("nosuchscheme://bucket/ds"), mapping, {}, CommitMode::Create, version, err) &&
                err.find("not supported") != std::string::npos,
            "unsupported URI: " + err);

    // The same version published twice is a conflict, and the first stays as it was.
    pb::Manifest m;
    require(load_manifest_version(ds, 1, m, err), "load: " + err);
    pb::Manifest again = m;
    again.fragments.clear();
    again.transaction_file.clear();
    require(!publish_manifest(ds, again, err) && err.find("commit conflict") != std::string::npos,
            "version published twice: " + err);
    pb::Manifest still;
    require(load_manifest_version(ds, 1, still, err) && still.fragments.size() == 1U, "first publish untouched");
    // The refused publish leaves no transaction file behind.
    std::size_t transactions = 0;
    for (const auto& e : fs::directory_iterator(ds / "_transactions")) {
        (void)e;
        ++transactions;
    }
    require(transactions == 1U, "no stray transaction file");

    // A manifest this build cannot write (unknown writer flag) is refused before anything is written.
    pb::Manifest future = m;
    future.version = 2;
    future.writer_feature_flags |= (1ULL << 40U);
    require(!publish_manifest(ds, future, err) && !fs::exists(ds / "_versions" / "18446744073709551613.manifest"),
            "unknown writer feature flag");
    fs::remove_all(ds);
}

}  // namespace

int main() {
    test_fields();
    test_data_fragment();
    test_commit_cycle();
    test_write_dataset_manifest();
    test_stable_row_ids();
    test_deletion_files();
    test_refusals();
    std::cout << "manifest_writer: ok\n";
    return 0;
}
