// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance's fragment-reuse index: after a deferred compaction, how each old row address moves to its new
// address (or says the row is gone), and how an index's fragment coverage is carried across the rewrite.
// Indexes built before a compaction keep old addresses, so this map is what keeps their answers right.
// The index is written by fragment_reuse_index_entry and read back by FragReuse::from_details, with inline
// details, so the whole round trip runs in memory with no dataset on disk.

#include "lance_minimal.pb.hpp"
#include "nanolance/frag_reuse.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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

constexpr std::uint64_t addr(std::uint64_t fragment, std::uint64_t offset) {
    return (fragment << 32U) | offset;
}

// The address of `fragment`'s row `offset`, resolved through `remap`; false when the row is gone.
bool resolve(const nano_lance::FragReuse& remap, std::uint64_t a, std::uint64_t& out) {
    return remap.resolve(a, out);
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    const std::filesystem::path nowhere = "/nonexistent-dataset";  // inline details never read a file
    const pb::Manifest no_manifest;

    // Version 1: fragments 0 (4 rows, 2 deleted) and 1 (3 rows, 1 deleted) are compacted into fragment 5.
    // Their survivors, in address order: 0/1, 0/3, 1/0, 1/2 -- which become 5/0 .. 5/3.
    nano_lance::FragReuseWriteGroup compaction;
    compaction.changed_row_addrs = {addr(0, 1), addr(0, 3), addr(1, 0), addr(1, 2)};
    compaction.old_fragments = {{0, 4, 2}, {1, 3, 1}};
    compaction.new_fragments = {{5, 4, 0}};

    pb::IndexMetadata first;
    {
        std::string error;
        require(nano_lance::fragment_reuse_index_entry(nullptr, 7, {compaction}, {0, 1, 7}, first, error),
                "write version 1: ", error);
        require(first.name == "__lance_frag_reuse", "the entry is the fragment reuse index");
    }

    std::shared_ptr<const nano_lance::FragReuse> remap;
    {
        std::string error;
        require(nano_lance::FragReuse::from_details(nowhere, no_manifest, first, remap, error),
                "read version 1: ", error);
        require(remap != nullptr && !remap->empty(), "one version was read");
    }

    // Each survivor moves to its new address; a row the compaction dropped says so.
    {
        std::uint64_t out = 0;
        require(resolve(*remap, addr(0, 1), out) && out == addr(5, 0), "fragment 0 row 1 moves to 5/0");
        require(resolve(*remap, addr(0, 3), out) && out == addr(5, 1), "fragment 0 row 3 moves to 5/1");
        require(resolve(*remap, addr(1, 0), out) && out == addr(5, 2), "fragment 1 row 0 moves to 5/2");
        require(resolve(*remap, addr(1, 2), out) && out == addr(5, 3), "fragment 1 row 2 moves to 5/3");

        require(!resolve(*remap, addr(0, 0), out), "a row deleted before the compaction is gone");
        require(!resolve(*remap, addr(0, 2), out), "a deleted row of fragment 0 is gone");
        require(!resolve(*remap, addr(1, 1), out), "a deleted row of fragment 1 is gone");

        require(resolve(*remap, addr(9, 4), out) && out == addr(9, 4), "a fragment nothing rewrote is unchanged");
        require(resolve(*remap, addr(0, 7), out) && out == addr(0, 7), "an address past the rows is unchanged");
    }

    // touches: only the fragments the compaction rewrote.
    {
        require(remap->touches(0) && remap->touches(1), "the compacted fragments are touched");
        require(!remap->touches(5) && !remap->touches(9), "the new and unrelated fragments are not touched");
    }

    // An index's coverage: a group it holds whole swaps its old fragments for the new ones; a group it
    // holds only in part is dropped from it, so its rows are scanned instead.
    {
        std::vector<std::uint32_t> ids = {0, 1, 7};
        remap->remap_coverage(ids);
        require(ids == std::vector<std::uint32_t>({5, 7}), "the whole group swaps 0 and 1 for 5");

        ids = {0, 7};
        remap->remap_coverage(ids);
        require(ids == std::vector<std::uint32_t>({7}), "a part-held group is dropped, not swapped");

        ids = {2, 3};
        remap->remap_coverage(ids);
        require(ids == std::vector<std::uint32_t>({2, 3}), "a coverage that holds none of the group is unchanged");
    }

    // Version 2: a later compaction rewrites fragment 5's rows 1 and 2 into fragment 6. Read on top of
    // version 1, so an address moves through both versions in order.
    {
        nano_lance::FragReuseWriteGroup second;
        second.changed_row_addrs = {addr(5, 1), addr(5, 2)};
        second.old_fragments = {{5, 4, 2}};
        second.new_fragments = {{6, 2, 0}};

        pb::IndexMetadata chained;
        std::string error;
        require(nano_lance::fragment_reuse_index_entry(&first, 8, {second}, {5, 7}, chained, error),
                "write version 2: ", error);

        std::shared_ptr<const nano_lance::FragReuse> two;
        require(nano_lance::FragReuse::from_details(nowhere, no_manifest, chained, two, error),
                "read both versions: ", error);

        std::uint64_t out = 0;
        require(resolve(*two, addr(0, 3), out) && out == addr(6, 0), "an address moves through both versions");
        require(resolve(*two, addr(1, 0), out) && out == addr(6, 1), "the second version's rows are counted too");
        require(!resolve(*two, addr(0, 1), out), "a row the second compaction dropped is gone");
        require(!resolve(*two, addr(1, 2), out), "and so is one of the second version's dropped rows");

        std::vector<std::uint32_t> ids = {0, 1, 7};
        two->remap_coverage(ids);
        require(ids == std::vector<std::uint32_t>({6, 7}), "the coverage follows both rewrites");
    }

    // Refused: a new fragment that does not hold the rows the group rewrote.
    {
        nano_lance::FragReuseWriteGroup short_group = compaction;
        short_group.new_fragments = {{5, 3, 0}};  // 4 rows rewritten, 3 rows of room

        pb::IndexMetadata bad;
        std::string error;
        require(nano_lance::fragment_reuse_index_entry(nullptr, 7, {short_group}, {0}, bad, error),
                "the entry is written; the check is on reading it back");
        std::shared_ptr<const nano_lance::FragReuse> rejected;
        require(!nano_lance::FragReuse::from_details(nowhere, no_manifest, bad, rejected, error),
                "a group that rewrote more rows than its new fragments hold is refused");
        require(error.find("a group rewrote 4 rows into fragments holding 3") != std::string::npos,
                "the error gives both counts: ", error);
        require(rejected == nullptr, "a refused read yields no index");
    }

    // Refused: an index of another name is not a fragment reuse index.
    {
        pb::IndexMetadata other = first;
        other.name = "btree";
        other.details_type_url = "/lance.table.BTreeIndexDetails";
        std::shared_ptr<const nano_lance::FragReuse> rejected;
        std::string error;
        require(!nano_lance::FragReuse::from_details(nowhere, no_manifest, other, rejected, error),
                "a different index's details are refused");
        require(error == "index 'btree' is not a fragment reuse index", "the error names it: ", error);
    }

    std::cout << "fragment reuse: address resolution across versions, coverage remap, refusals OK\n";
    return 0;
}
