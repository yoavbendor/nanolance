// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/nano_lance_writer.h"

#include "nanolance/array_accessor.hpp"
#include "nanolance/blob_builder.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct WriterState {
    std::filesystem::path dataset_path;
    int compression_level = 0;
    std::uint64_t pending_batches = 0;
    std::uint64_t pending_rows = 0;
    nano_lance::LanceSchemaMapping schema_mapping;
    std::vector<nano_lance::ColumnValues> column_values;
    nano_lance::ColumnValues blob_column_values;
    const nano_lance::LanceField* blob_field = nullptr;
    bool ignore_nullability = false;
    bool blob_uri_dictionary = false;
    bool compression = false;
    bool has_schema = false;
    /// After the first successful manifest write, further commits must pass `is_append=true`.
    bool append_only_commits = false;
    bool closed = false;
};

void clear_error(NanoLanceWriter* writer) {
    if (writer != nullptr) {
        writer->last_error[0] = '\0';
    }
}

int set_error(NanoLanceWriter* writer, int code, const std::string& message) {
    if (writer != nullptr) {
        const auto bytes_to_copy = std::min(message.size(), sizeof(writer->last_error) - 1U);
        std::memcpy(writer->last_error, message.data(), bytes_to_copy);
        writer->last_error[bytes_to_copy] = '\0';
    }
    return code;
}

// Decide whether RLE beats bitpacking for a fixed-width column. Lance requires 8-bit run lengths, so
// runs longer than 255 are split into <=255 sub-runs; we count those split runs.
bool fixed_column_rle_plan(const nano_lance::ColumnValues& cv, std::size_t bpv) {
    if (bpv == 0U || cv.fixed.empty() || cv.fixed.size() % bpv != 0U) {
        return false;
    }
    const std::size_t n = cv.fixed.size() / bpv;
    std::size_t split_runs = 0;
    std::size_t i = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n &&
               std::memcmp(cv.fixed.data() + (i + run) * bpv, cv.fixed.data() + i * bpv, bpv) == 0) {
            ++run;
        }
        split_runs += (run + 254U) / 255U;  // each sub-run holds at most 255
        i += run;
    }
    // RLE pays off only with substantial repetition (Lance uses runs < 50% of values).
    if (split_runs * 2U >= n) {
        return false;
    }
    // One chunk for the whole column: run buffers must fit the miniblock (12-bit word => 32760 bytes).
    const std::size_t values_size = split_runs * bpv;
    const std::size_t lengths_size = split_runs;  // 1 byte each
    if ((values_size + lengths_size + 32U) > 32760U) {
        return false;
    }
    return true;
}

