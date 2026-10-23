// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/dataset_stitcher.hpp"

#include "lance_minimal.pb.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/path_safety.hpp"

#include <limits>
#include <system_error>

namespace nano_lance {
namespace {

// Two schemas are stitch-compatible iff their fields match structurally (same readable schema). Field
// `metadata` is annotation and is intentionally not compared — workers may attach incidental per-capture KV.
bool fields_match(const std::vector<pb::Field>& a, const std::vector<pb::Field>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].id != b[i].id || a[i].parent_id != b[i].parent_id ||
            a[i].logical_type != b[i].logical_type || a[i].type != b[i].type || a[i].nullable != b[i].nullable) {
            return false;
        }
    }
    return true;
}

// Move (default) or copy one data file into the master, with a cross-device fallback for move.
bool relocate(const std::filesystem::path& src, const std::filesystem::path& dst, bool move_file,
              std::string& error) {
    std::error_code ec;
    if (move_file) {
        std::filesystem::rename(src, dst, ec);
        if (!ec) {
            return true;
        }
        ec.clear();  // rename across volumes fails (EXDEV) — fall back to copy + remove.
    }
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "failed to copy " + src.string() + " -> " + dst.string() + ": " + ec.message();
        return false;
    }
    if (move_file) {
        std::filesystem::remove(src, ec);  // best effort; the source folder is removed wholesale later.
    }
    return true;
}

}  // namespace

bool stitch_datasets(const std::filesystem::path& master_path,
                     const std::vector<std::filesystem::path>& sources, const StitchOptions& options,
                     StitchSummary& summary, std::string& error) {
    error.clear();
    summary = StitchSummary{};
    if (sources.empty()) {
        error = "stitch: no source datasets";
        return false;
    }

    // Refuse to clobber an existing master.
    std::string verr;
    if (highest_manifest_version(master_path, verr) > 0) {
        error = "stitch: master already has a manifest: " + master_path.string();
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(master_path / "data", ec);
    if (ec) {
        error = "stitch: failed to create master data dir: " + ec.message();
        return false;
    }

    pb::Manifest master;
    master.version = 1;
    std::uint64_t global_frag_id = 0;

    for (std::size_t s = 0; s < sources.size(); ++s) {
        const auto& src = sources[s];
        pb::Manifest m;
        std::uint64_t ver = 0;
        if (!load_latest_manifest(src, m, ver, error)) {
            error = "stitch: read manifest of " + src.string() + ": " + error;
            return false;
        }

        if (s == 0) {
            master.fields = m.fields;            // adopt the first source's schema...
            master.data_format = m.data_format;  // ...and its Lance file-format version.
        } else if (!fields_match(master.fields, m.fields)) {
            error = "stitch: schema of " + src.string() + " differs from " + sources[0].string();
            return false;
        }

        for (const auto& frag : m.fragments) {
            pb::DataFragment out_frag;
            out_frag.id = global_frag_id;
            out_frag.physical_rows = frag.physical_rows;
            for (std::size_t k = 0; k < frag.files.size(); ++k) {
                const pb::DataFile& in_file = frag.files[k];
                // Deterministic, collision-free name matching nanolance's own convention, so the stitched
                // dataset is indistinguishable from a single-run one (every worker ships a fragment-0.lance).
                std::string new_name = "fragment-" + std::to_string(global_frag_id);
                if (frag.files.size() > 1) {
                    new_name += "-" + std::to_string(k);
                }
                new_name += ".lance";
                // in_file.path comes from an untrusted source manifest; confine it under <src>/data/ so a
                // hostile ".."/absolute path can't make the stitcher relocate a file outside the dataset.
                const auto in_path = safe_join_under(src / "data", in_file.path);
                if (!in_path) {
                    error = "stitch: data file path escapes the dataset directory";
                    return false;
                }
                if (!relocate(*in_path, master_path / "data" / new_name, options.move_files, error)) {
                    return false;
                }
                pb::DataFile out_file = in_file;  // keep field ids / column indices / size / version
                out_file.path = new_name;
                out_frag.files.push_back(std::move(out_file));
            }
            master.fragments.push_back(std::move(out_frag));
            summary.total_rows += frag.physical_rows;
            ++global_frag_id;
        }
    }

    master.has_max_fragment_id = true;
    master.max_fragment_id = global_frag_id == 0
                                 ? 0
                                 : static_cast<std::uint32_t>(global_frag_id - 1);
    if (global_frag_id > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        error = "stitch: fragment id overflow";
        return false;
    }

    if (!publish_manifest(master_path, master, error)) {
        return false;
    }

    // Remove the now-hollow source folders (best effort; their data files have been moved out).
    if (options.move_files && options.remove_sources) {
        for (const auto& src : sources) {
            std::error_code rmec;
            std::filesystem::remove_all(src, rmec);  // best effort — a leftover folder is not a failure.
        }
    }

    summary.source_count = sources.size();
    summary.fragment_count = global_frag_id;
    return true;
}

}  // namespace nano_lance
