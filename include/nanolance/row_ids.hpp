// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nano_lance {

/// The row ids of one fragment of a dataset with stable row ids, in the order of the fragment's rows
/// (physical rows, deleted ones included): Lance's RowIdSequence (rowids.proto), which is what
/// `DataFragment.inline_row_ids` holds. A row's id never changes when it is updated, compacted or
/// moved to another fragment; its address (fragment id << 32 | offset) does.
///
/// Kept expanded -- a contiguous run (the common case: `start, start + 1, ...`) as just its start
/// and length, anything else as a vector.
class RowIdSequence {
public:
    RowIdSequence() = default;

    /// `count` consecutive ids starting at `start`.
    static RowIdSequence range(std::uint64_t start, std::uint64_t count);
    static RowIdSequence from_values(std::vector<std::uint64_t> values);

    /// Decode a serialized RowIdSequence message. False (and `error`) when it is malformed: a segment
    /// whose bounds, holes or order are inconsistent, as Lance validates them.
    static bool decode(const std::uint8_t* data, std::size_t size, RowIdSequence& out, std::string& error);

    /// The message, with each segment encoded as Lance chooses: a plain range, a range with holes, a
    /// range with a bitmap or a sorted / unsorted array, whichever is smallest.
    std::vector<std::uint8_t> encode() const;

    std::uint64_t size() const { return compact_ ? count_ : values_.size(); }
    bool empty() const { return size() == 0; }
    std::uint64_t at(std::uint64_t index) const { return compact_ ? start_ + index : values_[index]; }
    /// A single run `start .. start + size`; `start` is set when it is one.
    bool is_range(std::uint64_t& start) const;

    /// The ids at `offsets` (row offsets of the fragment); False when one is out of range.
    bool select(const std::vector<std::uint64_t>& offsets, std::vector<std::uint64_t>& out) const;
    std::vector<std::uint64_t> to_vector() const;

    /// The largest id, or nullopt when empty.
    bool max_id(std::uint64_t& out) const;

private:
    bool compact_ = true;
    std::uint64_t start_ = 0;
    std::uint64_t count_ = 0;
    std::vector<std::uint64_t> values_;
};

/// The versions of a fragment's rows (`created_at` / `last_updated_at`), run-length encoded as Lance
/// stores them (RowDatasetVersionSequence): `runs` of `length` rows at `version`, in row order.
struct VersionRun {
    std::uint64_t length = 0;
    std::uint64_t version = 0;
};

class RowVersionSequence {
public:
    static RowVersionSequence uniform(std::uint64_t rows, std::uint64_t version);
    static bool decode(const std::uint8_t* data, std::size_t size, RowVersionSequence& out, std::string& error);
    std::vector<std::uint8_t> encode() const;

    std::uint64_t size() const;
    /// The version of row `index`, or 0 when out of range.
    std::uint64_t at(std::uint64_t index) const;
    std::vector<std::uint64_t> to_vector() const;
    static RowVersionSequence from_values(const std::vector<std::uint64_t>& values);

private:
    std::vector<VersionRun> runs_;
};

/// The pieces of a fragment message that this holds outside the generated codec (it keeps them in
/// `DataFragment.unknown`): field 5 (inline row ids), 7 (last updated at versions), 9 (created at
/// versions). External sequences (fields 6, 8, 10) are read when they sit in a file of the dataset
/// is not supported and report `false`.
struct FragmentRowMeta {
    bool has_row_ids = false;
    std::vector<std::uint8_t> row_ids;          // the serialized RowIdSequence
    bool has_last_updated = false;
    std::vector<std::uint8_t> last_updated;     // the serialized RowDatasetVersionSequence
    bool has_created = false;
    std::vector<std::uint8_t> created;
    bool external = false;                       // an external sequence is referenced (6, 8 or 10)
};

/// Read the row meta of `fragment` from its unknown fields.
bool read_fragment_row_meta(const pb::DataFragment& fragment, FragmentRowMeta& out, std::string& error);