// True if every row of a variable-width column is identical; returns that value in `value_out`.
bool variable_column_constant_value(const nano_lance::ColumnValues& cv, std::vector<std::uint8_t>& value_out) {
    const std::size_t ow = cv.variable.large ? 8U : 4U;
    if (cv.variable.offsets.size() < 2U * ow) {
        return false;
    }
    auto read_offset = [&](std::size_t index) -> std::int64_t {
        const auto* p = cv.variable.offsets.data() + index * ow;
        if (cv.variable.large) {
            std::int64_t v = 0;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const std::size_t rows = cv.variable.offsets.size() / ow - 1U;
    if (rows == 0U) {
        return false;
    }
    const auto start0 = read_offset(0);
    const auto end0 = read_offset(1);
    if (start0 < 0 || end0 < start0 || static_cast<std::size_t>(end0) > cv.variable.data.size()) {
        return false;
    }
    const auto len = static_cast<std::size_t>(end0 - start0);
    for (std::size_t i = 1; i < rows; ++i) {
        const auto s = read_offset(i);
        const auto e = read_offset(i + 1);
        if (e - s != static_cast<std::int64_t>(len) ||
            std::memcmp(cv.variable.data.data() + s, cv.variable.data.data() + start0, len) != 0) {
            return false;
        }
    }
    value_out.assign(cv.variable.data.begin() + start0, cv.variable.data.begin() + end0);
    return true;
}

// Decide whether dictionary + RLE wins for a variable-width column. dict-RLE only helps when the
// per-row values form long runs (the per-minute URI case); since distinct values <= number of runs,
// "run-friendly" already implies "low cardinality", so this needs NO dictionary build — just a cheap
// consecutive-value run scan that bails the moment it stops being run-friendly. Zero allocation.
bool variable_column_dict_rle_beneficial(const nano_lance::ColumnValues& cv) {
    const std::size_t ow = cv.variable.large ? 8U : 4U;
    if (cv.variable.offsets.size() < 2U * ow) {
        return false;
    }
    auto read_offset = [&](std::size_t index) -> std::int64_t {
        const auto* p = cv.variable.offsets.data() + index * ow;
        if (cv.variable.large) {
            std::int64_t v = 0;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const std::size_t rows = cv.variable.offsets.size() / ow - 1U;
    if (rows == 0U) {
        return false;
    }
    const char* base = reinterpret_cast<const char*>(cv.variable.data.data());
    const std::size_t data_size = cv.variable.data.size();
    std::size_t split_runs = 0;
    std::size_t i = 0;
    while (i < rows) {
        const auto s0 = read_offset(i);
        const auto e0 = read_offset(i + 1);
        if (s0 < 0 || e0 < s0 || static_cast<std::size_t>(e0) > data_size) {
            return false;
        }
        const std::size_t len0 = static_cast<std::size_t>(e0 - s0);
        std::size_t run = 1;
        while (i + run < rows) {
            const auto sk = read_offset(i + run);
            const auto ek = read_offset(i + run + 1);
            if (sk < 0 || ek < sk || static_cast<std::size_t>(ek) > data_size) {
                return false;
            }
            if (static_cast<std::size_t>(ek - sk) != len0 || std::memcmp(base + sk, base + s0, len0) != 0) {
                break;
            }
            ++run;
        }
        split_runs += (run + 254U) / 255U;  // Lance caps run length at 255
        if (split_runs * 2U >= rows) {
            return false;  // not run-friendly (and therefore not low-cardinality)
        }
        i += run;
    }
    const std::size_t values_size = split_runs * 4U;  // u32 dictionary indices, one per split run
    return (values_size + split_runs + 32U) <= 32760U;
}

// Decide whether a structural dictionary (flat bitpacked indices + dictionary buffer) wins for a
// scattered low-cardinality string column. Mirrors Lance defaults: dict-divisor=2, dict-size-ratio=0.8,
// min 100 rows, max 100k distinct values.
bool variable_column_dict_beneficial(nano_lance::ColumnValues& cv) {
    constexpr std::size_t kMinRows = 100U;
    constexpr std::size_t kDictDivisor = 2U;
    constexpr double kDictSizeRatio = 0.8;
    constexpr std::size_t kMaxCardinality = 100000U;

    cv.structural_dict_plan = {};
    const std::size_t ow = cv.variable.large ? 8U : 4U;
    if (cv.variable.offsets.size() < 2U * ow) {
        return false;
    }
    auto read_offset = [&](std::size_t index) -> std::int64_t {
        const auto* p = cv.variable.offsets.data() + index * ow;
        if (cv.variable.large) {
            std::int64_t v = 0;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    };
    const std::size_t rows = cv.variable.offsets.size() / ow - 1U;
    if (rows < kMinRows) {
        return false;
    }
    const char* base = reinterpret_cast<const char*>(cv.variable.data.data());
    const std::size_t data_size = cv.variable.data.size();
    // Build the dictionary (distinct values + per-row indices) exactly as the data-file encoder would,
    // so on success the encoder can reuse this scan instead of repeating the dedup + index pass.
    std::unordered_map<std::string_view, std::uint32_t> dict;
    dict.reserve(rows / 4U);
    std::vector<std::string_view> distinct;
    std::vector<std::uint32_t> indices;
    indices.reserve(rows);
    std::size_t raw_bytes = 0;
    std::size_t dict_data = 0;
    for (std::size_t i = 0; i < rows; ++i) {
        const auto s = read_offset(i);
        const auto e = read_offset(i + 1);
        if (s < 0 || e < s || static_cast<std::size_t>(e) > data_size) {
            return false;
        }
        const auto len = static_cast<std::size_t>(e - s);
        raw_bytes += len;
        const std::string_view val(base + s, len);
        const auto id = static_cast<std::uint32_t>(distinct.size());
        const auto [it, inserted] = dict.emplace(val, id);
        if (inserted) {
            if (distinct.size() >= kMaxCardinality) {
                return false;
            }
            distinct.push_back(val);
            dict_data += len;
        }
        indices.push_back(it->second);
    }
    if (distinct.empty() || distinct.size() > rows / kDictDivisor) {
        return false;
    }
    const std::size_t dict_bytes = 8U + (distinct.size() + 1U) * 4U + dict_data;
    const std::size_t index_bytes = rows * 4U;
    const std::size_t raw_total = raw_bytes + (rows + 1U) * ow;
    const std::size_t encoded_total = dict_bytes + index_bytes;
    if (encoded_total >= static_cast<std::size_t>(static_cast<double>(raw_total) * kDictSizeRatio)) {
        return false;
    }
    cv.structural_dict_plan.computed = true;
    cv.structural_dict_plan.distinct = std::move(distinct);
    cv.structural_dict_plan.indices = std::move(indices);
    return true;
}

WriterState* state_from(NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return nullptr;
    }
    return static_cast<WriterState*>(writer->private_data);
}

const WriterState* state_from(const NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return nullptr;
    }
    return static_cast<const WriterState*>(writer->private_data);
}

std::size_t count_non_blob_physical_columns(const nano_lance::LanceSchemaMapping& mapping,
                                            std::int32_t blob_parent_id) {
    std::size_t count = 0;
    for (const auto& field : mapping.fields) {
        if (!nano_lance::lance_field_is_physical(field)) {
            continue;
        }
        if (blob_parent_id >= 0 && field.parent_id == blob_parent_id) {
            continue;
        }
        ++count;
    }
    return count;
}

/// Next numeric suffix for `data/fragment-<n>.lance` (0 if `data/` is missing or has no matching files).
std::uint64_t next_fragment_numeric_suffix(const std::filesystem::path& dataset_path) {
    const auto data_dir = dataset_path / "data";
    std::error_code ec;
    if (!std::filesystem::exists(data_dir, ec)) {
        return 0;
    }
    std::uint64_t max_seen = 0;
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        constexpr const char kPrefix[] = "fragment-";
        constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1U;
        if (name.size() <= kPrefixLen + 6U) {
            continue;
        }
        if (name.rfind(".lance") != name.size() - 6U) {
            continue;
        }
        if (name.compare(0, kPrefixLen, kPrefix) != 0) {
            continue;
        }
        const auto mid = name.substr(kPrefixLen, name.size() - 6U - kPrefixLen);
        try {
            const auto n = static_cast<std::uint64_t>(std::stoull(mid));
            max_seen = std::max(max_seen, n);
            found = true;
        } catch (...) {
        }
    }
    return found ? max_seen + 1U : 0U;
}

}  // namespace

extern "C" {

int nano_lance_writer_init(NanoLanceWriter* writer, const char* path, int compression_level) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    writer->private_data = nullptr;
    clear_error(writer);

    if (path == nullptr || path[0] == '\0') {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "dataset path must not be empty");
    }
    if (compression_level < 0 || compression_level > 22) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "zstd compression level must be in range 0..22");
    }

    auto state = std::make_unique<WriterState>();
    state->dataset_path = path;
    state->compression_level = compression_level;
    state->append_only_commits = false;
    writer->private_data = state.release();
    return NANO_LANCE_OK;
}

