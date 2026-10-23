// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Keeping a dataset's indices honest across the commits nanolance makes: which indices survive a schema
// change (a dropped field takes its index with it; the system indices always stay), which segments of one
// index are still worth keeping once their fragments are gone, how a compaction takes fragments out of an
// index's coverage, and the first fragment id a new fragment may take without an index claiming it.

#include "lance_minimal.pb.hpp"
#include "nanolance/index_maintenance.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
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

pb::IndexMetadata index(const std::string& name, std::vector<std::int32_t> fields, std::uint64_t version,
                        std::vector<std::uint32_t> fragments, bool has_bitmap = true) {
    pb::IndexMetadata out;
    out.name = name;
    out.fields = std::move(fields);
    out.dataset_version = version;
    out.fragment_ids = std::move(fragments);
    out.has_fragment_bitmap = has_bitmap;
    return out;
}

pb::Field field(std::int32_t id) {
    pb::Field f;
    f.id = id;
    f.name = "f" + std::to_string(id);
    return f;
}

pb::DataFragment fragment(std::uint64_t id) {
    pb::DataFragment f;
    f.id = id;
    return f;
}

// "name@version" for each index, in order: how a test reads back what was kept.
std::vector<std::string> names(const std::vector<pb::IndexMetadata>& indices) {
    std::vector<std::string> out;
    for (const auto& i : indices) {
        out.push_back(i.name + "@" + std::to_string(i.dataset_version));
    }
    return out;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    // System indices are kept whatever their fields; others are not.
    {
        require(nano_lance::is_system_index(index("__lance_frag_reuse", {}, 1, {})), "the fragment reuse index");
        require(nano_lance::is_system_index(index("__lance_mem_wal", {}, 1, {})), "the MemWAL index");
        require(!nano_lance::is_system_index(index("user_idx", {1}, 1, {0})), "a user index is not a system index");
    }

    // retain_relevant_indices: schema, coverage and segments decide what stays.
    {
        const std::vector<pb::Field> schema = {field(1), field(2)};
        const std::vector<pb::DataFragment> fragments = {fragment(0), fragment(1)};

        std::vector<pb::IndexMetadata> indices = {
            index("kept", {1}, 1, {0}),                       // its field exists, it covers fragment 0
            index("dropped_field", {9}, 1, {0}),              // its field is gone from the schema
            index("__lance_frag_reuse", {9}, 2, {}),          // a system index stays regardless
            index("needs_all", {1, 9}, 1, {0}),               // one of its two fields is gone
            index("segs", {1}, 3, {7}),                       // a segment covering no existing fragment...
            index("segs", {1}, 4, {0}),                       // ...is dropped, this one stays
            index("gone", {2}, 2, {8}),                       // two segments, neither covers a live fragment:
            index("gone", {2}, 5, {9}),                       // the oldest is kept
            index("unknown_cover", {1}, 1, {}, false),        // no known coverage: kept
            index("unknown_cover", {1}, 2, {}, false),        // ... and so is every such segment
        };
        nano_lance::retain_relevant_indices(indices, schema, fragments);

        const std::vector<std::string> expected = {
            "kept@1",           "__lance_frag_reuse@2", "segs@4",          "gone@2",
            "unknown_cover@1",  "unknown_cover@2",
        };
        require(names(indices) == expected, "the indices kept, in order, are the ones the rules keep");
    }

    // drop_fragments_from_indices: taken out of coverage, flagged only where something changed.
    {
        std::vector<pb::IndexMetadata> indices = {
            index("covered", {1}, 1, {0, 1, 2}),
            index("uncovered_by_drop", {1}, 1, {5}),
            index("no_bitmap", {1}, 1, {1}, false),
        };
        nano_lance::drop_fragments_from_indices(indices, {1, 3});

        require(indices[0].fragment_ids == std::vector<std::uint32_t>({0, 2}), "fragment 1 leaves the coverage");
        require(indices[0].fragment_bitmap_changed, "a changed coverage is flagged for rewrite");
        require(indices[1].fragment_ids == std::vector<std::uint32_t>({5}), "a coverage without the fragments is kept");
        require(!indices[1].fragment_bitmap_changed, "an unchanged coverage is not flagged");
        require(!indices[2].fragment_bitmap_changed && indices[2].fragment_ids == std::vector<std::uint32_t>({1}),
                "an index with no bitmap is left alone");
    }

    // first_fragment_id_after_indices: one past the highest id any index claims; none claimed means 0.
    {
        require(nano_lance::first_fragment_id_after_indices({}) == 0, "no indices: the first id is 0");

        std::vector<pb::IndexMetadata> indices = {
            index("a", {1}, 1, {0, 4}),
            index("b", {1}, 1, {2}),
            index("empty", {1}, 1, {}),
        };
        require(nano_lance::first_fragment_id_after_indices(indices) == 5,
                "one past the highest claimed fragment (4)");
    }

    std::cout << "index maintenance: schema and segment retention, coverage drops, id reservation OK\n";
    return 0;
}