/// Replace the row meta of `fragment` (fields 5, 7, 9; 6, 8, 10 removed) in its unknown bytes.
void write_fragment_row_meta(pb::DataFragment& fragment, const FragmentRowMeta& meta);

/// Drop the row meta (every one of fields 5 to 10) from `fragment`.
void clear_fragment_row_meta(pb::DataFragment& fragment);

/// A fragment's decoded row ids; an error when it carries none or an external file.
bool fragment_row_ids(const pb::DataFragment& fragment, RowIdSequence& out, std::string& error);

/// Where each row id of a dataset version lives. Lance's RowIdIndex.
struct RowLocation {
    std::uint64_t fragment_id = 0;
    std::uint64_t offset = 0;
};

class RowIdIndex {
public:
    /// Build the index of `manifest`'s fragments (their deletion files are read from `dataset_path`):
    /// the live rows only -- a deleted row's id stays in its fragment's sequence, and an updated
    /// row's id also sits in the fragment that now holds the row. Fragments without ids make it fail.
    static bool build(const std::filesystem::path& dataset_path, const pb::Manifest& manifest, RowIdIndex& out,
                      std::string& error);

    /// The address of `row_id` (fragment << 32 | offset), or false when the id is not in the dataset
    /// (never existed, or its row was deleted).
    bool find(std::uint64_t row_id, std::uint64_t& address) const;

    std::size_t size() const { return by_id_.size(); }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> by_id_;
    // Ranges are the usual shape; a sorted table of them avoids a hash entry per row.
    struct Run {
        std::uint64_t first_id;
        std::uint64_t count;
        std::uint64_t fragment_id;
        std::uint64_t first_offset;
    };
    std::vector<Run> runs_;
    bool sorted_ = false;
};

/// Row address -> row id, for what an index stores: Lance's indexes hold stable row ids on a dataset
/// that has them and row addresses on one that has not (then this is the identity).
class AddressToRowId {
public:
    /// Read the id sequences of `manifest`'s fragments (all of them, deleted rows included).
    static bool build(const pb::Manifest& manifest, AddressToRowId& out, std::string& error);

    bool stable() const { return stable_; }
    /// The id of the row at `address`; the address itself without stable ids.
    std::uint64_t operator()(std::uint64_t address) const;

private:
    bool stable_ = false;
    std::unordered_map<std::uint64_t, RowIdSequence> by_fragment_;
};

/// Row id -> row address, the other way: ids of an index's rows found in the dataset at `manifest`
/// (deletion files read from `dataset_path`). Ids whose rows no longer exist are not found.
class RowIdToAddress {
public:
    static bool build(const std::filesystem::path& dataset_path, const pb::Manifest& manifest, RowIdToAddress& out,
                      std::string& error);

    bool stable() const { return stable_; }
    /// False when the id is not a live row.
    bool find(std::uint64_t row_id, std::uint64_t& address) const;

private:
    bool stable_ = false;
    RowIdIndex index_;
};

/// The addresses (fragment << 32 | offset) of rows given by id, in order, at `version` (the latest
/// when null). Without stable row ids an id is its address and is only checked to exist. False and
/// `error` when an id is not in the dataset or its row has been deleted -- unless `skip_missing`, which
/// leaves such ids out (what pylance's take_rows does with a dataset's stable row ids).
bool resolve_row_ids(const std::filesystem::path& dataset_path, std::optional<std::uint64_t> version,
                     const std::vector<std::uint64_t>& ids, std::vector<std::uint64_t>& addresses,
                     std::string& error, bool skip_missing = false);

/// The version each row at `addresses` was created and last updated at (1 for a fragment without
/// version metadata, as Lance reads it), at `version` (the latest when null).
bool row_versions_at(const std::filesystem::path& dataset_path, std::optional<std::uint64_t> version,
                    const std::vector<std::uint64_t>& addresses, std::vector<std::uint64_t>& created,
                    std::vector<std::uint64_t>& last_updated, std::string& error);

}  // namespace nano_lance