int nano_lance_writer_init_append(NanoLanceWriter* writer, const char* path, int compression_level) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    if (writer->private_data != nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "close writer before init_append");
    }
    writer->private_data = nullptr;
    clear_error(writer);

    if (path == nullptr || path[0] == '\0') {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "dataset path must not be empty");
    }
    if (compression_level < 0 || compression_level > 22) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "zstd compression level must be in range 0..22");
    }

    auto state = std::make_unique<WriterState>();
    state->dataset_path = path;
    state->compression_level = compression_level;
    state->append_only_commits = true;

    std::error_code ec;
    if (!std::filesystem::exists(state->dataset_path / "_versions", ec)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, "init_append requires an existing dataset with _versions");
    }

    nano_lance::pb::Manifest manifest{};
    std::uint64_t manifest_version = 0;
    std::string load_error;
    if (!nano_lance::load_latest_manifest(state->dataset_path, manifest, manifest_version, load_error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, load_error);
    }
    if (!nano_lance::lance_schema_mapping_from_manifest(manifest, state->schema_mapping, load_error)) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, load_error);
    }

    state->blob_field = nano_lance::find_blob_v2_parent(state->schema_mapping);
    const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
    state->column_values.resize(count_non_blob_physical_columns(state->schema_mapping, blob_parent_id));
    state->has_schema = true;

    writer->private_data = state.release();
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_ignore_nullability(NanoLanceWriter* writer, bool ignore_nullability) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "ignore-nullability must be set before writing batches");
    }
    state->ignore_nullability = ignore_nullability;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_blob_uri_dictionary(NanoLanceWriter* writer, bool enable) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "blob URI dictionary must be set before writing batches");
    }
    if (enable && state->append_only_commits) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, "blob URI dictionary mode is not supported for append datasets");
    }
    state->blob_uri_dictionary = enable;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_set_compression(NanoLanceWriter* writer, bool enable) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "compression must be set before writing batches");
    }
    state->compression = enable;
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_write_batch(NanoLanceWriter* writer, struct ArrowArray* batch, struct ArrowSchema* schema) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->closed) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is already closed");
    }
    if (batch == nullptr || schema == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "batch and schema must not be null");
    }
    if (batch->length < 0) {
        return set_error(writer, NANO_LANCE_INVALID_ARGUMENT, "batch length must not be negative");
    }

    nano_lance::LanceSchemaMapping batch_mapping;
    std::string error;
    if (!nano_lance::map_arrow_schema(*schema, batch_mapping, error, state->ignore_nullability)) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, error);
    }

    const auto* batch_blob_field = nano_lance::find_blob_v2_parent(batch_mapping);
    if (!state->has_schema) {
        state->schema_mapping = std::move(batch_mapping);
        state->blob_field = nano_lance::find_blob_v2_parent(state->schema_mapping);
        const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
        state->column_values.resize(count_non_blob_physical_columns(state->schema_mapping, blob_parent_id));
        state->has_schema = true;
    } else if (!nano_lance::schema_mappings_equal(state->schema_mapping, batch_mapping)) {
        // `state->schema_mapping` is the ingest-shape mapping captured on the first batch; compare the
        // new batch's ingest mapping directly. (Finalization to the packed blob layout happens at commit,
        // not here — finalizing only the candidate made every 2nd+ blob batch look like a schema change.)
        return set_error(writer, NANO_LANCE_UNSUPPORTED, "schema changes are not supported in this phase");
    }

    const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
    if (!nano_lance::append_batch_column_values(*batch,
                                                state->schema_mapping,
                                                state->column_values,
                                                error,
                                                blob_parent_id)) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, error);
    }
    if (state->blob_field != nullptr) {
        if (!nano_lance::append_blob_v2_batch_column_values(*batch,
                                                            state->schema_mapping,
                                                            *state->blob_field,
                                                            state->blob_uri_dictionary,
                                                            state->blob_column_values,
                                                            error)) {
            return set_error(writer, NANO_LANCE_UNSUPPORTED, error);
        }
    } else if (batch_blob_field != nullptr) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED, "schema gained lance.blob.v2 mid-write (not supported)");
    }

    ++state->pending_batches;
    state->pending_rows += static_cast<std::uint64_t>(batch->length);
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_commit(NanoLanceWriter* writer, bool is_append) {
    auto* state = state_from(writer);
    if (state == nullptr) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is not initialized");
    }
    if (state->closed) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "writer is already closed");
    }
    if (state->pending_rows == 0) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "no pending rows to commit");
    }
    if (state->append_only_commits && !is_append) {
        return set_error(writer, NANO_LANCE_INVALID_STATE,
                         "commit requires is_append=true after the first manifest was written");
    }
    if (state->blob_uri_dictionary && is_append) {
        return set_error(writer, NANO_LANCE_UNSUPPORTED,
                         "blob URI dictionary mode does not support append commits");
    }

    std::error_code error;
    if (is_append && !std::filesystem::exists(state->dataset_path, error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, "append requested but dataset path does not exist");
    }
    if (!state->has_schema) {
        return set_error(writer, NANO_LANCE_INVALID_STATE, "no complete Arrow IPC batches were written");
    }

    std::string writer_error;
    if (!is_append) {
        std::filesystem::create_directories(state->dataset_path / "_versions", error);
        if (error) {
            return set_error(writer, NANO_LANCE_IO_ERROR, "failed to create dataset version directory: " + error.message());
        }
        std::filesystem::create_directories(state->dataset_path / "data", error);
        if (error) {
            return set_error(writer, NANO_LANCE_IO_ERROR, "failed to create dataset data directory: " + error.message());
        }
    }

    nano_lance::LanceSchemaMapping disk_schema = state->schema_mapping;
    if (state->blob_field != nullptr) {
        if (!nano_lance::finalize_blob_v2_schema_for_write(disk_schema, writer_error)) {
            return set_error(writer, NANO_LANCE_UNSUPPORTED, writer_error);
        }
        // Persist the URI dictionary on the blob parent field so the reader can resolve each row's
        // URI by index. Stored in manifest/field metadata; only present in dictionary mode.
        if (state->blob_uri_dictionary && !state->blob_column_values.blob_v2.uri_dictionary.empty()) {
            const auto serialized =
                nano_lance::blob_v2_serialize_uri_dictionary(state->blob_column_values.blob_v2.uri_dictionary);
            for (auto& field : disk_schema.fields) {
                if (field.extension_name == nano_lance::kBlobV2ExtensionName && field.logical_type == "struct" &&
                    field.parent_id == -1) {
                    field.metadata[nano_lance::kBlobV2UriDictMetadataKey] = serialized;
                    break;
                }
            }
        }
    }

    // When compressing, tag each variable-width physical field so the reader knows to zstd-decompress.
    // (Stock Lance reads the encoding from the data-file PageLayout; this metadata is nanolance's own
    // read-side signal and is an inert write hint to Lance.)
    if (state->compression) {
        for (auto& field : disk_schema.fields) {
            if (!nano_lance::lance_field_is_physical(field) || !field.extension_name.empty()) {
                continue;
            }
            if (nano_lance::lance_field_is_variable_width(field.logical_type)) {
                field.metadata["lance-encoding:compression"] = "zstd";
            } else if (nano_lance::lance_logical_type_is_bitpackable_integer(field.logical_type)) {
                field.metadata["nanolance:packing"] = "bitpack";
            }
        }
    }

    std::vector<nano_lance::ColumnValues> commit_columns;
    if (state->blob_field != nullptr) {
        const auto physical = nano_lance::lance_physical_fields(disk_schema);
        commit_columns.reserve(physical.size());
        std::size_t non_blob_index = 0;
        for (const auto* field : physical) {
            if (field->extension_name == nano_lance::kBlobV2ExtensionName) {
                commit_columns.push_back(state->blob_column_values);
                continue;
            }
            if (non_blob_index >= state->column_values.size()) {
                return set_error(writer, NANO_LANCE_INVALID_STATE, "missing column values for physical field " + field->name);
            }
            commit_columns.push_back(state->column_values[non_blob_index++]);
        }
        if (commit_columns.size() != physical.size() || non_blob_index != state->column_values.size()) {
            return set_error(writer, NANO_LANCE_INVALID_STATE, "blob dataset physical column layout mismatch");
        }
    } else {
        commit_columns = std::move(state->column_values);
    }

    // Constant fixed-width columns -> ConstantLayout (value inline in the page descriptor, zero data
    // bytes). Overrides the bitpack tag for those columns. Tags disk_schema so both the manifest and
    // the data-file descriptor carry packing=constant + the raw value bytes for the reader.
    if (state->compression) {
        const auto physical = nano_lance::lance_physical_fields(disk_schema);
        for (std::size_t i = 0; i < physical.size() && i < commit_columns.size(); ++i) {
            const auto* pf = physical[i];
            if (!pf->extension_name.empty()) {
                continue;
            }
            auto& cv = commit_columns[i];
            std::vector<std::uint8_t> value;
            bool constant = false;
            if (cv.kind == nano_lance::ColumnValues::Kind::FixedWidth) {
                const auto bpv = nano_lance::lance_logical_type_value_bytes(pf->logical_type);
                if (bpv != 0U && cv.fixed.size() >= bpv && cv.fixed.size() % bpv == 0U) {
                    constant = true;
                    for (std::size_t off = bpv; off + bpv <= cv.fixed.size(); off += bpv) {
                        if (std::memcmp(cv.fixed.data(), cv.fixed.data() + off, bpv) != 0) {
                            constant = false;
                            break;
                        }
                    }
                    if (constant) {
                        value.assign(cv.fixed.begin(), cv.fixed.begin() + static_cast<std::ptrdiff_t>(bpv));
                    }
                }
            } else if (cv.kind == nano_lance::ColumnValues::Kind::VariableWidth) {
                constant = variable_column_constant_value(cv, value);
            }
            if (constant) {
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "constant";
                        field.metadata["nanolance:const-value"] = std::string(value.begin(), value.end());
                        break;
                    }
                }
                continue;
            }
            // Run-length encoding for repetitive fixed-width integer columns (beats bitpacking when
            // there are long runs, e.g. dictionary indices later).
            if (cv.kind == nano_lance::ColumnValues::Kind::FixedWidth &&
                nano_lance::lance_logical_type_is_bitpackable_integer(pf->logical_type)) {
                const auto bpv = nano_lance::lance_logical_type_value_bytes(pf->logical_type);
                if (fixed_column_rle_plan(cv, bpv)) {
                    for (auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            field.metadata["nanolance:packing"] = "rle";
                            break;
                        }
                    }
                }
            } else if (cv.kind == nano_lance::ColumnValues::Kind::VariableWidth &&
                       variable_column_dict_rle_beneficial(cv)) {
                // Low-cardinality run-length string column -> dictionary + RLE'd indices.
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "dict-rle";
                        field.metadata.erase("lance-encoding:compression");
                        break;
                    }
                }
            } else if (cv.kind == nano_lance::ColumnValues::Kind::VariableWidth &&
                       variable_column_dict_beneficial(cv)) {
                // Scattered low-cardinality strings -> structural dictionary + flat indices.
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "dict";
                        field.metadata.erase("lance-encoding:compression");
                        break;
                    }
                }
            }
        }
    }

    nano_lance::DataFileResult data_file;
    const auto data_file_name =
        "fragment-" + std::to_string(next_fragment_numeric_suffix(state->dataset_path)) + ".lance";
    if (!nano_lance::write_lance_data_file(state->dataset_path,
                                           data_file_name,
                                           disk_schema,
                                           commit_columns,
                                           state->pending_rows,
                                           state->compression_level,
                                           state->compression,
                                           data_file,
                                           writer_error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, writer_error);
    }

    std::uint64_t version = 0;
    if (!nano_lance::write_dataset_manifest(state->dataset_path,
                                            disk_schema,
                                            data_file,
                                            state->pending_rows,
                                            is_append,
                                            version,
                                            writer_error)) {
        return set_error(writer, NANO_LANCE_IO_ERROR, writer_error);
    }

    state->append_only_commits = true;
    state->pending_batches = 0;
    state->pending_rows = 0;
    state->column_values.clear();
    state->blob_column_values = nano_lance::ColumnValues{};
    state->blob_field = nano_lance::find_blob_v2_parent(state->schema_mapping);
    {
        const std::int32_t blob_parent_id = state->blob_field != nullptr ? state->blob_field->id : -1;
        state->column_values.resize(count_non_blob_physical_columns(state->schema_mapping, blob_parent_id));
    }
    clear_error(writer);
    return NANO_LANCE_OK;
}

int nano_lance_writer_close(NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return NANO_LANCE_INVALID_ARGUMENT;
    }
    auto* state = state_from(writer);
    if (state == nullptr) {
        clear_error(writer);
        return NANO_LANCE_OK;
    }
    state->closed = true;
    delete state;
    writer->private_data = nullptr;
    clear_error(writer);
    return NANO_LANCE_OK;
}

const char* nano_lance_writer_last_error(const NanoLanceWriter* writer) {
    if (writer == nullptr) {
        return "writer is null";
    }
    return writer->last_error;
}

uint64_t nano_lance_writer_pending_batches(const NanoLanceWriter* writer) {
    const auto* state = state_from(writer);
    if (state == nullptr) {
        return 0;
    }
    return state->pending_batches;
}

}  // extern "C"
