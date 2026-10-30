// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "dataset/write_session.hpp"

#include "nanolance/byte_sink.hpp"

#include "nanolance/array_accessor.hpp"
#include "nanolance/blob_builder.hpp"
#include "nanolance/blob_v2_external.hpp"
#include "nanolance/data_file_writer.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/manifest_writer.hpp"
#include "nanolance/schema_mapper.hpp"
#include "encoding/encoding_choice.hpp"
#include "arrow/arrow_views.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cstdlib>
#include <system_error>
#include <cstring>
#include <functional>
#include <filesystem>
#include <optional>
#include <memory>
#include <random>
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nano_lance {


struct WriteSession::State {
    std::filesystem::path dataset_path;
    int compression_level = 0;
    std::uint64_t pending_batches = 0;
    std::uint64_t pending_rows = 0;
    nano_lance::LanceSchemaMapping schema_mapping;
    std::vector<nano_lance::ColumnValues> column_values;
    /// The top-level lance.blob.v2 columns (their field ids, in schema order) and their rows.
    std::vector<std::int32_t> blob_ids;
    std::vector<nano_lance::ColumnValues> blob_column_values;
    /// write_dataset's blob_pack_file_size_threshold: over each blob field's own (0: the field's).
    std::uint64_t blob_pack_file_size = 0;
    bool ignore_nullability = false;
    bool blob_uri_dictionary = false;
    bool compression = false;
    // Structural (lossless) re-encodings — bitpacking, ConstantLayout, RLE, dictionary, dict-RLE — are
    // independent of zstd byte compression and on by default: they shrink files and usually speed up
    // writes, and stay stock-Lance-readable. `compression` (set_compression) now controls ONLY zstd.
    bool structural = true;
    /// Per-field encoding declarations (set_column_encoding): "plain"/"bitpack"/"bss-zstd"/"zstd".
    /// A declared column skips the commit-time detection scans entirely. Absent == "auto".
    std::map<std::string, std::string> column_encodings;
    /// Borrow the caller's fixed-width Arrow buffers until commit instead of copying (zero-copy
    /// ingest for single-batch columns; multi-batch columns silently fall back to copying).
    bool borrow_buffers = false;
    bool has_schema = false;
    /// After the first successful manifest write, further commits must pass `is_append=true`.
    bool append_only_commits = false;
    bool closed = false;
    /// Memory budget for rows buffered between commits (set_max_pending_bytes); 0 = unlimited. When
    /// a write_batch takes the buffered data to this size, the writer commits a fragment itself.
    std::uint64_t max_pending_bytes = 0;
    /// write_batch has committed at least once on its own: the caller's final commit is then an
    /// append whatever it says, and a no-op when nothing is left pending.
    bool flushed_by_budget = false;
    /// Staged mode (NanoLanceWriteOptions::stage_fragments): a commit writes its data file and stages
    /// the fragment; nano_lance_writer_finish publishes them all as one version.
    bool stage = false;
    /// A progress tracker's data-file events (writer_set_file_events): begin (0) when a file's first rows
    /// are buffered, its name chosen then; complete (2) once the file is written.
    std::function<void(int, const std::string&, std::uint64_t)> file_events;
    std::string open_file_name;
    std::vector<nano_lance::NewFragment> staged;
    nano_lance::LanceSchemaMapping staged_schema;  // the on-disk schema the staged files were written with
    /// The Arrow schema metadata of the first batch, recorded in the manifest.
    std::map<std::string, std::vector<std::uint8_t>> schema_metadata;
    /// Global buffers of the data file, after the schema's (nano_lance_writer_add_global_buffer).
    std::vector<std::vector<std::uint8_t>> global_buffers;
    /// Table config recorded when the commit creates the dataset (nano_lance_writer_set_initial_config).
    std::map<std::string, std::string> initial_config;
    /// Create the dataset with stable row ids (nano_lance_writer_set_stable_row_ids).
    bool stable_row_ids = false;
    /// Base paths a dataset created by this writer registers (nano_lance_writer_set_initial_bases).
    std::vector<nano_lance::BasePath> initial_bases;
    /// Where each new fragment goes, in turn (nano_lance_writer_set_fragment_targets); none: the data dir.
    struct FragmentTarget {
        std::filesystem::path dir;
        std::uint32_t base_id = 0;  // 0: the dataset itself
    };
    std::vector<FragmentTarget> fragment_targets;
    std::uint64_t fragments_written = 0;
    /// Recorded in the finishing commit's transaction file (nano_lance_writer_set_transaction_property).
    std::map<std::string, std::string> transaction_properties;
    /// Field ids of the schema taken from the first batch start here (writer_set_field_id_base): new
    /// columns for an existing dataset, numbered after its fields.
    std::int32_t field_id_base = 0;
};

namespace {

/// Reads the `lance-encoding:` knobs that move nanolance's structural choices, named as pylance names them:
/// rle-threshold, dict-divisor and dict-size-ratio. A value that does not parse keeps the default, as pylance
/// does. A dict-size-ratio outside (0, 1] is refused by name here (pylance panics on it). Returns false with
/// `error` set on a refusal.
bool read_encoding_options(const LanceField& field, encoding_choice::ColumnEncodingOptions& out, std::string& error) {
    if (const auto it = field.metadata.find("lance-encoding:rle-threshold"); it != field.metadata.end()) {
        char* end = nullptr;
        const double value = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str() && *end == '\0' && std::isfinite(value) && value >= 0.0) {
            out.rle_set = true;
            out.rle_threshold = value;
        }
    }
    if (const auto it = field.metadata.find("lance-encoding:dict-divisor"); it != field.metadata.end()) {
        std::size_t value = 0;
        const auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
        if (ec == std::errc{} && end == it->second.data() + it->second.size()) {
            out.dict_divisor = value;
        }
    }
    if (const auto it = field.metadata.find("lance-encoding:dict-size-ratio"); it != field.metadata.end()) {
        char* end = nullptr;
        const double value = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str() && *end == '\0' && !std::isnan(value)) {
            if (!(value > 0.0 && value <= 1.0)) {
                error = "lance-encoding:dict-size-ratio " + it->second + " on column '" + field.name +
                        "' is not in (0, 1]";
                return false;
            }
            out.dict_size_ratio = value;
        }
    }
    if (const auto it = field.metadata.find("lance-encoding:dict-values-compression"); it != field.metadata.end()) {
        out.dict_values_compression = it->second;
    }
    return true;
}

