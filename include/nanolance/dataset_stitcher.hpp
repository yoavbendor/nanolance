#pragma once

// Gather/merge: stitch many free-standing Lance datasets (e.g. one per scatter worker) into a single
// self-contained dataset, with NO data re-encode. Each source's fragments are relocated into one
// `<master>/data/` directory and one master manifest is written referencing them all, in the order the
// caller passes the sources. This is the "manifest stitch" merge: the bytes of the column data files are
// never read or rewritten — only the (small) manifest is produced and the data files are moved.
//
// Preconditions: every source is a nanolance-written dataset with an IDENTICAL schema (same field ids /
// types / names / nullability). Fragment ids are renumbered globally; row order in a scan of the master is
// fragment order, i.e. the caller's source order. Global sort / de-duplication is a separate compaction
// step, not done here.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nano_lance {

struct StitchOptions {
    // Default workflow: MOVE each fragment file into the master, then delete the emptied source folders.
    bool move_files = true;       // false => copy (leave source data in place) — for debugging only.
    bool remove_sources = true;   // after a successful move, delete each (now hollow) source folder.
};

struct StitchSummary {
    std::size_t source_count = 0;
    std::uint64_t fragment_count = 0;
    std::uint64_t total_rows = 0;
};

// Merge `sources` (in the given order) into a fresh master dataset at `master_path`. `master_path` must not
// already contain a manifest. On success writes `<master_path>/_versions/1.manifest` + the relocated data
// files under `<master_path>/data/`. Returns false (with `error` set) on any I/O error or schema mismatch.
bool stitch_datasets(const std::filesystem::path& master_path,
                     const std::vector<std::filesystem::path>& sources,
                     const StitchOptions& options,
                     StitchSummary& summary,
                     std::string& error);

}  // namespace nano_lance
