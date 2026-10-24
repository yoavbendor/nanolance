// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// dataset_refs: a dataset's named references. Tags (create at a version or the latest, get, list, update,
// replace metadata, delete) and branches (create from main or from another branch, list with their lineage,
// replace metadata, delete) on a small dataset committed by the manifest writer, with the refusals Lance makes
// and its messages: invalid names, duplicates, missing refs and versions, deleting main or a branch another
// branch or a tag still names. Also drop_dataset's evidence rule and humantime-style duration parsing.

#include "nanolance/dataset_refs.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

fs::path fresh_dir(const std::string& name) {
    auto p = fs::temp_directory_path() / ("nano_lance_dataset_refs_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

// A dataset of `versions` versions (each an append of one 3-row fragment).
fs::path make_dataset(const std::string& name, int versions) {
    const auto ds = fresh_dir(name);
    LanceSchemaMapping mapping;
    LanceField id;
    id.name = "id";
    id.logical_type = "int64";
    id.id = 0;
    id.column_index = 0;
    mapping.fields.push_back(id);
    for (int v = 1; v <= versions; ++v) {
        NewFragment f;
        f.data_file.relative_path = "data/f" + std::to_string(v) + ".lance";
        f.data_file.file_size_bytes = 10;
        f.rows = 3;
        std::uint64_t version = 0;
        std::string err;
        require(commit_dataset_version(ds, mapping, {f}, v == 1 ? CommitMode::Create : CommitMode::Append, version, err),
                "commit: " + err);
        require(version == static_cast<std::uint64_t>(v), "version numbering");
    }
    return ds;
}

const TagInfo* find_tag(const std::vector<TagInfo>& tags, const std::string& name) {
    for (const auto& t : tags) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

const BranchInfo* find_branch(const std::vector<BranchInfo>& branches, const std::string& name) {
    for (const auto& b : branches) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

void test_tags() {
    const auto ds = make_dataset("tags", 3);
    std::string err;
    std::vector<TagInfo> tags;
    require(list_tags(ds, tags, err) && tags.empty(), "no tags yet: " + err);

    require(create_tag(ds, "v1", "", 1, err), "create v1: " + err);
    require(create_tag(ds, "latest", "", 0, err), "create at the latest: " + err);
    require(create_tag(ds, "caf\xC3\xA9-1.0_x", "", 2, err), "non-ASCII, dots, dashes and underscores are fine: " + err);

    require(list_tags(ds, tags, err) && tags.size() == 3U, "three tags listed");
    const auto* v1 = find_tag(tags, "v1");
    require(v1 != nullptr && v1->version == 1U && !v1->branch && v1->metadata.empty(), "v1");
    require(v1->created_at[4] == '-' && v1->created_at[10] == 'T' &&
                v1->created_at.back() == 'Z',
            "created_at is RFC 3339 UTC (fractional seconds, as Lance writes): " + v1->created_at);
    require(!v1->updated_at.empty(), "updated_at set");
    const auto* latest = find_tag(tags, "latest");
    require(latest != nullptr && latest->version == 3U, "version 0 means the latest");
    require(find_tag(tags, "caf\xC3\xA9-1.0_x") != nullptr, "unicode name round trip");

    TagInfo got;
    require(get_tag(ds, "v1", got, err) && got.name == "v1" && got.version == 1U && got.created_at == v1->created_at,
            "get_tag agrees with list_tags");
    // manifest_size is the size of the manifest file the tag names.
    std::uint64_t size_v1 = 0;
    for (const auto& e : fs::directory_iterator(ds / "_versions")) {
        std::uint64_t v = 0;
        if (parse_manifest_version(e.path().filename().string(), v) && v == 1U) {
            size_v1 = fs::file_size(e.path());
        }
    }
    require(size_v1 != 0U && got.manifest_size == size_v1, "manifest size");

    // Refusals.
    require(!create_tag(ds, "v1", "", 2, err) && has(err, "Ref conflict error") && has(err, "v1"), "duplicate: " + err);
    require(get_tag(ds, "v1", got, err) && got.version == 1U, "a refused duplicate leaves the tag alone");
    require(!create_tag(ds, "new", "", 99, err) && has(err, "Version not found error"), "missing version: " + err);
    require(!create_tag(ds, "new", "nobranch", 0, err) && has(err, "Version not found error"), "missing branch: " + err);
    TagInfo none;
    require(!get_tag(ds, "new", none, err) && has(err, "Ref not found error"), "a refused create writes nothing: " + err);
    for (const char* bad : {"", ".hidden", "trailing.", "a..b", "x.lock", "a/b", "a b", "a:b", "a*", "a\\b"}) {
        require(!create_tag(ds, bad, "", 1, err) && has(err, "Ref is invalid"), std::string("bad tag name '") + bad + "': " + err);
        require(!get_tag(ds, bad, none, err) && has(err, "Ref is invalid"), std::string("get bad name '") + bad + "'");
        require(!delete_tag(ds, bad, err) && has(err, "Ref is invalid"), std::string("delete bad name '") + bad + "'");
    }
    require(!get_tag(ds, "missing", none, err) && has(err, "Ref not found error"), "get missing: " + err);
    require(!delete_tag(ds, "missing", err) && has(err, "Ref not found error"), "delete missing: " + err);
    require(!update_tag(ds, "missing", "", 1, err) && has(err, "Ref not found error"), "update missing: " + err);
    require(!replace_tag_metadata(ds, "missing", {}, err) && has(err, "Ref not found error"), "metadata of missing: " + err);

    // Metadata replaces whole and survives the odd characters JSON escapes.
    const std::map<std::string, std::string> md{{"note", "line1\nline2 \"quoted\" \\ back"}, {"caf\xC3\xA9", "\xE2\x82\xAC"}, {"", ""}};
    require(replace_tag_metadata(ds, "v1", md, err), "replace metadata: " + err);
    require(get_tag(ds, "v1", got, err) && got.metadata == md && got.version == 1U && got.created_at == v1->created_at,
            "metadata round trip, version and creation time unchanged");
    require(replace_tag_metadata(ds, "v1", {{"only", "this"}}, err) && get_tag(ds, "v1", got, err) &&
                got.metadata == std::map<std::string, std::string>({{"only", "this"}}),
            "metadata is replaced, not merged");

    // update_tag moves the tag and keeps its creation time and metadata.
    require(update_tag(ds, "v1", "", 3, err), "update: " + err);
    require(get_tag(ds, "v1", got, err) && got.version == 3U && got.created_at == v1->created_at &&
                got.metadata == std::map<std::string, std::string>({{"only", "this"}}),
            "updated tag");
    require(!update_tag(ds, "v1", "", 42, err) && has(err, "Version not found error"), "update to a missing version: " + err);
    require(get_tag(ds, "v1", got, err) && got.version == 3U, "refused update leaves the tag");

    // Delete.
    require(delete_tag(ds, "v1", err), "delete: " + err);
    require(!get_tag(ds, "v1", got, err) && has(err, "Ref not found error"), "deleted tag is gone");
    require(!delete_tag(ds, "v1", err), "deleting twice fails");
    require(list_tags(ds, tags, err) && tags.size() == 2U && find_tag(tags, "v1") == nullptr, "list after delete");
    require(create_tag(ds, "v1", "", 2, err), "a deleted name can be reused: " + err);
    fs::remove_all(ds);
}

void test_branches() {
    const auto ds = make_dataset("branches", 3);
    std::string err;
    std::vector<BranchInfo> branches;
    require(list_branches(ds, branches, err) && branches.empty(), "no branches yet: " + err);
    require(branch_location(ds, "") == ds && branch_location(ds, "b1") == ds / "tree" / "b1", "branch_location");

    require(create_branch(ds, "b1", "", 2, err), "create b1: " + err);
    require(create_branch(ds, "team/feature-1.x_y", "", 3, err), "nested name: " + err);
    pb::Manifest first;
    std::uint64_t latest = 0;
    require(load_latest_manifest(branch_location(ds, "b1"), first, latest, err), "b1 has a manifest: " + err);
    require(first.fragments.size() == 2U, "b1 forked at version 2 holds its 2 fragments");

    require(list_branches(ds, branches, err), "list: " + err);
    const auto* b1 = find_branch(branches, "b1");
    require(b1 != nullptr && !b1->parent_branch && b1->parent_version == 2U && b1->identifier.size() == 1U &&
                b1->identifier[0].first == 2U && !b1->identifier[0].second.empty() && b1->create_at > 1600000000U &&
                b1->manifest_size > 0U && b1->metadata.empty(),
            "b1 lineage");
    const auto* nested = find_branch(branches, "team/feature-1.x_y");
    require(nested != nullptr && nested->parent_version == 3U, "nested branch listed under its own name");
    require(fs::exists(branch_location(ds, "team/feature-1.x_y") / "_versions"), "nested branch has its versions");

    // A child of b1: its lineage extends b1's.
    require(create_branch(ds, "child", "b1", latest, err), "child of b1: " + err);
    require(list_branches(ds, branches, err), "list: " + err);
    const auto* child = find_branch(branches, "child");
    b1 = find_branch(branches, "b1");
    require(child != nullptr && child->parent_branch && *child->parent_branch == "b1" && child->identifier.size() == 2U &&
                child->identifier[0] == b1->identifier[0],
            "child lineage");

    // Refusals.
    require(!create_branch(ds, "b1", "", 1, err) && has(err, "Ref conflict error"), "duplicate: " + err);
    for (const char* bad : {"", "main", "/a", "a/", "a//b", "a..b", "a\\b", "a b", "x.lock", "a/.b/c:d", "a/ /b"}) {
        require(!create_branch(ds, bad, "", 1, err) && has(err, "Ref is invalid"), std::string("bad branch name '") + bad + "': " + err);
    }
    require(!create_branch(ds, "nope", "ghost", 1, err) && has(err, "Ref not found error"), "missing parent: " + err);
    require(!create_branch(ds, "nope", "", 99, err) && has(err, "Version not found error"), "missing version: " + err);
    require(!create_branch(ds, "nope", "", 0, err), "version 0 is not a version to fork at");
    require(list_branches(ds, branches, err) && find_branch(branches, "nope") == nullptr, "refused creates leave nothing");
    require(!fs::exists(branch_location(ds, "nope")), "...nor a directory");

    // Metadata.
    const std::map<std::string, std::string> md{{"owner", "me"}, {"q", "\"x\""}};
    require(replace_branch_metadata(ds, "b1", md, err), "branch metadata: " + err);
    require(list_branches(ds, branches, err), "list: " + err);
    b1 = find_branch(branches, "b1");
    require(b1->metadata == md && b1->parent_version == 2U && b1->identifier.size() == 1U, "metadata stored, lineage kept");
    require(!replace_branch_metadata(ds, "ghost", md, err) && has(err, "Ref not found error"), "metadata of a missing branch: " + err);
    require(!replace_branch_metadata(ds, "main", md, err) && has(err, "Ref is invalid"), "metadata of main: " + err);

    // Tags on a branch.
    require(create_tag(ds, "on-b1", "b1", 0, err), "tag on a branch: " + err);
    TagInfo tag;
    require(get_tag(ds, "on-b1", tag, err) && tag.branch && *tag.branch == "b1" && tag.version == latest, "branch tag");

    // Deletion refusals, in Lance's order: main, missing, children, tags.
    require(!delete_branch(ds, "main", err) && has(err, "Ref is invalid") && has(err, "'main'"), "delete main: " + err);
    require(!delete_branch(ds, "ghost", err) && has(err, "Ref not found error"), "delete missing: " + err);
    require(!delete_branch(ds, "b1", err) && has(err, "Ref conflict error") && has(err, "child"), "b1 has a child: " + err);
    require(fs::exists(branch_location(ds, "b1") / "_versions"), "refused delete keeps the branch");
    require(delete_branch(ds, "child", err), "delete child: " + err);
    require(!fs::exists(branch_location(ds, "child")), "child's directory is gone");
    require(!delete_branch(ds, "b1", err) && has(err, "Ref conflict error") && has(err, "tags") && has(err, "on-b1"),
            "b1 is named by a tag: " + err);
    require(delete_tag(ds, "on-b1", err), "untag: " + err);
    require(delete_branch(ds, "b1", err), "delete b1: " + err);
    require(!fs::exists(branch_location(ds, "b1")), "b1's directory is gone");
    require(delete_branch(ds, "team/feature-1.x_y", err), "delete nested: " + err);
    require(!fs::exists(ds / "tree" / "team"), "the empty parent directory goes too");
    require(list_branches(ds, branches, err) && branches.empty(), "no branches left");
    // A name can come back.
    require(create_branch(ds, "b1", "", 1, err), "recreate: " + err);
    fs::remove_all(ds);
}

void test_drop() {
    std::string err;
    const auto missing = fresh_dir("drop_missing");
    require(!drop_dataset(missing, false, err) && has(err, "was not found"), "drop a missing path: " + err);
    require(drop_dataset(missing, true, err), "...unless not-found is ignored");

    // A directory with files but no manifest is not a dataset: refused, and untouched.
    const auto stranger = fresh_dir("drop_stranger");
    fs::create_directories(stranger);
    std::ofstream(stranger / "precious.txt") << "keep me";
    require(!drop_dataset(stranger, false, err) && !err.empty(), "drop a non-dataset: " + err);
    require(fs::exists(stranger / "precious.txt"), "its files survive");
    fs::remove_all(stranger);

    const auto empty = fresh_dir("drop_empty");
    fs::create_directories(empty);
    require(drop_dataset(empty, false, err) && !fs::exists(empty), "an empty directory may be dropped");

    const auto ds = make_dataset("drop", 2);
    require(create_tag(ds, "t", "", 0, err), "tag: " + err);
    require(drop_dataset(ds, false, err), "drop a dataset: " + err);
    require(!fs::exists(ds), "everything under the root is gone");
}

void test_duration() {
    std::int64_t ns = -1;
    const auto ok = [&](const std::string& text, std::int64_t expect) {
        ns = -1;
        require(parse_duration(text, ns) && ns == expect, "duration '" + text + "' = " + std::to_string(ns));
    };
    ok("1s", 1000000000LL);
    ok("250ms", 250000000LL);
    ok("14days", 14LL * 86400LL * 1000000000LL);
    ok("1h 30m", 5400LL * 1000000000LL);
    ok("1h30m", 5400LL * 1000000000LL);
    ok("2 weeks", 14LL * 86400LL * 1000000000LL);
    ok("0s", 0);
    ok("5ns", 5);
    ok("  3 seconds ", 3000000000LL);
    for (const char* bad : {"", " ", "s", "10", "1x", "1.5s", "-1s", "1 s s"}) {
        require(!parse_duration(bad, ns), std::string("bad duration '") + bad + "'");
    }
    require(!parse_duration("99999999999y", ns), "too large for nanoseconds in an int64");
    require(!parse_duration(std::string(400, '9') + "s", ns), "a number too large for a double");
}

}  // namespace

int main() {
    test_tags();
    test_branches();
    test_drop();
    test_duration();
    std::cout << "dataset_refs: ok\n";
    return 0;
}