/// Bytes the writer is holding for rows not yet committed: the capacity of every buffer it owns,
/// plus borrowed caller buffers, which stay pinned until the commit.
std::uint64_t held_bytes(const WriteSession::State& state) {
    const auto column_bytes = [](const nano_lance::ColumnValues& cv) {
        std::uint64_t n = cv.fixed.capacity() + cv.fixed_borrowed_size + cv.variable.offsets.capacity() +
                          cv.variable.data.capacity() + cv.validity.capacity() + cv.item_validity.capacity() +
                          cv.blob_v2.packed_payload.capacity() +
                          cv.blob_v2.row_packed_sizes.capacity() * sizeof(std::uint32_t);
        for (const auto& layer : cv.layers) {
            n += layer.offsets.capacity() * sizeof(std::int64_t) + layer.validity.capacity();
        }
        for (const auto& uri : cv.blob_v2.uri_dictionary) {
            n += uri.capacity();
        }
        return n;
    };
    std::uint64_t total = 0;
    for (const auto& cv : state.blob_column_values) {
        total += column_bytes(cv);
    }
    for (const auto& cv : state.column_values) {
        total += column_bytes(cv);
    }
    return total;
}

std::size_t count_non_blob_physical_columns(const nano_lance::LanceSchemaMapping& mapping) {
    std::size_t count = 0;
    for (const auto& field : mapping.fields) {
        if (nano_lance::lance_field_is_physical(field) && !nano_lance::lance_field_is_blob_v2_part(mapping, field)) {
            ++count;
        }
    }
    return count;
}

/// Empty row buffers for the writer's schema: one per column, one per blob column.
void reset_column_values(WriteSession::State& state) {
    state.blob_ids = nano_lance::blob_v2_parent_ids(state.schema_mapping);
    state.blob_column_values.assign(state.blob_ids.size(), nano_lance::ColumnValues{});
    state.column_values.clear();
    state.column_values.resize(count_non_blob_physical_columns(state.schema_mapping));
}

/// A new data file's name, as Lance names its files (generate_random_filename): a random UUID v4, its
/// first 3 bytes as 24 binary digits, then the other 13 as 26 hex digits -- 50 characters, ".lance".
/// Random because writers in other processes add files to the same `data/` at the same time: a name
/// derived from what is already there (a counter) could be taken twice, and the second file would
/// overwrite one the first writer has committed.
/// The bits come from the system's entropy source on every call: a seeded generator would be
/// copied into each process a writer forks, and those would then all draw the same names.
std::string new_data_file_name() {
    std::random_device device;
    std::array<std::uint8_t, 16> uuid{};
    for (std::size_t i = 0; i < uuid.size(); i += 4) {
        const auto bits = static_cast<std::uint32_t>(device());
        for (std::size_t k = 0; k < 4; ++k) {
            uuid[i + k] = static_cast<std::uint8_t>(bits >> (8U * k));
        }
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0FU) | 0x40U);  // version 4
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    static constexpr char kHex[] = "0123456789abcdef";
    std::string name;
    name.reserve(56);
    for (std::size_t i = 0; i < 3; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            name += ((uuid[i] >> bit) & 1U) != 0U ? '1' : '0';
        }
    }
    for (std::size_t i = 3; i < uuid.size(); ++i) {
        name += kHex[uuid[i] >> 4U];
        name += kHex[uuid[i] & 0xFU];
    }
    return name + ".lance";
}

}  // namespace

WriteSession::WriteSession() = default;
WriteSession::~WriteSession() = default;

WriteStatus WriteSession::open(const std::string& path, const WriteOptions& opts) {
    error_.clear();

    if (path.empty()) {
        return fail(WriteStatus::InvalidArgument, "dataset path must not be empty");
    }
    // Local paths and registered object stores (s3://); any other URI would become a local directory
    // named after its scheme.
    if (path.find("://") != std::string::npos && !io::is_object_store_output(path)) {
        return fail(WriteStatus::Unsupported,
                         std::string("writes to this kind of URI are not supported (local paths and s3:// only): ") +
                             path);
    }
    if (opts.compression_level < 0 || opts.compression_level > 22) {
        return fail(WriteStatus::InvalidArgument, "zstd compression level must be in range 0..22");
    }
    if (opts.append && opts.blob_uri_dictionary) {
        // Same refusal, same code, as set_blob_uri_dictionary on an append writer.
        return fail(WriteStatus::Unsupported,
                         "blob URI dictionary mode is not supported for append datasets");
    }
    if (opts.max_pending_bytes != 0U && opts.blob_uri_dictionary) {
        // Every flush after the first is an append, which dictionary mode cannot do.
        return fail(WriteStatus::Unsupported,
                         "blob URI dictionary mode cannot be combined with max_pending_bytes");
    }

    auto state = std::make_unique<State>();
    state->dataset_path = path;
    state->compression_level = opts.compression_level;
    state->compression = opts.compression;
    state->structural = !opts.disable_structural_encoding;
    state->blob_uri_dictionary = opts.blob_uri_dictionary;
    state->borrow_buffers = opts.borrow_buffers;
    state->append_only_commits = opts.append;
    state->max_pending_bytes = opts.max_pending_bytes;
    state->stage = opts.stage_fragments;

    for (const auto& [field_name, encoding] : opts.column_encodings) {
        if (field_name.empty()) {
            return fail(WriteStatus::InvalidArgument, "field name must not be empty");
        }
        const std::string& enc = encoding;
        if (enc == "auto") {
            state->column_encodings.erase(field_name);
        } else if (enc == "plain" || enc == "bitpack" || enc == "bss-zstd" || enc == "zstd") {
            state->column_encodings[field_name] = enc;
        } else {
            return fail(WriteStatus::InvalidArgument,
                             "unknown column encoding (expected auto/plain/bitpack/bss-zstd/zstd): " + enc);
        }
    }

    if (opts.append) {
        std::error_code ec;
        // An object store has no directories: loading the latest manifest below checks it there.
        if (!io::is_object_store_output(state->dataset_path.string()) &&
            !std::filesystem::exists(state->dataset_path / "_versions", ec)) {
            return fail(WriteStatus::IoError, "init_append requires an existing dataset with _versions");
        }

        pb::Manifest manifest{};
        std::uint64_t manifest_version = 0;
        std::string load_error;
        if (!load_latest_manifest(state->dataset_path, manifest, manifest_version, load_error)) {
            return fail(WriteStatus::IoError, load_error);
        }
        // Refused here, before any data file is written (pylance leaves no file behind either).
        if (!check_manifest_writable(manifest, WriteKind::Insert, load_error)) {
            return fail(WriteStatus::Unsupported, load_error);
        }
        if (!lance_schema_mapping_from_manifest(manifest, state->schema_mapping, load_error)) {
            return fail(WriteStatus::Unsupported, load_error);
        }
        // The new fragment is one file with every column, whatever files the latest fragment has.
        renumber_columns_for_one_file(state->schema_mapping);

        reset_column_values(*state);
        state->has_schema = true;
    }

    state_ = std::move(state);
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_ignore_nullability(bool ignore_nullability) {
    auto* state = state_.get();
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return fail(WriteStatus::InvalidState, "ignore-nullability must be set before writing batches");
    }
    state->ignore_nullability = ignore_nullability;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_blob_uri_dictionary(bool enable) {
    auto* state = state_.get();
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return fail(WriteStatus::InvalidState, "blob URI dictionary must be set before writing batches");
    }
    if (enable && state->append_only_commits) {
        return fail(WriteStatus::Unsupported, "blob URI dictionary mode is not supported for append datasets");
    }
    if (enable && state->max_pending_bytes != 0U) {
        return fail(WriteStatus::Unsupported,
                         "blob URI dictionary mode cannot be combined with max_pending_bytes");
    }
    state->blob_uri_dictionary = enable;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_compression(bool enable) {
    auto* state = state_.get();
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return fail(WriteStatus::InvalidState, "compression must be set before writing batches");
    }
    state->compression = enable;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_structural_encoding(bool enable) {
    auto* state = state_.get();
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return fail(WriteStatus::InvalidState,
                         "structural encoding must be set before writing batches");
    }
    state->structural = enable;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_column_encoding(const char* field_name,
                                          const char* encoding) {
    auto* state = state_.get();
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return fail(WriteStatus::InvalidState,
                         "column encodings must be set before writing batches");
    }
    if (field_name == nullptr || field_name[0] == '\0') {
        return fail(WriteStatus::InvalidArgument, "field name must not be empty");
    }
    const std::string enc = encoding != nullptr ? encoding : "";
    if (enc == "auto") {
        state->column_encodings.erase(field_name);
    } else if (enc == "plain" || enc == "bitpack" || enc == "bss-zstd" || enc == "zstd") {
        state->column_encodings[field_name] = enc;
    } else {
        return fail(WriteStatus::InvalidArgument,
                         "unknown column encoding (expected auto/plain/bitpack/bss-zstd/zstd): " + enc);
    }
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_max_pending_bytes(uint64_t max_pending_bytes) {
    auto* state = state_.get();
    if (max_pending_bytes != 0U && state->blob_uri_dictionary) {
        return fail(WriteStatus::Unsupported,
                         "blob URI dictionary mode cannot be combined with max_pending_bytes");
    }
    state->max_pending_bytes = max_pending_bytes;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_borrow_buffers(bool enable) {
    auto* state = state_.get();
    if (state->pending_batches != 0 || state->pending_rows != 0) {
        return fail(WriteStatus::InvalidState,
                         "borrow_buffers must be set before writing batches");
    }
    state->borrow_buffers = enable;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::write_batch(struct ArrowArray* batch, struct ArrowSchema* schema) {
    auto* state = state_.get();
    if (state->closed) {
        return fail(WriteStatus::InvalidState, "writer is already closed");
    }
    if (batch == nullptr || schema == nullptr) {
        return fail(WriteStatus::InvalidArgument, "batch and schema must not be null");
    }
    if (batch->length < 0) {
        return fail(WriteStatus::InvalidArgument, "batch length must not be negative");
    }

    // Utf8View and BinaryView columns are written as string and binary, as pylance writes them. A batch without a
    // view column is not touched.
    std::string error;
    arrow_views::ViewlessBatch viewless;
    if (arrow_views::has_view_types(*schema)) {
        if (!viewless.convert(*schema, *batch, error)) {
            return fail(WriteStatus::Unsupported, error);
        }
        batch = viewless.array();
        schema = viewless.schema();
    }

    LanceSchemaMapping batch_mapping;
    if (!map_arrow_schema(*schema, batch_mapping, error, state->ignore_nullability)) {
        return fail(WriteStatus::Unsupported, error);
    }

    if (!state->has_schema) {
        ArrowMetadataReader reader;
        if (schema->metadata != nullptr && ArrowMetadataReaderInit(&reader, schema->metadata) == NANOARROW_OK) {
            ArrowStringView key;
            ArrowStringView value;
            while (reader.remaining_keys > 0 && ArrowMetadataReaderRead(&reader, &key, &value) == NANOARROW_OK) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data);
                state->schema_metadata[std::string(key.data, static_cast<std::size_t>(key.size_bytes))] =
                    std::vector<std::uint8_t>(bytes, bytes + value.size_bytes);
            }
        }
    }
    if (!state->has_schema) {
        state->schema_mapping = std::move(batch_mapping);
        if (state->field_id_base != 0) {
            for (auto& f : state->schema_mapping.fields) {
                f.id += state->field_id_base;
                if (f.parent_id >= 0) {
                    f.parent_id += state->field_id_base;
                }
            }
        }
        reset_column_values(*state);
        state->has_schema = true;
    } else if (!schema_mappings_equivalent(state->schema_mapping, batch_mapping)) {
        // `state->schema_mapping` is the ingest-shape mapping captured on the first batch; compare the
        // new batch's ingest mapping directly. (Finalization to the packed blob layout happens at commit,
        // not here — finalizing only the candidate made every 2nd+ blob batch look like a schema change.)
        return fail(WriteStatus::Unsupported,
                         "all batches in a writer session must share one schema (locked by the first "
                         "batch); " +
                             describe_schema_mapping_mismatch(state->schema_mapping,
                                                                          batch_mapping));
    }

    if (!append_batch_column_values(*batch,
                                                state->schema_mapping,
                                                state->column_values,
                                                error,
                                                !state->blob_ids.empty(),
                                                state->borrow_buffers)) {
        return fail(WriteStatus::Unsupported, error);
    }
    for (std::size_t b = 0; b < state->blob_ids.size(); ++b) {
        const LanceField* blob_field = nullptr;
        for (const auto& f : state->schema_mapping.fields) {
            blob_field = f.id == state->blob_ids[b] ? &f : blob_field;
        }
        if (blob_field == nullptr ||
            !append_blob_v2_batch_column_values(*batch,
                                                            state->schema_mapping,
                                                            *blob_field,
                                                            state->blob_uri_dictionary,
                                                            state->blob_column_values[b],
                                                            error)) {
            return fail(WriteStatus::Unsupported, error.empty() ? "blob column not found" : error);
        }
    }

    if (state->file_events && state->pending_rows == 0 && batch->length > 0) {
        state->open_file_name = new_data_file_name();
        state->file_events(0, state->open_file_name, 0);
    }
    ++state->pending_batches;
    state->pending_rows += static_cast<std::uint64_t>(batch->length);
    if (state->max_pending_bytes != 0U && state->pending_rows != 0U &&
        held_bytes(*state) >= state->max_pending_bytes) {
        // Over budget: write what is buffered as a fragment now and free it. The first flush of a
        // create-mode writer creates the dataset; every later one appends.
        const auto rc = commit_pending(state->append_only_commits);
        if (rc != WriteStatus::Ok) {
            return rc;
        }
        state->flushed_by_budget = true;
    }
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::commit(bool is_append) {
    auto* state = state_.get();
    if (state->closed) {
        return fail(WriteStatus::InvalidState, "writer is already closed");
    }
    if (state->flushed_by_budget) {
        // write_batch already committed on its own (max_pending_bytes): the dataset exists, so this
        // commit appends -- the caller wrote the same code it would without a budget -- and with
        // nothing left pending there is nothing to do.
        if (state->pending_rows == 0) {
            error_.clear();
            return WriteStatus::Ok;
        }
        is_append = true;
    }
    return commit_pending(is_append);
}

WriteStatus WriteSession::commit_pending(bool is_append) {
    auto* state = state_.get();
    // A staged writer may commit a batch of no rows on purpose: a Lance file of a schema alone.
    if (state->pending_rows == 0 && !(state->stage && state->pending_batches != 0U)) {
        return fail(WriteStatus::InvalidState, "no pending rows to commit");
    }
    if (state->append_only_commits && !is_append && !state->stage) {
        return fail(WriteStatus::InvalidState,
                         "commit requires is_append=true after the first manifest was written");
    }
    if (state->blob_uri_dictionary && is_append) {
        return fail(WriteStatus::Unsupported,
                         "blob URI dictionary mode does not support append commits");
    }

    std::error_code error;
    const bool object_store = io::is_object_store_output(state->dataset_path.string());
    if (is_append && !object_store && !std::filesystem::exists(state->dataset_path, error)) {
        return fail(WriteStatus::IoError, "append requested but dataset path does not exist");
    }
    if (!state->has_schema) {
        return fail(WriteStatus::InvalidState, "no complete Arrow IPC batches were written");
    }

    std::string writer_error;
    if (!is_append && !object_store) {
        std::filesystem::create_directories(state->dataset_path / "_versions", error);
        if (error) {
            return fail(WriteStatus::IoError, "failed to create dataset version directory: " + error.message());
        }
        std::filesystem::create_directories(state->dataset_path / "data", error);
        if (error) {
            return fail(WriteStatus::IoError, "failed to create dataset data directory: " + error.message());
        }
    }

    LanceSchemaMapping disk_schema = state->schema_mapping;
    if (!state->blob_ids.empty()) {
        if (!finalize_blob_v2_schema_for_write(disk_schema, writer_error)) {
            return fail(WriteStatus::Unsupported, writer_error);
        }
        // Persist each URI dictionary on its blob field so the reader can resolve each row's URI by
        // index. Stored in manifest/field metadata; only present in dictionary mode.
        for (std::size_t b = 0; state->blob_uri_dictionary && b < state->blob_ids.size(); ++b) {
            const auto& dictionary = state->blob_column_values[b].blob_v2.uri_dictionary;
            if (dictionary.empty()) {
                continue;
            }
            for (auto& field : disk_schema.fields) {
                if (field.id == state->blob_ids[b]) {
                    field.metadata[kBlobV2UriDictMetadataKey] =
                        blob_v2_serialize_uri_dictionary(dictionary);
                }
            }
        }
    }

    // Tag fields for encoding. zstd (a real CPU-for-size tradeoff) is opt-in via set_compression and
    // applies to variable-width columns and, for float/double, as byte-stream-split + zstd (splitting
    // each value into its mantissa/exponent byte planes before compressing — the same technique stock
    // Lance uses for these types, since compressing already-interleaved float bytes barely helps).
    // Integer bitpacking is a structural, lossless re-encoding and is enabled by the (default-on)
    // structural switch. (Stock Lance reads the encoding from the data-file PageLayout; this metadata
    // is nanolance's own read-side signal and an inert write hint to Lance.)
    for (auto& field : disk_schema.fields) {
        if (!lance_field_is_physical(field) ||
            lance_extension_is_lance_owned(field.extension_name)) {
            continue;
        }
        // Declared encodings (set_column_encoding) override the automatic tagging below and later skip
        // the detection scans entirely -- the caller vouched for the column's shape at compile/config
        // time, so nanolance doesn't re-derive it from a full data scan. Type compatibility is checked
        // here so a bad declaration fails the commit loudly instead of writing a broken file.
        const auto declared_it = state->column_encodings.find(field.name);
        if (declared_it != state->column_encodings.end()) {
            const auto& declared = declared_it->second;
            const bool variable = lance_field_is_variable_width(field.logical_type);
            if (declared == "plain") {
                continue;  // no tags: flat/variable pages, no structural encoding, no zstd
            }
            if (declared == "bitpack") {
                if (!lance_logical_type_is_bitpackable_integer(field.logical_type)) {
                    return fail(WriteStatus::InvalidArgument,
                                     "column encoding 'bitpack' requires an integer column: " + field.name);
                }
                field.metadata["nanolance:packing"] = "bitpack";
                continue;
            }
            if (declared == "bss-zstd") {
                if (field.logical_type != "float" && field.logical_type != "double") {
                    return fail(WriteStatus::InvalidArgument,
                                     "column encoding 'bss-zstd' requires a float/double column: " + field.name);
                }
                field.metadata["nanolance:packing"] = "bss-zstd";
                continue;
            }
            // declared == "zstd"
            if (!variable) {
                return fail(WriteStatus::InvalidArgument,
                                 "column encoding 'zstd' requires a string/binary column: " + field.name);
            }
            if (state->compression) {
                field.metadata["lance-encoding:compression"] = "zstd";
            }
            continue;
        }
        if (lance_field_is_variable_width(field.logical_type)) {
            if (state->compression) {
                field.metadata["lance-encoding:compression"] = "zstd";
            }
        } else if (state->structural && lance_logical_type_is_bitpackable_integer(field.logical_type)) {
            field.metadata["nanolance:packing"] = "bitpack";
        } else if (state->compression && (field.logical_type == "float" || field.logical_type == "double")) {
            field.metadata["nanolance:packing"] = "bss-zstd";
        }
    }

    std::vector<ColumnValues> commit_columns;
    if (!state->blob_ids.empty()) {
        const auto physical = lance_physical_fields(disk_schema);
        commit_columns.reserve(physical.size());
        std::size_t non_blob_index = 0;
        for (const auto* field : physical) {
            const auto blob = std::find(state->blob_ids.begin(), state->blob_ids.end(), field->id);
            if (blob != state->blob_ids.end()) {
                commit_columns.push_back(
                    std::move(state->blob_column_values[static_cast<std::size_t>(blob - state->blob_ids.begin())]));
                continue;
            }
            if (non_blob_index >= state->column_values.size()) {
                return fail(WriteStatus::InvalidState, "missing column values for physical field " + field->name);
            }
            commit_columns.push_back(state->column_values[non_blob_index++]);
        }
        if (commit_columns.size() != physical.size() || non_blob_index != state->column_values.size()) {
            return fail(WriteStatus::InvalidState, "blob dataset physical column layout mismatch");
        }
    } else {
        commit_columns = std::move(state->column_values);
    }

    // Constant fixed-width columns -> ConstantLayout (value inline in the page descriptor, zero data
    // bytes). Overrides the bitpack tag for those columns. Tags disk_schema so both the manifest and
    // the data-file descriptor carry packing=constant + the raw value bytes for the reader. These
    // (constant / RLE / dictionary / dict-RLE) are structural encodings, independent of zstd.
    if (state->structural) {
        const auto physical = lance_physical_fields(disk_schema);
        for (std::size_t i = 0; i < physical.size() && i < commit_columns.size(); ++i) {
            const auto* pf = physical[i];
            encoding_choice::ColumnEncodingOptions enc_options;
            if (std::string option_error; !read_encoding_options(*pf, enc_options, option_error)) {
                return fail(WriteStatus::InvalidArgument, option_error);
            }
            if (lance_extension_is_lance_owned(pf->extension_name)) {
                continue;
            }
            // A fixed_size_list stays flat. Every structural encoding here would describe a row as one
            // wide scalar -- Constant, RLE, a dictionary -- with no FixedSizeList wrapper, and Lance
            // reads the list type from exactly that wrapper. (A constant one is doubly out: Lance's
            // inline constant value is for types with no child data.)
            {
                std::string element;
                std::uint64_t items = 0;
                if (lance_fixed_size_list_parts(pf->logical_type, element, items)) {
                    continue;
                }
            }
            // A leaf under a list writes its own pages (repetition levels, items): no structural
            // encoding applies to it, and its values count items, not rows.
            if (i < commit_columns.size() && commit_columns[i].needs_nested_pages()) {
                continue;
            }
            // Declared columns (set_column_encoding) skip ALL detection scans -- the encoding was
            // decided by the caller; these scans are exactly the work the declaration saves.
            if (state->column_encodings.find(pf->name) != state->column_encodings.end()) {
                continue;
            }
            auto& cv = commit_columns[i];
            std::vector<std::uint8_t> value;
            bool constant = false;
            if (cv.kind == ColumnValues::Kind::FixedWidth) {
                const auto bpv = lance_logical_type_value_bytes(pf->logical_type);
                // A fixed-width constant is stored INLINE in the page descriptor, and Lance allows at
                // most 32 bytes there (ConstantLayout.inline_value). Wider values -- a 200-byte
                // fixed_size_binary, a vector -- stay on the flat path.
                if (bpv != 0U && bpv <= kMaxInlineConstantBytes && cv.fixed_size() >= bpv &&
                    cv.fixed_size() % bpv == 0U) {
                    // A buffer is all-one-value iff it equals itself shifted by one element, so ONE
                    // overlapped memcmp over the whole column replaces the previous
                    // one-libc-call-per-row loop (memcmp only reads, so overlap is fine; a 1-row
                    // column compares 0 bytes and is correctly constant).
                    constant = std::memcmp(cv.fixed_data(), cv.fixed_data() + bpv,
                                           cv.fixed_size() - bpv) == 0;
                    if (constant) {
                        value.assign(cv.fixed_data(), cv.fixed_data() + bpv);
                    }
                }
            } else if (cv.kind == ColumnValues::Kind::VariableWidth) {
                constant = encoding_choice::variable_column_constant_value(cv, value);
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
            // Floats take the same decisions on their raw bits (W1s: pylance dictionary-encodes, RLEs and bitpacks
            // float columns with the integer rules); a float with its own codec or the writer-wide bss-zstd keeps that layout.
            if (cv.kind == ColumnValues::Kind::FixedWidth &&
                (lance_logical_type_is_bitpackable_integer(pf->logical_type) ||
                 lance_logical_type_is_float(pf->logical_type))) {
                const auto bpv = lance_logical_type_value_bytes(pf->logical_type);
                const bool is_float = lance_logical_type_is_float(pf->logical_type);
                // A 64-bit dictionary (W1p) is decided before RLE, as pylance does: a dictionary column's indices are
                // RLE'd when they have runs (W1p2, in fixed_column_dict_beneficial), and a column without a dictionary
                // is RLE'd or not as before. A column with its own lance-encoding:compression keeps its layout (the
                // dictionary is not written under a codec yet).
                bool own_codec = false;
                for (const auto& original : state->schema_mapping.fields) {
                    if (original.id == pf->id) {
                        own_codec = original.metadata.find("lance-encoding:compression") != original.metadata.end();
                        break;
                    }
                }
                if (is_float) {
                    bool bss_tagged = false;
                    for (const auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            const auto tag_it = field.metadata.find("nanolance:packing");
                            bss_tagged = tag_it != field.metadata.end() && tag_it->second == "bss-zstd";
                            break;
                        }
                    }
                    if (own_codec || bss_tagged) {
                        continue;
                    }
                }
                if (bpv == 8U && !own_codec && encoding_choice::fixed_column_dict_beneficial(cv, enc_options)) {
                    for (auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            field.metadata["nanolance:packing"] = "dict";
                            field.metadata.erase("lance-encoding:compression");
                            break;
                        }
                    }
                } else if (encoding_choice::fixed_column_rle_plan(cv, bpv, enc_options)) {
                    for (auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            field.metadata["nanolance:packing"] = "rle";
                            break;
                        }
                    }
                } else if (is_float) {
                    // A float is bitpacked only where the integer estimate beats raw (sign bit clear: uniform
                    // [0, 1) values, 63 bits); signed floats stay flat, as in Lance's estimate.
                    if (encoding_choice::bitpack_beats_flat(cv, bpv)) {
                        for (auto& field : disk_schema.fields) {
                            if (field.id == pf->id) {
                                field.metadata["nanolance:packing"] = "bitpack";
                                break;
                            }
                        }
                    }
                } else if (!encoding_choice::bitpack_beats_flat(cv, bpv)) {
                    // Incompressible integers (hashes, ids, random keys): drop the bitpack tag so the
                    // column writes flat pages, which is both smaller and cheaper to read.
                    for (auto& field : disk_schema.fields) {
                        if (field.id == pf->id) {
                            field.metadata.erase("nanolance:packing");
                            break;
                        }
                    }
                }
            } else if (cv.kind == ColumnValues::Kind::VariableWidth &&
                       encoding_choice::variable_column_dict_rle_beneficial(cv, enc_options)) {
                // Low-cardinality run-length string column -> dictionary + RLE'd indices.
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "dict-rle";
                        field.metadata.erase("lance-encoding:compression");
                        break;
                    }
                }
            } else if (cv.kind == ColumnValues::Kind::VariableWidth &&
                       encoding_choice::variable_column_dict_beneficial(cv, enc_options)) {
                // Scattered low-cardinality strings -> structural dictionary + indices. The indices take the
                // caller's lance-encoding:compression layout (write_dict_column). The writer-wide compression flag
                // tagged every string column zstd above; that tag is not the caller's, so it is replaced by the
                // caller's own option (from the schema as given) or dropped.
                std::string own_codec;
                bool has_own_codec = false;
                for (const auto& original : state->schema_mapping.fields) {
                    if (original.id == pf->id) {
                        if (const auto it = original.metadata.find("lance-encoding:compression");
                            it != original.metadata.end()) {
                            own_codec = it->second;
                            has_own_codec = true;
                        }
                        break;
                    }
                }
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "dict";
                        if (has_own_codec) {
                            field.metadata["lance-encoding:compression"] = own_codec;
                        } else {
                            field.metadata.erase("lance-encoding:compression");
                        }
                        break;
                    }
                }
            } else if (cv.kind == ColumnValues::Kind::VariableWidth &&
                       (pf->logical_type == "utf8" || pf->logical_type == "large_utf8")) {
                // Everything else a string column can be: FSST candidate (roadmap F2). The data file
                // writer trains the table and still writes plain pages where it does not pay.
                for (auto& field : disk_schema.fields) {
                    if (field.id == pf->id) {
                        field.metadata["nanolance:packing"] = "fsst";
                        break;
                    }
                }
            }
        }
    }

    DataFileResult data_file;
    // A file a progress tracker began keeps its name.
    const auto data_file_name = state->open_file_name.empty() ? new_data_file_name() : state->open_file_name;
    state->open_file_name.clear();
    LanceFileExtras file_extras;
    file_extras.blob_pack_file_size = state->blob_pack_file_size;
    file_extras.schema_metadata = state->schema_metadata;  // Lance writes it into every data file
    file_extras.global_buffers = state->global_buffers;
    std::optional<std::uint32_t> base_id;
    if (!state->fragment_targets.empty()) {
        const auto& target = state->fragment_targets[state->fragments_written % state->fragment_targets.size()];
        file_extras.path = target.dir / data_file_name;
        if (target.base_id != 0U) {
            base_id = target.base_id;
        }
    }
    state->fragments_written++;
    if (!write_lance_data_file(state->dataset_path,
                                           data_file_name,
                                           disk_schema,
                                           commit_columns,
                                           state->pending_rows,
                                           state->compression_level,
                                           state->compression,
                                           data_file,
                                           writer_error,
                                           // A memory budget means resident bytes matter more
                                           // than speed: stream each column to the file.
                                           state->max_pending_bytes == 0U, &file_extras)) {
        return fail(WriteStatus::IoError, writer_error);
    }

    if (state->stage) {
        state->staged.push_back(NewFragment{data_file, state->pending_rows, base_id});
        state->staged_schema = disk_schema;
        if (state->file_events) {
            state->file_events(2, data_file_name, state->pending_rows);
        }
    } else {
        std::uint64_t version = 0;
        CommitExtras extras;
        extras.schema_metadata = is_append ? nullptr : &state->schema_metadata;
        extras.stable_row_ids = state->stable_row_ids;
        extras.initial_bases = state->initial_bases.empty() ? nullptr : &state->initial_bases;
        if (!commit_dataset_version(state->dataset_path, disk_schema,
                                                {NewFragment{data_file, state->pending_rows, base_id}},
                                                is_append ? CommitMode::Append
                                                          : CommitMode::Overwrite,
                                                version, writer_error, extras)) {
            return fail(WriteStatus::IoError, writer_error);
        }
        state->append_only_commits = true;
    }
    state->pending_batches = 0;
    state->pending_rows = 0;
    reset_column_values(*state);
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::finish(int mode, uint64_t* version_out) {
    auto* state = state_.get();
    if (state->closed) {
        return fail(WriteStatus::InvalidState, "writer is already closed");
    }
    if (!state->stage) {
        return fail(WriteStatus::InvalidState, "finish needs a writer opened with stage_fragments");
    }
    CommitMode commit_mode;
    switch (mode) {
        case kCommitCreate: commit_mode = CommitMode::Create; break;
        case kCommitAppend: commit_mode = CommitMode::Append; break;
        case kCommitOverwrite: commit_mode = CommitMode::Overwrite; break;
        default: return fail(WriteStatus::InvalidArgument, "unknown commit mode");
    }
    std::error_code ec;
    std::string error;
    if (commit_mode == CommitMode::Create &&
        highest_manifest_version(state->dataset_path, error) != 0U) {
        return fail(WriteStatus::InvalidState,
                         "Dataset already exists: " + state->dataset_path.string());
    }
    if (state->pending_rows != 0U) {
        const auto rc = commit_pending(commit_mode == CommitMode::Append);
        if (rc != WriteStatus::Ok) {
            return rc;
        }
    }
    if (!state->has_schema) {
        return fail(WriteStatus::InvalidState, "no schema: write at least one batch");
    }
    const auto& mapping = state->staged.empty() ? state->schema_mapping : state->staged_schema;
    std::uint64_t version = 0;
    CommitExtras extras;
    extras.schema_metadata = commit_mode == CommitMode::Append ? nullptr : &state->schema_metadata;
    extras.initial_config = state->initial_config;
    extras.transaction_properties = state->transaction_properties;
    extras.stable_row_ids = state->stable_row_ids;
    extras.initial_bases = state->initial_bases.empty() ? nullptr : &state->initial_bases;
    if (!io::is_object_store_output(state->dataset_path.string())) {
        std::filesystem::create_directories(state->dataset_path / "data", ec);
    }
    if (!commit_dataset_version(state->dataset_path, mapping, state->staged, commit_mode, version, error,
                                            extras)) {
        return fail(WriteStatus::IoError, error);
    }
    state->staged.clear();
    if (version_out != nullptr) {
        *version_out = version;
    }
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_initial_config(const char* key, const char* value) {
    auto* state = state_.get();
    if (key == nullptr || value == nullptr || key[0] == '\0') {
        return fail(WriteStatus::InvalidArgument, "config key and value are required");
    }
    state->initial_config[key] = value;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::add_global_buffer(const uint8_t* data, size_t size) {
    auto* state = state_.get();
    if (data == nullptr && size != 0U) {
        return fail(WriteStatus::InvalidArgument, "a global buffer needs its bytes");
    }
    state->global_buffers.emplace_back(data, data + size);
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_schema_metadata_entry(const char* key, const uint8_t* value,
                                                size_t size) {
    auto* state = state_.get();
    if (key == nullptr || key[0] == '\0' || (value == nullptr && size != 0U)) {
        return fail(WriteStatus::InvalidArgument, "schema metadata needs a key and a value");
    }
    state->schema_metadata[key] = std::vector<std::uint8_t>(value, value + size);
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_stable_row_ids(int enable) {
    auto* state = state_.get();
    state->stable_row_ids = enable != 0;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_initial_bases(size_t count, const char* const* paths,
                                        const char* const* names, const int* is_dataset_root) {
    auto* state = state_.get();
    if (count > 0 && paths == nullptr) {
        return fail(WriteStatus::InvalidArgument, "base paths are required");
    }
    std::vector<BasePath> bases;
    for (size_t i = 0; i < count; ++i) {
        if (paths[i] == nullptr || paths[i][0] == '\0') {
            return fail(WriteStatus::InvalidArgument, "a base path is empty");
        }
        BasePath base;
        base.id = static_cast<std::uint32_t>(i + 1U);
        base.path = paths[i];
        if (names != nullptr && names[i] != nullptr) {
            base.name = names[i];
            base.has_name = true;
        }
        base.is_dataset_root = is_dataset_root != nullptr && is_dataset_root[i] != 0;
        bases.push_back(std::move(base));
    }
    state->initial_bases = std::move(bases);
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_fragment_targets(size_t count, const char* const* dirs,
                                           const uint32_t* base_ids) {
    auto* state = state_.get();
    if (count > 0 && (dirs == nullptr || base_ids == nullptr)) {
        return fail(WriteStatus::InvalidArgument, "fragment targets need directories and base ids");
    }
    std::vector<State::FragmentTarget> targets;
    for (size_t i = 0; i < count; ++i) {
        if (dirs[i] == nullptr || dirs[i][0] == '\0') {
            return fail(WriteStatus::InvalidArgument, "a fragment target directory is empty");
        }
        targets.push_back(State::FragmentTarget{std::filesystem::path(dirs[i]), base_ids[i]});
    }
    state->fragment_targets = std::move(targets);
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_blob_pack_file_size(uint64_t bytes) {
    auto* state = state_.get();
    state->blob_pack_file_size = bytes;
    error_.clear();
    return WriteStatus::Ok;
}

WriteStatus WriteSession::set_transaction_property(const char* key, const char* value) {
    auto* state = state_.get();
    if (key == nullptr || value == nullptr) {
        return fail(WriteStatus::InvalidArgument, "property key and value are required");
    }
    state->transaction_properties[key] = value;
    error_.clear();
    return WriteStatus::Ok;
}

std::uint64_t WriteSession::pending_batches() const {
    return state_ == nullptr ? 0U : state_->pending_batches;
}

std::uint64_t WriteSession::pending_rows() const {
    return state_ == nullptr ? 0U : state_->pending_rows;
}

std::uint64_t WriteSession::pending_bytes() const {
    return state_ == nullptr ? 0U : held_bytes(*state_);
}

bool WriteSession::set_field_id_base(std::int32_t first_id, std::string& error) {
    auto* state = state_.get();
    if (state == nullptr || state->has_schema) {
        error = "the field id base must be set on a new writer before its first batch";
        return false;
    }
    state->field_id_base = first_id;
    return true;
}

bool WriteSession::project_append(const std::vector<std::string>& keep, std::string& error) {
    auto* state = state_.get();
    if (state == nullptr || !state->append_only_commits || !state->has_schema || state->pending_batches != 0U ||
        !state->staged.empty()) {
        error = "only a new append writer can write part of the schema";
        return false;
    }
    // A field is kept when its top-level ancestor is; parents come before their children.
    std::map<std::int32_t, bool> kept;
    LanceSchemaMapping projected;
    for (const auto& f : state->schema_mapping.fields) {
        const bool keep_it = f.parent_id < 0 ? std::find(keep.begin(), keep.end(), f.name) != keep.end()
                                             : kept[f.parent_id];
        kept[f.id] = keep_it;
        if (keep_it) {
            projected.fields.push_back(f);
        }
    }
    for (const auto& name : keep) {
        const bool found = std::any_of(projected.fields.begin(), projected.fields.end(), [&](const LanceField& f) {
            return f.parent_id < 0 && f.name == name;
        });
        if (!found) {
            error = "no column '" + name + "' in the dataset";
            return false;
        }
    }
    renumber_columns_for_one_file(projected);
    state->schema_mapping = std::move(projected);
    reset_column_values(*state);
    return true;
}

bool WriteSession::set_file_events(std::function<void(int, const std::string&, std::uint64_t)> events,
                            std::string& error) {
    auto* state = state_.get();
    if (state == nullptr || !state->stage) {
        error = "not a staged writer";
        return false;
    }
    state->file_events = std::move(events);
    return true;
}

bool WriteSession::take_staged(std::vector<NewFragment>& out, LanceSchemaMapping& mapping,
                        std::string& error, bool keep_empty) {
    auto* state = state_.get();
    if (state == nullptr || !state->stage) {
        error = "not a staged writer";
        return false;
    }
    if (state->pending_rows != 0U || (keep_empty && state->pending_batches != 0U)) {
        if (commit_pending(state->append_only_commits) != WriteStatus::Ok) {
            error = error_;
            return false;
        }
    }
    out = std::move(state->staged);
    state->staged.clear();
    mapping = out.empty() ? state->schema_mapping : state->staged_schema;
    return true;
}

}  // namespace nano_lance
